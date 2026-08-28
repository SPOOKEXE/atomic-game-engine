#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::Store;

	namespace {
		using nlohmann::json;

		// Whether a relative path stays inside the folder it is relative to.
		//
		// **The one piece of path handling here that is a decision.** A manifest
		// is a file somebody downloaded, and a `main` of `../../../../etc/passwd`
		// is that file reading one outside its plugin. Checked on the *lexical*
		// path rather than on the resolved one, so a folder that does not exist
		// yet is still refused rather than passing because nothing resolved.
		bool StaysInside(const std::string &relative) {
			if (relative.empty()) {
				return false;
			}

			const std::filesystem::path path(relative);
			if (path.is_absolute()) {
				return false;
			}

			int depth = 0;
			for (const std::filesystem::path &part : path) {
				if (part == "..") {
					depth--;
					if (depth < 0) {
						return false;
					}
				} else if (part != "." && !part.empty()) {
					depth++;
				}
			}
			return true;
		}

		bool ReadWhole(const std::filesystem::path &path, std::string &out) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		// The language a plugin's entry file is written in.
		//
		// **From the extension, which is the same rule the Rojo sync uses one
		// file over.** A `.js` plugin and a `.luau` one are the same surface in
		// two languages, and the engine already runs both.
		engine::script::Language LanguageOf(const std::filesystem::path &main) {
			const std::string extension = main.extension().string();
			return extension == ".js" || extension == ".mjs" ? engine::script::Language::JavaScript
															 : engine::script::Language::Luau;
		}
	}

	bool RegisterSelectionComponent() {
		// A tag: no fields, matched by a query and nothing else. Registering it
		// twice agrees rather than conflicting, which is what makes this
		// callable from wherever the editor happens to reach first.
		const engine::ecs::Schemas::Result result = engine::ecs::Schemas::Register(SELECTED_COMPONENT, {});

		if (result.Why != engine::ecs::Schemas::Status::Ok) {
			ENGINE_WARN("plugins: could not register {}", SELECTED_COMPONENT);
			return false;
		}
		return true;
	}

	std::filesystem::path PluginRoot() {
		return ConfigPath("plugins");
	}

	bool ParsePluginManifest(std::string_view json_, PluginManifest &out, std::string &error) {
		const json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}

		const auto name = document.find("name");
		if (name == document.end() || !name->is_string() || name->get<std::string>().empty()) {
			// **A name is required and the rest is not.** Everything else has a
			// sensible default; a plugin with no name is a row in a list nobody
			// can identify, and a list of "(unnamed)" is worse than a refusal.
			error = "no 'name' - this is not a plugin manifest";
			return false;
		}

		out.Name = name->get<std::string>();

		if (const auto found = document.find("description"); found != document.end() && found->is_string()) {
			out.Description = found->get<std::string>();
		}
		if (const auto found = document.find("main"); found != document.end() && found->is_string()) {
			out.Main = found->get<std::string>();
		}
		if (const auto found = document.find("enabled"); found != document.end() && found->is_boolean()) {
			out.Enabled = found->get<bool>();
		}

		if (!StaysInside(out.Main)) {
			error = "'main' has to name a file inside the plugin's own folder";
			return false;
		}
		return true;
	}

	std::vector<LoadedPlugin> DiscoverPlugins(const std::filesystem::path &root) {
		std::vector<LoadedPlugin> found;

		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			// No plugins folder is the ordinary state of a fresh install, not an
			// error to report every time the editor starts.
			return found;
		}

		std::vector<std::filesystem::path> folders;
		for (const auto &entry : std::filesystem::directory_iterator(root, failed)) {
			if (entry.is_directory(failed)) {
				folders.push_back(entry.path());
			}
		}
		if (failed) {
			return found;
		}

		// Sorted, because a directory walk is not ordered and plugins run in
		// this order - one may build on what another left in the world.
		std::sort(folders.begin(), folders.end());

		for (const std::filesystem::path &folder : folders) {
			const std::filesystem::path manifest = folder / "plugin.json";
			if (!std::filesystem::is_regular_file(manifest, failed)) {
				// Somebody's notes, not a broken plugin.
				continue;
			}

			LoadedPlugin plugin;
			plugin.Root = folder;

			std::string text;
			if (!ReadWhole(manifest, text)) {
				plugin.Error = "could not read " + manifest.string();
				plugin.Manifest.Name = folder.filename().string();
				found.push_back(std::move(plugin));
				continue;
			}

			if (!ParsePluginManifest(text, plugin.Manifest, plugin.Error)) {
				// **Returned rather than skipped.** A folder with no manifest is
				// not a plugin; a folder whose manifest is broken is one, and
				// saying so is the whole point of walking it.
				plugin.Manifest.Name = folder.filename().string();
				found.push_back(std::move(plugin));
				continue;
			}

			found.push_back(std::move(plugin));
		}

		return found;
	}

	void StartPlugins(
		std::vector<LoadedPlugin> &plugins,
		engine::ecs::Store &store,
		const std::function<std::unique_ptr<engine::script::HostSurface>(LoadedPlugin &)> &surface
	) {
		for (LoadedPlugin &plugin : plugins) {
			plugin.Running = false;
			plugin.Faults = 0;

			if (!plugin.Error.empty()) {
				// Broken at discovery. Left as it is, so the reason survives to
				// the panel that shows it.
				continue;
			}
			if (!plugin.Manifest.Enabled) {
				plugin.Error = "switched off";
				continue;
			}

			const std::filesystem::path main = plugin.Root / plugin.Manifest.Main;

			std::error_code missing;
			if (!std::filesystem::is_regular_file(main, missing)) {
				plugin.Error = "no such file: " + plugin.Manifest.Main;
				continue;
			}

			std::string source;
			if (!ReadWhole(main, source)) {
				plugin.Error = "could not read " + plugin.Manifest.Main;
				continue;
			}

			// **A runtime each**, so one plugin cannot see another's globals or
			// spend another's step budget. The role says this is a studio, which
			// is what `RunService:IsStudio()` reads - a plugin is not a game
			// server and should not think it is.
			engine::script::RuntimeLimits limits;
			limits.Role.Server = false;
			limits.Role.Client = false;
			limits.Role.Studio = true;
			limits.Origin = engine::script::ScriptOrigin::Plugin;

			plugin.Vm = engine::script::MakeRuntime(store, LanguageOf(main), limits);
			if (plugin.Vm == nullptr) {
				plugin.Error = "could not start a runtime";
				continue;
			}

			// **Before the entry script**, because a plugin's top level is where
			// it creates its toolbar - a host set afterwards would be a global
			// the chunk had already failed to find.
			//
			// The surface holds a reference to `plugin`, so it is stored beside
			// the runtime that points at it and both go away together.
			if (surface) {
				plugin.Surface = surface(plugin);
				if (plugin.Surface != nullptr) {
					plugin.Vm->SetHost(plugin.Surface.get());
				}
			}

			if (!plugin.Vm->Run(source, plugin.Manifest.Name)) {
				// **The whole error, because a plugin author is the person
				// reading it.** A truncated message costs them the line number,
				// which is the only part that matters.
				plugin.Error = plugin.Vm->LastError();

				// The surface goes with the runtime that pointed at it, and in
				// that order: a host outliving its VM is a pointer nothing owns.
				plugin.Vm.reset();
				plugin.Surface.reset();
				continue;
			}

			plugin.Running = true;
			plugin.Error.clear();
			ENGINE_INFO("plugin '{}' started", plugin.Manifest.Name);
		}
	}

	size_t BeatPlugins(std::vector<LoadedPlugin> &plugins, float delta) {
		size_t beaten = 0;

		for (LoadedPlugin &plugin : plugins) {
			if (!plugin.Running || plugin.Vm == nullptr) {
				continue;
			}

			beaten++;
			if (plugin.Vm->Heartbeat(delta)) {
				continue;
			}

			plugin.Faults++;
			plugin.Error = plugin.Vm->LastError();

			// **Switched off rather than logged every frame.** A plugin whose
			// heartbeat raises does it again next frame and every frame after,
			// which is a log nobody can read and a profile nobody can use.
			if (plugin.Faults >= PLUGIN_FAULT_LIMIT) {
				plugin.Running = false;
				ENGINE_ERROR(
					"plugin '{}' switched off after {} fault(s): {}",
					plugin.Manifest.Name,
					plugin.Faults,
					plugin.Error
				);
			}
		}

		return beaten;
	}

	// --- the editor's half ---------------------------------------------------

	void Editor::LoadPlugins() {
		// Every plugin holds a `Store &`. Stopping them before the discovery
		// replaces the list is what stops a runtime outliving the world it was
		// started against.
		Plugins.clear();
		PublishedSelection.clear();

		if (Universe == nullptr || !Active.IsValid()) {
			return;
		}

		RegisterSelectionComponent();
		Plugins = DiscoverPlugins(PluginRoot());
		if (Plugins.empty()) {
			return;
		}

		Universe->Enter(Active, [this](Store &store) {
			StartPlugins(Plugins, store, [this](LoadedPlugin &plugin) {
				return MakePluginSurface(*this, plugin);
			});
		});

		size_t running = 0;
		for (const LoadedPlugin &plugin : Plugins) {
			if (plugin.Running) {
				running++;
				continue;
			}
			// Named on the way in rather than only in the panel, because the
			// output pane is where somebody is already looking when a plugin
			// they just installed does nothing.
			Say("plugin '" + plugin.Manifest.Name + "': " + plugin.Error, engine::core::LogLevel::Warning);
		}

		Say("plugins: " + std::to_string(running) + " of " + std::to_string(Plugins.size()) + " running");
	}

	std::string Editor::ActiveWorldName() const {
		if (Universe == nullptr || !Active.IsValid()) {
			return {};
		}
		return std::string(Universe->NameOf(Active).Text());
	}

	bool Editor::HasActiveWorld() const {
		return Universe != nullptr && Active.IsValid();
	}

	void Editor::WithSelectionWorld(const std::function<void(engine::ecs::Store &)> &body) {
		if (Universe == nullptr || !SelectionWorld.IsValid()) {
			// **A no-op rather than a refusal**, because "nothing is open" is an
			// ordinary state for a plugin's heartbeat to run in and a plugin
			// should not have to guard every call against it.
			return;
		}
		Universe->Enter(SelectionWorld, body);
	}

	void Editor::PublishSelection() {
		if (Universe == nullptr || !SelectionWorld.IsValid()) {
			return;
		}

		std::vector<Entity> wanted(Selection.begin(), Selection.end());
		std::sort(wanted.begin(), wanted.end(), [](Entity left, Entity right) { return left.Id < right.Id; });

		// **Nothing written when nothing changed**, which is not a
		// micro-optimisation: a tag written every frame moves
		// `Store::ChangeVersion` every frame, and `physics::SyncBroadphase`
		// reads that counter to decide whether static geometry moved. Publishing
		// unconditionally would rebuild the static index every tick, forever -
		// the exact failure `physics/AGENTS.md` names.
		if (wanted == PublishedSelection) {
			return;
		}

		const engine::ecs::ComponentId id =
			engine::ecs::Components::Find(engine::core::Name(std::string(SELECTED_COMPONENT).c_str()));
		if (!id.IsValid()) {
			return;
		}

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity entity : PublishedSelection) {
				if (store.Alive(entity)) {
					store.RemoveComponent(entity, id);
				}
			}
			for (const Entity entity : wanted) {
				if (store.Alive(entity)) {
					store.SetComponent(entity, id, nullptr);
				}
			}
		});

		PublishedSelection = std::move(wanted);
	}

	void Editor::PumpPlugins(float delta) {
		ENGINE_PROFILE_CAT("plugins", engine::core::ProfileCategory::Render);

		if (Plugins.empty()) {
			return;
		}

		// **The selection first, so a plugin's heartbeat sees this frame's.** A
		// plugin that read a selection one frame stale would act on what was
		// selected before the click that ran it.
		PublishSelection();

		const size_t beaten = BeatPlugins(Plugins, delta);
		(void)beaten;
	}

	void Editor::InvokePlugin(
		LoadedPlugin &plugin,
		engine::script::HostCallback callback,
		bool drawing,
		engine::script::HostArguments arguments
	) {
		if (!plugin.Running || plugin.Vm == nullptr || !callback.Valid()) {
			return;
		}

		// **The gate is opened around the call and closed after it**, so
		// "am I drawing" is a fact about where a host call came from rather than
		// a promise the plugin makes. A `plugin.Label` from a heartbeat would
		// otherwise draw into whatever window the editor was building.
		if (plugin.Surface != nullptr && drawing) {
			SetPluginDrawing(*plugin.Surface, true);
		}

		const bool ok = plugin.Vm->Invoke(callback, arguments);

		if (plugin.Surface != nullptr && drawing) {
			SetPluginDrawing(*plugin.Surface, false);
		}

		if (ok) {
			return;
		}

		// **A handler that raised is counted with the heartbeat's faults**, and
		// for the same reason: a render callback that throws does it every frame
		// its window is open, which is a log nobody can read.
		plugin.Faults++;
		plugin.Error = plugin.Vm->LastError();

		if (plugin.Faults >= PLUGIN_FAULT_LIMIT) {
			plugin.Running = false;
			Say("plugin '" + plugin.Manifest.Name + "' switched off: " + plugin.Error,
				engine::core::LogLevel::Error);
		}
	}

	void Editor::DrawPluginWidgets() {
		for (LoadedPlugin &plugin : Plugins) {
			if (!plugin.Running) {
				continue;
			}

			for (PluginWidget &widget : plugin.Widgets) {
				if (!widget.Open) {
					continue;
				}

				// **The plugin's name in the id and not in the title.** Two
				// plugins may both call a panel "Settings", and ImGui keys a
				// window on its whole label - so the id suffix keeps them apart
				// without putting a prefix in front of what a person reads.
				const std::string label =
					widget.Title + "###plugin." + plugin.Manifest.Name + "." + widget.Title;

				// **Around `Begin` and `End`, not inside them.** A window's
				// background is read at `Begin`, so colours pushed within the
				// window would tint everything in it except the window itself -
				// which reads as a bug in the theme rather than as a widget that
				// was coloured wrong. Same bracket the editor's own panels get
				// from `Editor::Skinned`.
				const engine::ui::ScopedColours skin(widget.Colours);

				if (ImGui::Begin(label.c_str(), &widget.Open)) {
					InvokePlugin(plugin, widget.Render, true);
				}
				ImGui::End();
			}
		}
	}

	void Editor::DrawPluginTools() {
		// **Reload first, and always present.** It is the one control that is
		// useful when *nothing* is running, which is exactly when somebody is on
		// this tab - a plugin they have just written and just fixed.
		if (ImGui::Button("Reload", ImVec2(84.0f, 0.0f))) {
			LoadPlugins();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Restarts every plugin against the active scene");
		}

		ImGui::SameLine();
		if (ImGui::Button("Manage", ImVec2(84.0f, 0.0f))) {
			ShowPlugins = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", PluginRoot().string().c_str());
		}

		size_t drawn = 0;
		for (LoadedPlugin &plugin : Plugins) {
			// **A stopped plugin's toolbar is not drawn.** Its buttons call
			// handlers in a runtime that has been torn down, and a button that
			// looks live and cannot run is worse than one that is not there -
			// the Manage panel is where a stopped plugin is explained.
			if (!plugin.Running) {
				continue;
			}

			for (size_t bar = 0; bar < plugin.Toolbars.size(); bar++) {
				PluginToolbar &toolbar = plugin.Toolbars[bar];

				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();

				// The toolbar's own name, dimmed, so a row of buttons from three
				// plugins can be read as three groups.
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(toolbar.Name.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", plugin.Manifest.Name.c_str());
				}
				drawn++;

				for (size_t at = 0; at < toolbar.Buttons.size(); at++) {
					PluginButton &button = toolbar.Buttons[at];
					ImGui::SameLine();

					// The id keeps two buttons of one name apart; the label is
					// what somebody reads.
					const std::string id = button.Name + "###plugin." + plugin.Manifest.Name + "." +
										   std::to_string(bar) + "." + std::to_string(at);

					const bool pressed = button.Active
											 ? ImGui::Selectable(id.c_str(), true, 0, ImVec2(92.0f, 0.0f))
											 : ImGui::Button(id.c_str(), ImVec2(92.0f, 0.0f));

					if (!button.Tooltip.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", button.Tooltip.c_str());
					}

					if (pressed) {
						// **Not drawing.** A click handler runs the plugin's own
						// code, which may open a widget or change the selection;
						// letting it draw here would put its widgets on the
						// toolbar rather than in their own window.
						InvokePlugin(plugin, button.OnClick, false);
					}
				}
			}
		}

		if (drawn > 0) {
			return;
		}

		// **Says which of the two nothings it is.** No plugins installed and
		// plugins installed that asked for no toolbar are different situations,
		// and a single "nothing here" would send somebody looking in the wrong
		// place.
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::TextDisabled(
			Plugins.empty() ? "no plugins installed - Manage says where they go"
							: "nothing installed a toolbar - Manage says what is running"
		);
	}

	void Editor::DrawPlugins() {
		if (!ShowPlugins) {
			return;
		}

		if (!ImGui::Begin("Plugins", &ShowPlugins)) {
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("%s", PluginRoot().string().c_str());
		ImGui::Separator();

		if (ImGui::Button("Reload", ImVec2(96.0f, 0.0f))) {
			LoadPlugins();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("A reload restarts every plugin against the active scene.");

		ImGui::Spacing();

		// **The toolbars are not here any more; they are the ribbon's Plugins
		// tab.** A toolbar button is pressed while working and this panel is
		// opened when something is wrong, so the two belong in different places
		// - and drawing them in both would be two sets of the same buttons that
		// could disagree about which is active. `DrawPluginTools` is the one.

		if (Plugins.empty()) {
			ImGui::TextDisabled("nothing installed");
			ImGui::TextWrapped(
				"A plugin is a folder holding a plugin.json and a script. It runs "
				"against the scene you are editing, with the same surface a game "
				"script gets - including World, the ECS underneath."
			);
			ImGui::End();
			return;
		}

		if (ImGui::BeginTable("plugins", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("What it is doing", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (LoadedPlugin &plugin : Plugins) {
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(plugin.Manifest.Name.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", plugin.Root.string().c_str());
				}

				ImGui::TableNextColumn();
				if (plugin.Running) {
					ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "running");
				} else if (plugin.Faults > 0) {
					ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "faulted");
				} else {
					ImGui::TextDisabled("stopped");
				}

				ImGui::TableNextColumn();

				// A panel somebody closed is reopened from here, which is the
				// one thing they cannot do from the plugin's own surface.
				for (PluginWidget &widget : plugin.Widgets) {
					const std::string id =
						widget.Title + "###toggle." + plugin.Manifest.Name + "." + widget.Title;
					ImGui::Checkbox(id.c_str(), &widget.Open);
					ImGui::SameLine();
				}

				// **The error where the description would be**, because a
				// plugin that is not running is one somebody is trying to fix
				// and the description is not what they need.
				if (!plugin.Error.empty()) {
					ImGui::TextWrapped("%s", plugin.Error.c_str());
				} else if (!plugin.Manifest.Description.empty()) {
					ImGui::TextWrapped("%s", plugin.Manifest.Description.c_str());
				} else {
					ImGui::TextDisabled("-");
				}
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}

	// --- ChangeHistoryService's events ----------------------------------------
	//
	// **One watcher on the log, fanned out here.** `CommandLog` has a single
	// seam because a log that knew what a plugin was would be a log that knows
	// what an editor is. Deciding who hears about a waypoint is this class's
	// job: the plugins, and - when one is open - the team-create edit stream.
	//
	// **Every running plugin hears every event, including its own.** Roblox does
	// the same, and the alternative needs the log to record which plugin made a
	// change, which it deliberately does not: an edit is an edit whoever asked
	// for it, and a plugin that reacts to its own undo is a plugin that asked to.

	void Editor::InstallHistoryWatcher() {
		if (Commands == nullptr) {
			return;
		}

		CommandLog::Watcher watcher;

		watcher.Undone = [this](std::string_view waypoint) {
			const engine::script::HostValue name = engine::script::HostValue::Of(waypoint);
			for (LoadedPlugin &plugin : Plugins) {
				InvokePlugin(plugin, plugin.OnUndo, false, engine::script::HostArguments(&name, 1));
			}
		};

		watcher.Redone = [this](std::string_view waypoint) {
			const engine::script::HostValue name = engine::script::HostValue::Of(waypoint);
			for (LoadedPlugin &plugin : Plugins) {
				InvokePlugin(plugin, plugin.OnRedo, false, engine::script::HostArguments(&name, 1));
			}
		};

		watcher.RecordingStarted = [this](const Recording &recording) {
			// Roblox passes name and displayName, in that order.
			const engine::script::HostValue arguments[] = {
				engine::script::HostValue::Of(std::string_view(recording.Name)),
				engine::script::HostValue::Of(std::string_view(recording.DisplayName)),
			};
			for (LoadedPlugin &plugin : Plugins) {
				InvokePlugin(
					plugin, plugin.OnRecordingStarted, false, engine::script::HostArguments(arguments, 2)
				);
			}
		};

		watcher.RecordingFinished = [this](const Recording &recording, FinishOperation operation) {
			// Name, displayName, identifier, operation - Roblox's order. The
			// operation crosses as its member's name, which is how an
			// `EnumItem` crosses in the other direction too.
			const engine::script::HostValue arguments[] = {
				engine::script::HostValue::Of(std::string_view(recording.Name)),
				engine::script::HostValue::Of(std::string_view(recording.DisplayName)),
				engine::script::HostValue::Of(std::string_view(recording.Identifier)),
				engine::script::HostValue::Of(std::string_view(Describe(operation))),
			};
			for (LoadedPlugin &plugin : Plugins) {
				InvokePlugin(
					plugin, plugin.OnRecordingFinished, false, engine::script::HostArguments(arguments, 4)
				);
			}
		};

		watcher.Committed = [this](uint64_t waypoint, std::span<const Command> group) {
			// **One waypoint, whole, in the order it was made.** Team create's
			// end of it: a peer that applied half of a group would show a state
			// the author never saw. Does nothing when no session is open, which
			// is what keeps an editor that never opens the panel from paying
			// for any of this.
			if (Team != nullptr) {
				Team->PublishEdits(waypoint, group, engine::core::Clock::Seconds());
			}
		};

		Commands->Watch(std::move(watcher));
	}
}
