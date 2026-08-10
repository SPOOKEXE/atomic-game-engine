#include <studio/Plugins.hpp>

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <engine/ecs/Schema.hpp>
#include <studio/Config.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

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
		const engine::ecs::Schemas::Result result =
			engine::ecs::Schemas::Register(SELECTED_COMPONENT, {});

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
			error = "no 'name' — this is not a plugin manifest";
			return false;
		}

		out.Name = name->get<std::string>();

		if (const auto found = document.find("description");
			found != document.end() && found->is_string()) {
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
		// this order — one may build on what another left in the world.
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

	void StartPlugins(std::vector<LoadedPlugin> &plugins, engine::ecs::Store &store) {
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
			// is what `RunService:IsStudio()` reads — a plugin is not a game
			// server and should not think it is.
			engine::script::RuntimeLimits limits;
			limits.Role.Server = false;
			limits.Role.Client = false;
			limits.Role.Studio = true;

			plugin.Vm = engine::script::MakeRuntime(store, LanguageOf(main), limits);
			if (plugin.Vm == nullptr) {
				plugin.Error = "could not start a runtime";
				continue;
			}

			if (!plugin.Vm->Run(source, plugin.Manifest.Name)) {
				// **The whole error, because a plugin author is the person
				// reading it.** A truncated message costs them the line number,
				// which is the only part that matters.
				plugin.Error = plugin.Vm->LastError();
				plugin.Vm.reset();
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

		Universe->Enter(Active, [this](Store &store) { StartPlugins(Plugins, store); });

		size_t running = 0;
		for (const LoadedPlugin &plugin : Plugins) {
			if (plugin.Running) {
				running++;
				continue;
			}
			// Named on the way in rather than only in the panel, because the
			// output pane is where somebody is already looking when a plugin
			// they just installed does nothing.
			Say("plugin '" + plugin.Manifest.Name + "': " + plugin.Error,
				engine::core::LogLevel::Warning);
		}

		Say("plugins: " + std::to_string(running) + " of " + std::to_string(Plugins.size()) +
			" running");
	}

	void Editor::PublishSelection() {
		if (Universe == nullptr || !SelectionWorld.IsValid()) {
			return;
		}

		std::vector<Entity> wanted(Selection.begin(), Selection.end());
		std::sort(wanted.begin(), wanted.end(), [](Entity left, Entity right) {
			return left.Id < right.Id;
		});

		// **Nothing written when nothing changed**, which is not a
		// micro-optimisation: a tag written every frame moves
		// `Store::ChangeVersion` every frame, and `physics::SyncBroadphase`
		// reads that counter to decide whether static geometry moved. Publishing
		// unconditionally would rebuild the static index every tick, forever —
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
		if (Plugins.empty()) {
			return;
		}

		ENGINE_PROFILE_CAT("plugins", engine::core::ProfileCategory::Render);

		// **The selection first, so a plugin's heartbeat sees this frame's.** A
		// plugin that read a selection one frame stale would act on what was
		// selected before the click that ran it.
		PublishSelection();

		const size_t beaten = BeatPlugins(Plugins, delta);
		(void)beaten;
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

		if (Plugins.empty()) {
			ImGui::TextDisabled("nothing installed");
			ImGui::TextWrapped(
				"A plugin is a folder holding a plugin.json and a script. It runs "
				"against the scene you are editing, with the same surface a game "
				"script gets — including World, the ECS underneath."
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

				// **The error where the description would be**, because a
				// plugin that is not running is one somebody is trying to fix
				// and the description is not what they need.
				if (!plugin.Error.empty()) {
					ImGui::TextWrapped("%s", plugin.Error.c_str());
				} else if (!plugin.Manifest.Description.empty()) {
					ImGui::TextWrapped("%s", plugin.Manifest.Description.c_str());
				} else {
					ImGui::TextDisabled("—");
				}
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
}
