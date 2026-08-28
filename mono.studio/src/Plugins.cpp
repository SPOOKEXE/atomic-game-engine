#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <tuple>

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

		bool WriteWhole(const std::filesystem::path &path, std::string_view text, std::string &error) {
			std::error_code failed;
			if (!path.parent_path().empty()) {
				std::filesystem::create_directories(path.parent_path(), failed);
				if (failed) {
					error = "could not create " + path.parent_path().string();
					return false;
				}
			}

			const std::filesystem::path temporary = path.string() + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out) {
					error = "could not write " + temporary.string();
					return false;
				}
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out) {
					error = "could not finish " + temporary.string();
					return false;
				}
			}

			std::filesystem::rename(temporary, path, failed);
			if (!failed) {
				return true;
			}

			// Windows does not replace an existing file with `rename`. The old
			// document stays valid until the complete temporary file exists.
			std::filesystem::remove(path, failed);
			failed.clear();
			std::filesystem::rename(temporary, path, failed);
			if (failed) {
				error = "could not replace " + path.string();
				return false;
			}
			return true;
		}

		bool LoadPluginState(
			const std::filesystem::path &path,
			std::map<std::string, bool, std::less<>> &enabled,
			std::string &error
		) {
			std::string text;
			if (!ReadWhole(path, text)) {
				error = "could not read " + path.string();
				return false;
			}

			const json document = json::parse(text, nullptr, false);
			if (document.is_discarded() || !document.is_object()) {
				error = "plugin state is not a JSON object";
				return false;
			}

			enabled.clear();
			for (const auto &[id, value] : document.items()) {
				if (value.is_boolean()) {
					enabled[id] = value.get<bool>();
				}
			}
			error.clear();
			return true;
		}

		bool SavePluginState(
			const std::filesystem::path &path,
			const std::map<std::string, bool, std::less<>> &enabled,
			std::string &error
		) {
			json document = json::object();
			for (const auto &[id, value] : enabled) {
				document[id] = value;
			}
			return WriteWhole(path, document.dump(2) + "\n", error);
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

	const char *Describe(PluginDock dock) {
		switch (dock) {
		case PluginDock::Floating:
			return "Floating";
		case PluginDock::Centre:
			return "Centre";
		case PluginDock::Left:
			return "Left";
		case PluginDock::Right:
			return "Right";
		case PluginDock::Bottom:
			return "Bottom";
		}
		return "Floating";
	}

	std::optional<PluginDock> ParsePluginDock(std::string_view text) {
		if (text == "Floating" || text == "floating") {
			return PluginDock::Floating;
		}
		if (text == "Centre" || text == "Center" || text == "centre" || text == "center") {
			return PluginDock::Centre;
		}
		if (text == "Left" || text == "left") {
			return PluginDock::Left;
		}
		if (text == "Right" || text == "right") {
			return PluginDock::Right;
		}
		if (text == "Bottom" || text == "bottom") {
			return PluginDock::Bottom;
		}
		return std::nullopt;
	}

	float ClampPluginToolWidth(float width) {
		if (!std::isfinite(width)) {
			return 92.0f;
		}
		return std::clamp(width, PLUGIN_TOOL_MINIMUM_WIDTH, PLUGIN_TOOL_MAXIMUM_WIDTH);
	}

	std::string PluginIdentity(const LoadedPlugin &plugin) {
		if (!plugin.Manifest.Id.empty()) {
			return plugin.Manifest.Id;
		}
		if (!plugin.Root.filename().empty()) {
			return plugin.Root.filename().string();
		}
		return plugin.Manifest.Name;
	}

	std::string PluginToolbarKey(const LoadedPlugin &plugin, const PluginToolbar &toolbar, size_t index) {
		const std::string pluginId = PluginIdentity(plugin);
		const std::string toolbarId = toolbar.Id.empty() ? std::to_string(index + 1) : toolbar.Id;
		return std::to_string(pluginId.size()) + ":" + pluginId + std::to_string(toolbarId.size()) + ":" +
			   toolbarId;
	}

	std::string PluginToolKey(
		const LoadedPlugin &plugin,
		const PluginToolbar &toolbar,
		size_t toolbarIndex,
		const PluginButton &button,
		size_t itemIndex
	) {
		const std::string toolbarKey = PluginToolbarKey(plugin, toolbar, toolbarIndex);
		const std::string itemId = button.Id.empty() ? std::to_string(itemIndex + 1) : button.Id;
		return toolbarKey + std::to_string(itemId.size()) + ":" + itemId;
	}

	std::vector<ToolbarTabView>
	ComposeToolbar(const std::vector<LoadedPlugin> &plugins, const ToolbarPreferences &preferences) {
		std::vector<ToolbarTabView> tabs;
		std::vector<bool> visible;

		const auto ensureTab = [&](std::string id, std::string name, bool shown) -> size_t {
			for (size_t index = 0; index < tabs.size(); index++) {
				if (tabs[index].Id == id) {
					visible[index] = visible[index] && shown;
					return index;
				}
			}
			tabs.push_back(ToolbarTabView{std::move(id), std::move(name), {}});
			visible.push_back(shown);
			return tabs.size() - 1;
		};

		for (const ToolbarTabPreference &tab : preferences.Tabs) {
			if (!tab.Id.empty() && !tab.Name.empty()) {
				ensureTab(tab.Id, tab.Name, tab.Visible);
			}
		}

		for (size_t pluginIndex = 0; pluginIndex < plugins.size(); pluginIndex++) {
			const LoadedPlugin &plugin = plugins[pluginIndex];
			if (!plugin.Running) {
				continue;
			}

			for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
				const PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
				const std::string defaultTab = PluginToolbarKey(plugin, toolbar, toolbarIndex);
				size_t tabIndex = ensureTab(defaultTab, toolbar.Name, toolbar.Visible);

				for (size_t itemIndex = 0; itemIndex < toolbar.Buttons.size(); itemIndex++) {
					const PluginButton &button = toolbar.Buttons[itemIndex];
					const std::string key = PluginToolKey(plugin, toolbar, toolbarIndex, button, itemIndex);

					const ToolbarItemPreference *preference = nullptr;
					for (const ToolbarItemPreference &candidate : preferences.Items) {
						if (candidate.Key == key) {
							preference = &candidate;
							break;
						}
					}

					const bool itemVisible = preference == nullptr ? button.Visible : preference->Visible;
					if (!itemVisible) {
						continue;
					}

					if (preference != nullptr && !preference->Tab.empty() && preference->Tab != defaultTab) {
						tabIndex = ensureTab(preference->Tab, preference->Tab, true);
					} else {
						tabIndex = ensureTab(defaultTab, toolbar.Name, toolbar.Visible);
					}

					const float width = preference == nullptr ? ClampPluginToolWidth(button.Width)
															  : ClampPluginToolWidth(preference->Width);
					tabs[tabIndex].Items.push_back(
						ToolbarItemLocation{pluginIndex, toolbarIndex, itemIndex, key, width}
					);
				}
			}
		}

		std::vector<ToolbarTabView> composed;
		for (size_t index = 0; index < tabs.size(); index++) {
			if (visible[index] && !tabs[index].Items.empty()) {
				composed.push_back(std::move(tabs[index]));
			}
		}
		return composed;
	}

	bool
	LoadToolbarPreferences(const std::filesystem::path &path, ToolbarPreferences &out, std::string &error) {
		std::string text;
		if (!ReadWhole(path, text)) {
			error = "could not read " + path.string();
			return false;
		}

		const json document = json::parse(text, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "toolbar preferences are not a JSON object";
			return false;
		}

		ToolbarPreferences loaded;
		if (const auto found = document.find("tabs"); found != document.end() && found->is_array()) {
			for (const json &entry : *found) {
				if (!entry.is_object()) {
					continue;
				}
				ToolbarTabPreference tab;
				if (const auto value = entry.find("id"); value != entry.end() && value->is_string()) {
					tab.Id = value->get<std::string>();
				}
				if (const auto value = entry.find("name"); value != entry.end() && value->is_string()) {
					tab.Name = value->get<std::string>();
				}
				if (const auto value = entry.find("visible"); value != entry.end() && value->is_boolean()) {
					tab.Visible = value->get<bool>();
				}
				if (const auto value = entry.find("user"); value != entry.end() && value->is_boolean()) {
					tab.UserCreated = value->get<bool>();
				}
				if (!tab.Id.empty() && !tab.Name.empty()) {
					loaded.Tabs.push_back(std::move(tab));
				}
			}
		}

		if (const auto found = document.find("items"); found != document.end() && found->is_array()) {
			for (const json &entry : *found) {
				if (!entry.is_object()) {
					continue;
				}
				ToolbarItemPreference item;
				if (const auto value = entry.find("key"); value != entry.end() && value->is_string()) {
					item.Key = value->get<std::string>();
				}
				if (const auto value = entry.find("tab"); value != entry.end() && value->is_string()) {
					item.Tab = value->get<std::string>();
				}
				if (const auto value = entry.find("visible"); value != entry.end() && value->is_boolean()) {
					item.Visible = value->get<bool>();
				}
				if (const auto value = entry.find("width"); value != entry.end() && value->is_number()) {
					item.Width = ClampPluginToolWidth(value->get<float>());
				}
				if (!item.Key.empty()) {
					loaded.Items.push_back(std::move(item));
				}
			}
		}

		out = std::move(loaded);
		error.clear();
		return true;
	}

	bool SaveToolbarPreferences(
		const std::filesystem::path &path, const ToolbarPreferences &preferences, std::string &error
	) {
		json document;
		document["tabs"] = json::array();
		for (const ToolbarTabPreference &tab : preferences.Tabs) {
			if (tab.Id.empty() || tab.Name.empty()) {
				continue;
			}
			document["tabs"].push_back(
				{{"id", tab.Id}, {"name", tab.Name}, {"visible", tab.Visible}, {"user", tab.UserCreated}}
			);
		}

		document["items"] = json::array();
		for (const ToolbarItemPreference &item : preferences.Items) {
			if (item.Key.empty()) {
				continue;
			}
			document["items"].push_back(
				{{"key", item.Key},
				 {"tab", item.Tab},
				 {"visible", item.Visible},
				 {"width", ClampPluginToolWidth(item.Width)}}
			);
		}

		return WriteWhole(path, document.dump(2) + "\n", error);
	}

	LoadedPlugin MakeDefaultStudioPlugin() {
		LoadedPlugin plugin;
		plugin.Root = "@builtin/default-studio";
		plugin.Manifest.Name = "Default Studio";
		plugin.Manifest.Description = "The standard Studio toolbar and management surfaces.";
		plugin.Manifest.Id = "atomic.default-studio";
		plugin.Manifest.Version = "1";
		plugin.Manifest.Author = "Atomic Game Engine";
		plugin.Running = true;
		plugin.Builtin = true;

		const auto addToolbar = [&](std::string id, std::string name, auto controls) {
			PluginToolbar toolbar;
			toolbar.Name = std::move(name);
			toolbar.Id = std::move(id);
			for (const auto &[controlId, label, tool] : controls) {
				PluginButton item;
				item.Name = label;
				item.Id = controlId;
				item.Kind = PluginControlKind::Builtin;
				item.Builtin = tool;
				item.Width = PLUGIN_TOOL_MAXIMUM_WIDTH;
				toolbar.Buttons.push_back(std::move(item));
			}
			plugin.Toolbars.push_back(std::move(toolbar));
		};

		using Row = std::tuple<const char *, const char *, BuiltinStudioTool>;
		addToolbar(
			"home",
			"Home",
			std::initializer_list<Row>{
				{"insert", "Insert Object", BuiltinStudioTool::InsertObject},
				{"transform", "Transform Modes", BuiltinStudioTool::TransformModes},
				{"snap", "Snap Controls", BuiltinStudioTool::SnapControls},
				{"selection", "Selection Flags", BuiltinStudioTool::SelectionFlags},
			}
		);
		addToolbar(
			"model",
			"Model",
			std::initializer_list<Row>{
				{"pivot", "Pivot Controls", BuiltinStudioTool::PivotControls},
				{"selection", "Selection Actions", BuiltinStudioTool::SelectionActions},
			}
		);
		addToolbar(
			"script",
			"Script",
			std::initializer_list<Row>{
				{"create", "Create Scripts", BuiltinStudioTool::ScriptCreation},
				{"panels", "Script Panels", BuiltinStudioTool::ScriptPanels},
			}
		);
		addToolbar(
			"view",
			"View",
			std::initializer_list<Row>{
				{"viewport", "Viewport Options", BuiltinStudioTool::ViewportOptions},
				{"indicator", "Direction Gizmo", BuiltinStudioTool::ViewportIndicator},
				{"cursor", "3D Cursor", BuiltinStudioTool::Cursor3D},
				{"orbit", "Orbit Around Cursor", BuiltinStudioTool::OrbitAroundCursor},
				{"direction-lock", "Lock Direction", BuiltinStudioTool::DirectionLock},
				{"panels", "Panel Options", BuiltinStudioTool::PanelOptions},
				{"camera", "Camera Speed", BuiltinStudioTool::CameraSpeed},
			}
		);
		addToolbar(
			"plugins",
			"Plugins",
			std::initializer_list<Row>{{"manage", "Plugin Management", BuiltinStudioTool::Plugins}}
		);
		addToolbar(
			"demo", "Demo", std::initializer_list<Row>{{"demo", "Demo Tools", BuiltinStudioTool::Demo}}
		);
		return plugin;
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
		if (const auto found = document.find("id"); found != document.end()) {
			if (!found->is_string() || found->get<std::string>().empty()) {
				error = "'id' has to be non-empty text";
				return false;
			}
			out.Id = found->get<std::string>();
		}
		if (const auto found = document.find("version"); found != document.end() && found->is_string()) {
			out.Version = found->get<std::string>();
		}
		if (const auto found = document.find("author"); found != document.end() && found->is_string()) {
			out.Author = found->get<std::string>();
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

			if (plugin.Manifest.Id.empty()) {
				plugin.Manifest.Id = folder.filename().string();
			}

			const auto duplicate = std::find_if(found.begin(), found.end(), [&](const LoadedPlugin &other) {
				return PluginIdentity(other) == plugin.Manifest.Id;
			});
			if (duplicate != found.end()) {
				plugin.Error = "duplicate plugin id '" + plugin.Manifest.Id + "'";
				duplicate->Error = plugin.Error;
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
			if (plugin.Builtin) {
				plugin.Running = true;
				plugin.Error.clear();
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

		if (!ToolbarPreferencesLoaded) {
			ToolbarPreferencesLoaded = true;
			std::string error;
			const std::filesystem::path path = ConfigPath("toolbar.json");
			if (std::filesystem::exists(path) && !LoadToolbarPreferences(path, ToolbarPrefs, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		}
		if (!PluginStateLoaded) {
			PluginStateLoaded = true;
			std::string error;
			const std::filesystem::path path = ConfigPath("plugins.json");
			if (std::filesystem::exists(path) && !LoadPluginState(path, PluginEnabled, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		}

		Plugins.push_back(MakeDefaultStudioPlugin());

		if (Universe == nullptr || !Active.IsValid()) {
			return;
		}

		RegisterSelectionComponent();
		std::vector<LoadedPlugin> discovered = DiscoverPlugins(PluginRoot());
		for (LoadedPlugin &plugin : discovered) {
			if (PluginIdentity(plugin) == PluginIdentity(Plugins.front())) {
				plugin.Error = "plugin id is reserved by Default Studio";
			}
			if (const auto found = PluginEnabled.find(PluginIdentity(plugin)); found != PluginEnabled.end()) {
				plugin.Manifest.Enabled = found->second;
			}
			Plugins.push_back(std::move(plugin));
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

			const size_t widgetCount = plugin.Widgets.size();
			for (size_t widgetIndex = 0; widgetIndex < widgetCount; widgetIndex++) {
				PluginWidget &widget = plugin.Widgets[widgetIndex];
				if (!widget.Open) {
					continue;
				}

				// **The plugin's name in the id and not in the title.** Two
				// plugins may both call a panel "Settings", and ImGui keys a
				// window on its whole label - so the id suffix keeps them apart
				// without putting a prefix in front of what a person reads.
				const std::string label =
					widget.Title + "###plugin." + PluginIdentity(plugin) + "." + widget.Id;

				const ImVec2 minimum(
					engine::ui::Scaled(std::max(1.0f, widget.MinimumWidth)),
					engine::ui::Scaled(std::max(1.0f, widget.MinimumHeight))
				);
				const ImVec2 maximum(
					widget.MaximumWidth > 0.0f
						? engine::ui::Scaled(std::max(widget.MaximumWidth, widget.MinimumWidth))
						: FLT_MAX,
					widget.MaximumHeight > 0.0f
						? engine::ui::Scaled(std::max(widget.MaximumHeight, widget.MinimumHeight))
						: FLT_MAX
				);
				ImGui::SetNextWindowSizeConstraints(minimum, maximum);

				const char *dockWindow = nullptr;
				switch (widget.Dock) {
				case PluginDock::Centre:
					dockWindow = "Viewport 1";
					break;
				case PluginDock::Left:
					dockWindow = "Explorer";
					break;
				case PluginDock::Right:
					dockWindow = "Properties";
					break;
				case PluginDock::Bottom:
					dockWindow = "Output";
					break;
				case PluginDock::Floating:
					break;
				}
				if (dockWindow != nullptr) {
					if (const ImGuiWindow *target = ImGui::FindWindowByName(dockWindow);
						target != nullptr && target->DockId != 0) {
						ImGui::SetNextWindowDockID(target->DockId, ImGuiCond_FirstUseEver);
					}
				}

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

	void Editor::DrawBuiltinStudioTool(BuiltinStudioTool tool) {
		switch (tool) {
		case BuiltinStudioTool::InsertObject:
			DrawInsertObjectTool();
			break;
		case BuiltinStudioTool::TransformModes:
			DrawTransformModesTool();
			break;
		case BuiltinStudioTool::SnapControls:
			DrawSnapControlsTool();
			break;
		case BuiltinStudioTool::SelectionFlags:
			DrawSelectionFlagsTool();
			break;
		case BuiltinStudioTool::PivotControls:
			DrawPivotControlsTool();
			break;
		case BuiltinStudioTool::SelectionActions:
			DrawSelectionActionsTool();
			break;
		case BuiltinStudioTool::ScriptCreation:
			DrawScriptCreationTool();
			break;
		case BuiltinStudioTool::ScriptPanels:
			DrawScriptPanelsTool();
			break;
		case BuiltinStudioTool::ViewportOptions:
			DrawViewportOptionsTool();
			break;
		case BuiltinStudioTool::ViewportIndicator:
			DrawViewportIndicatorTool();
			break;
		case BuiltinStudioTool::Cursor3D:
			DrawCursor3DTool();
			break;
		case BuiltinStudioTool::OrbitAroundCursor:
			DrawOrbitAroundCursorTool();
			break;
		case BuiltinStudioTool::DirectionLock:
			DrawDirectionLockTool();
			break;
		case BuiltinStudioTool::PanelOptions:
			DrawPanelOptionsTool();
			break;
		case BuiltinStudioTool::CameraSpeed:
			DrawCameraSpeedTool();
			break;
		case BuiltinStudioTool::Plugins:
			DrawPluginTools();
			break;
		case BuiltinStudioTool::Demo:
			DrawDemoTools();
			break;
		case BuiltinStudioTool::None:
			break;
		}
	}

	void Editor::DrawPluginToolbar() {
		const std::vector<ToolbarTabView> tabs = ComposeToolbar(Plugins, ToolbarPrefs);
		if (tabs.empty()) {
			if (ImGui::Button("Manage Plugins")) {
				ShowPlugins = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Edit Toolbar")) {
				ShowToolbarEditor = true;
			}
			return;
		}

		if (!ImGui::BeginTabBar("ribbon", ImGuiTabBarFlags_FittingPolicyScroll)) {
			return;
		}

		int selected = -1;
		for (size_t index = 0; index < tabs.size(); index++) {
			const ToolbarTabView &tab = tabs[index];
			const std::string label = tab.Name + "###toolbar." + tab.Id;
			if (ImGui::BeginTabItem(label.c_str())) {
				selected = static_cast<int>(index);
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();

		if (selected < 0) {
			selected = 0;
		}
		const ToolbarTabView &tab = tabs[static_cast<size_t>(selected)];

		ImGui::BeginChild("toolbar-row", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
		for (size_t shown = 0; shown < tab.Items.size(); shown++) {
			const ToolbarItemLocation &location = tab.Items[shown];
			if (location.Plugin >= Plugins.size()) {
				continue;
			}
			LoadedPlugin &plugin = Plugins[location.Plugin];
			if (location.Toolbar >= plugin.Toolbars.size()) {
				continue;
			}
			PluginToolbar &toolbar = plugin.Toolbars[location.Toolbar];
			if (location.Item >= toolbar.Buttons.size()) {
				continue;
			}
			PluginButton &button = toolbar.Buttons[location.Item];
			const std::string tooltip = button.Tooltip;

			if (shown > 0) {
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}

			ImGui::PushID(location.Key.c_str());
			if (button.Kind == PluginControlKind::Builtin) {
				DrawBuiltinStudioTool(button.Builtin);
				ImGui::PopID();
				continue;
			}

			const std::string label = button.Name + "###control";
			if (button.Kind == PluginControlKind::Button) {
				const bool pressed =
					button.Active ? ImGui::Selectable(label.c_str(), true, 0, ImVec2(location.Width, 0.0f))
								  : ImGui::Button(label.c_str(), ImVec2(location.Width, 0.0f));
				if (pressed) {
					InvokePlugin(plugin, button.OnClick, false);
				}
			} else if (button.Kind == PluginControlKind::Toggle) {
				const bool before = button.Active;
				ImGui::Checkbox(label.c_str(), &button.Active);
				if (before != button.Active) {
					const engine::script::HostValue value = engine::script::HostValue::Of(button.Active);
					InvokePlugin(plugin, button.OnChanged, false, engine::script::HostArguments(&value, 1));
				}
			} else if (button.Kind == PluginControlKind::Dropdown) {
				ImGui::SetNextItemWidth(location.Width);
				const char *preview = button.Selected < button.Options.size()
										  ? button.Options[button.Selected].c_str()
										  : "(none)";
				if (ImGui::BeginCombo(label.c_str(), preview)) {
					for (size_t option = 0; option < button.Options.size(); option++) {
						if (!ImGui::Selectable(button.Options[option].c_str(), option == button.Selected)) {
							continue;
						}
						button.Selected = option;
						const engine::script::HostValue arguments[] = {
							engine::script::HostValue::Of(static_cast<double>(option + 1)),
							engine::script::HostValue::Of(std::string_view(button.Options[option])),
						};
						InvokePlugin(
							plugin, button.OnChanged, false, engine::script::HostArguments(arguments, 2)
						);
					}
					ImGui::EndCombo();
				}
			}

			if (!tooltip.empty() && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tooltip.c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
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

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		if (ImGui::Button("Toolbar", ImVec2(84.0f, 0.0f))) {
			ShowToolbarEditor = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Dock Widgets", ImVec2(104.0f, 0.0f))) {
			ShowDockWidgetEditor = true;
		}
		ImGui::SameLine();
		const size_t running =
			static_cast<size_t>(std::count_if(Plugins.begin(), Plugins.end(), [](const LoadedPlugin &plugin) {
				return plugin.Running;
			}));
		ImGui::TextDisabled("%zu of %zu running", running, Plugins.size());
	}

	void Editor::DrawToolbarEditor() {
		if (!ShowToolbarEditor) {
			return;
		}
		if (!ImGui::Begin("Toolbar Editor", &ShowToolbarEditor)) {
			ImGui::End();
			return;
		}

		const auto save = [this]() {
			std::string error;
			if (!SaveToolbarPreferences(ConfigPath("toolbar.json"), ToolbarPrefs, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		};

		ImGui::SetNextItemWidth(engine::ui::Scaled(220.0f));
		ImGui::InputTextWithHint(
			"##new-toolbar-tab", "New tab name", ToolbarTabDraft, sizeof(ToolbarTabDraft)
		);
		ImGui::SameLine();
		if (ImGui::Button("Add Tab") && ToolbarTabDraft[0] != '\0') {
			size_t serial = 1;
			for (;;) {
				const std::string candidate = "user/" + std::to_string(serial++);
				const bool used = std::any_of(
					ToolbarPrefs.Tabs.begin(), ToolbarPrefs.Tabs.end(), [&](const ToolbarTabPreference &tab) {
						return tab.Id == candidate;
					}
				);
				if (used) {
					continue;
				}
				ToolbarPrefs.Tabs.push_back(ToolbarTabPreference{candidate, ToolbarTabDraft, true, true});
				ToolbarTabDraft[0] = '\0';
				save();
				break;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			ToolbarPrefs = ToolbarPreferences{};
			save();
		}

		if (!ToolbarPrefs.Tabs.empty()) {
			ImGui::SeparatorText("Custom tabs");
			for (size_t index = 0; index < ToolbarPrefs.Tabs.size();) {
				ToolbarTabPreference &tab = ToolbarPrefs.Tabs[index];
				if (!tab.UserCreated) {
					index++;
					continue;
				}
				ImGui::PushID(tab.Id.c_str());
				bool changed = ImGui::Checkbox("##visible", &tab.Visible);
				ImGui::SameLine();
				ImGui::TextUnformatted(tab.Name.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					for (ToolbarItemPreference &item : ToolbarPrefs.Items) {
						if (item.Tab == tab.Id) {
							item.Tab.clear();
						}
					}
					ToolbarPrefs.Tabs.erase(ToolbarPrefs.Tabs.begin() + static_cast<std::ptrdiff_t>(index));
					save();
					ImGui::PopID();
					continue;
				}
				if (changed) {
					save();
				}
				ImGui::PopID();
				index++;
			}
		}

		struct TabChoice {
			std::string Id;
			std::string Name;
		};
		std::vector<TabChoice> choices;
		for (const ToolbarTabPreference &tab : ToolbarPrefs.Tabs) {
			choices.push_back(TabChoice{tab.Id, tab.Name});
		}
		for (const LoadedPlugin &plugin : Plugins) {
			for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
				const PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
				const std::string id = PluginToolbarKey(plugin, toolbar, toolbarIndex);
				if (std::none_of(choices.begin(), choices.end(), [&](const TabChoice &choice) {
						return choice.Id == id;
					})) {
					choices.push_back(TabChoice{id, toolbar.Name});
				}
			}
		}

		ImGui::SeparatorText("Tools");
		if (ImGui::BeginTable(
				"toolbar-items",
				4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
			)) {
			ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(58.0f));
			ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Tab", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(170.0f));
			ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(130.0f));
			ImGui::TableHeadersRow();

			for (size_t pluginIndex = 0; pluginIndex < Plugins.size(); pluginIndex++) {
				LoadedPlugin &plugin = Plugins[pluginIndex];
				for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
					PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
					const std::string defaultTab = PluginToolbarKey(plugin, toolbar, toolbarIndex);
					for (size_t itemIndex = 0; itemIndex < toolbar.Buttons.size(); itemIndex++) {
						PluginButton &button = toolbar.Buttons[itemIndex];
						const std::string key =
							PluginToolKey(plugin, toolbar, toolbarIndex, button, itemIndex);
						auto found = std::find_if(
							ToolbarPrefs.Items.begin(),
							ToolbarPrefs.Items.end(),
							[&](const ToolbarItemPreference &item) { return item.Key == key; }
						);
						const auto ensure = [&]() -> ToolbarItemPreference & {
							if (found == ToolbarPrefs.Items.end()) {
								ToolbarPrefs.Items.push_back(
									ToolbarItemPreference{
										key, defaultTab, button.Visible, ClampPluginToolWidth(button.Width)
									}
								);
								found = std::prev(ToolbarPrefs.Items.end());
							}
							return *found;
						};

						ImGui::TableNextRow();
						ImGui::PushID(key.c_str());
						ImGui::TableNextColumn();
						bool visible = found == ToolbarPrefs.Items.end() ? button.Visible : found->Visible;
						if (ImGui::Checkbox("##visible", &visible)) {
							ensure().Visible = visible;
							save();
						}

						ImGui::TableNextColumn();
						ImGui::TextUnformatted(button.Name.c_str());
						ImGui::TextDisabled("%s", plugin.Manifest.Name.c_str());

						ImGui::TableNextColumn();
						const std::string current =
							found == ToolbarPrefs.Items.end() || found->Tab.empty() ? defaultTab : found->Tab;
						const auto currentChoice =
							std::find_if(choices.begin(), choices.end(), [&](const TabChoice &choice) {
								return choice.Id == current;
							});
						const char *preview =
							currentChoice == choices.end() ? current.c_str() : currentChoice->Name.c_str();
						if (ImGui::BeginCombo("##tab", preview)) {
							for (const TabChoice &choice : choices) {
								if (ImGui::Selectable(choice.Name.c_str(), choice.Id == current)) {
									ensure().Tab = choice.Id;
									save();
								}
							}
							ImGui::EndCombo();
						}

						ImGui::TableNextColumn();
						float width = found == ToolbarPrefs.Items.end() ? ClampPluginToolWidth(button.Width)
																		: found->Width;
						ImGui::BeginDisabled(button.Kind == PluginControlKind::Builtin);
						ImGui::SetNextItemWidth(-1.0f);
						if (ImGui::DragFloat(
								"##width",
								&width,
								1.0f,
								PLUGIN_TOOL_MINIMUM_WIDTH,
								PLUGIN_TOOL_MAXIMUM_WIDTH,
								"%.0f px",
								ImGuiSliderFlags_AlwaysClamp
							)) {
							ensure().Width = ClampPluginToolWidth(width);
							save();
						}
						ImGui::EndDisabled();
						ImGui::PopID();
					}
				}
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Editor::DrawDockWidgetEditor() {
		if (!ShowDockWidgetEditor) {
			return;
		}
		if (!ImGui::Begin("Dock Widgets", &ShowDockWidgetEditor)) {
			ImGui::End();
			return;
		}

		ImGui::TextWrapped(
			"Plugin widgets are ordinary ImGui dock windows. The requested dock and size limits apply on "
			"first use; "
			"after that, the saved layout belongs to the person using Studio."
		);
		ImGui::Separator();

		bool any = false;
		for (LoadedPlugin &plugin : Plugins) {
			for (PluginWidget &widget : plugin.Widgets) {
				any = true;
				const std::string id =
					PluginIdentity(plugin) + "/widget/" + (widget.Id.empty() ? widget.Title : widget.Id);
				ImGui::PushID(id.c_str());
				ImGui::Checkbox("##open", &widget.Open);
				ImGui::SameLine();
				ImGui::Text("%s  (%s)", widget.Title.c_str(), plugin.Manifest.Name.c_str());

				ImGui::SetNextItemWidth(engine::ui::Scaled(140.0f));
				if (ImGui::BeginCombo("Dock", Describe(widget.Dock))) {
					for (size_t ordinal = 0; ordinal <= static_cast<size_t>(PluginDock::Bottom); ordinal++) {
						const auto dock = static_cast<PluginDock>(ordinal);
						if (ImGui::Selectable(Describe(dock), dock == widget.Dock)) {
							widget.Dock = dock;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Min W", &widget.MinimumWidth, 1.0f, 1.0f, 4096.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Min H", &widget.MinimumHeight, 1.0f, 1.0f, 4096.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Max W", &widget.MaximumWidth, 1.0f, 0.0f, 8192.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Max H", &widget.MaximumHeight, 1.0f, 0.0f, 8192.0f, "%.0f px");
				widget.MinimumWidth = std::max(1.0f, widget.MinimumWidth);
				widget.MinimumHeight = std::max(1.0f, widget.MinimumHeight);
				widget.MaximumWidth = std::max(0.0f, widget.MaximumWidth);
				widget.MaximumHeight = std::max(0.0f, widget.MaximumHeight);
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		if (!any) {
			ImGui::TextDisabled("No running plugin created a dock widget.");
		}
		ImGui::End();
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

		bool reloadAfterTable = false;
		bool saveState = false;
		if (ImGui::BeginTable("plugins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 36.0f);
			ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("What it is doing", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (LoadedPlugin &plugin : Plugins) {
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				const std::string identity = PluginIdentity(plugin);
				bool enabled = plugin.Manifest.Enabled;
				const std::string enabledId = "##enabled." + identity;
				ImGui::BeginDisabled(plugin.Builtin);
				if (ImGui::Checkbox(enabledId.c_str(), &enabled)) {
					PluginEnabled[identity] = enabled;
					saveState = true;
					reloadAfterTable = true;
				}
				ImGui::EndDisabled();

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(plugin.Manifest.Name.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", plugin.Root.string().c_str());
				}
				if (!plugin.Manifest.Version.empty() || !plugin.Manifest.Author.empty()) {
					ImGui::TextDisabled(
						"%s%s%s",
						plugin.Manifest.Version.empty() ? "" : plugin.Manifest.Version.c_str(),
						!plugin.Manifest.Version.empty() && !plugin.Manifest.Author.empty() ? " by " : "",
						plugin.Manifest.Author.empty() ? "" : plugin.Manifest.Author.c_str()
					);
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

		if (saveState) {
			std::string error;
			if (!SavePluginState(ConfigPath("plugins.json"), PluginEnabled, error)) {
				Say(error, engine::core::LogLevel::Warning);
				reloadAfterTable = false;
			}
		}
		if (reloadAfterTable) {
			LoadPlugins();
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
