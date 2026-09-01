// Discovering, starting and beating the editor's plugins.
//
// **What is under test is the isolation.** A plugin system whose happy path
// works and whose failure path takes the editor with it is worse than no plugin
// system, because the failure arrives on somebody else's machine with a script
// nobody here wrote. So most of these cases install something broken and assert
// that the rest still run.
//
// The panel is not tested and does not need to be: it draws a list and a button,
// and what the button does is `LoadPlugins`, which is.

#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Host.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <vector>

TEST_SUITE_ID("studio.plugins")
TEST_DEPENDS("engine.scripthost.scripting")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using studio::BeatCppPlugins;
using studio::BeatPlugins;
using studio::BuiltinStudioPanel;
using studio::BuiltinStudioTool;
using studio::ClampPluginToolWidth;
using studio::ComposeToolbar;
using studio::DiscoverPlugins;
using studio::LoadedCppPlugin;
using studio::LoadedPlugin;
using studio::LoadToolbarPreferences;
using studio::MakeDefaultStudioPlugin;
using studio::ParsePluginManifest;
using studio::PLUGIN_FAULT_LIMIT;
using studio::PluginBindingLanguage;
using studio::PluginBindingRegistry;
using studio::PluginButton;
using studio::PluginControlKind;
using studio::PluginDock;
using studio::PluginManifest;
using studio::PluginRunTarget;
using studio::PluginToolbar;
using studio::PluginToolbarPlacement;
using studio::PluginToolbarTrack;
using studio::PluginWidget;
using studio::PluginWidgetLabel;
using studio::RegisterSelectionComponent;
using studio::SaveToolbarPreferences;
using studio::SELECTED_COMPONENT;
using studio::StartCppPlugins;
using studio::StartPlugins;
using studio::StopCppPlugins;
using studio::ToolbarItemPreference;
using studio::ToolbarPreferences;
using studio::ToolbarTabPreference;

namespace {
	// A plugins folder, written per case and removed after it.
	struct Folder {
		std::filesystem::path Root;

		Folder() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-studio-plugins-" +
					std::to_string(
						std::filesystem::hash_value(std::filesystem::temp_directory_path() / "studio-plugins")
					));
			std::filesystem::remove_all(Root);
			std::filesystem::create_directories(Root);
		}

		~Folder() {
			std::filesystem::remove_all(Root);
		}

		void Add(const std::string &folder, const char *manifest, const char *source) {
			std::filesystem::create_directories(Root / folder);

			std::ofstream out(Root / folder / "plugin.json");
			out << manifest;

			if (source != nullptr) {
				std::ofstream script(Root / folder / "main.luau");
				script << source;
			}
		}

		void
		AddWithMain(const std::string &folder, const char *manifest, const char *main, const char *source) {
			std::filesystem::create_directories(Root / folder);
			std::ofstream(Root / folder / "plugin.json") << manifest;
			std::ofstream(Root / folder / main) << source;
		}
	};

	const LoadedPlugin *Named(const std::vector<LoadedPlugin> &plugins, std::string_view name) {
		for (const LoadedPlugin &plugin : plugins) {
			if (plugin.Manifest.Name == name) {
				return &plugin;
			}
		}
		return nullptr;
	}

	std::vector<studio::ToolbarItemLocation> ItemsOf(const studio::ToolbarTabView &tab) {
		std::vector<studio::ToolbarItemLocation> items;
		for (const studio::ToolbarRowView &row : tab.Rows) {
			for (const studio::ToolbarCellView &cell : row.Cells) {
				items.insert(items.end(), cell.Items.begin(), cell.Items.end());
			}
		}
		return items;
	}

	studio::PluginPresentation DefaultPresentation() {
		const studio::CppPluginDefinition definition = MakeDefaultStudioPlugin();
		studio::PluginPresentation plugin;
		plugin.Manifest = definition.Manifest;
		plugin.Native = true;
		studio::CppPluginContext context;
		context.Presentation = &plugin;
		std::string error;
		plugin.Running = definition.Open(context, error);
		INFO(error);
		return plugin;
	}
}

// --- the manifest --------------------------------------------------------------

TEST_CASE("a manifest needs a name and defaults the rest", "[studio][plugins]") {
	PluginManifest manifest;
	std::string error;

	REQUIRE(ParsePluginManifest(R"({"name": "Align"})", manifest, error));
	CHECK(manifest.Name == "Align");
	CHECK(manifest.Main == "main.luau");
	CHECK(manifest.Enabled);
	CHECK(manifest.Description.empty());

	CHECK_FALSE(ParsePluginManifest(R"({"description": "no name"})", manifest, error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(ParsePluginManifest(R"({"name": ""})", manifest, error));
	CHECK_FALSE(ParsePluginManifest("{ this is not json", manifest, error));
}

TEST_CASE("a main that escapes its own folder is refused", "[studio][plugins]") {
	// **A manifest is a file somebody downloaded.** A `main` pointing outside
	// the plugin's folder is that file reading one it was never given.
	PluginManifest manifest;
	std::string error;

	CHECK_FALSE(ParsePluginManifest(R"({"name": "Bad", "main": "../../secrets"})", manifest, error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(ParsePluginManifest(R"({"name": "Bad", "main": "/etc/passwd"})", manifest, error));
	CHECK_FALSE(ParsePluginManifest(R"({"name": "Bad", "main": ""})", manifest, error));

	// Down is fine. It is only up that is refused.
	CHECK(ParsePluginManifest(R"({"name": "Fine", "main": "src/main.luau"})", manifest, error));
	CHECK(ParsePluginManifest(R"({"name": "Fine", "main": "a/../b/main.luau"})", manifest, error));
}

TEST_CASE("plugin run targets are explicit and default to Studio", "[studio][plugins]") {
	PluginManifest manifest;
	std::string error;

	REQUIRE(ParsePluginManifest(R"({"name":"Old"})", manifest, error));
	CHECK(studio::RunsIn(manifest.Runs, PluginRunTarget::Studio));
	CHECK_FALSE(studio::RunsIn(manifest.Runs, PluginRunTarget::PlaytestServer));
	CHECK_FALSE(studio::RunsIn(manifest.Runs, PluginRunTarget::PlaytestClient));

	REQUIRE(ParsePluginManifest(
		R"({"name":"Play","runs":["playtest-server","playtest-client"]})", manifest, error
	));
	CHECK_FALSE(studio::RunsIn(manifest.Runs, PluginRunTarget::Studio));
	CHECK(studio::RunsIn(manifest.Runs, PluginRunTarget::PlaytestServer));
	CHECK(studio::RunsIn(manifest.Runs, PluginRunTarget::PlaytestClient));

	CHECK_FALSE(ParsePluginManifest(R"({"name":"Bad","runs":[]})", manifest, error));
	CHECK(error.find("at least one") != std::string::npos);
	CHECK_FALSE(ParsePluginManifest(R"({"name":"Bad","runs":["shipping"]})", manifest, error));
	CHECK(error.find("unknown") != std::string::npos);
}

TEST_CASE("dynamic plugin bindings are collected with their owner", "[studio][plugins]") {
	PluginBindingRegistry registry;
	size_t changes = 0;
	registry.OnChanged([&] { changes++; });

	std::string error;
	auto owner = registry.OpenScope();
	REQUIRE(owner.Add(
		"Suite.Version",
		[](engine::script::HostArguments, engine::script::HostValue &result, std::string &) {
			result = engine::script::HostValue::Of(21.0);
			return true;
		},
		error
	));
	CHECK(changes == 1);
	CHECK(registry.Names(engine::script::Language::Luau) == std::vector<std::string>{"Suite.Version"});
	CHECK(registry.Names(engine::script::Language::JavaScript).empty());

	auto duplicate = registry.OpenScope();
	CHECK_FALSE(duplicate.Add("Suite.Version", [](auto, auto &, auto &) { return true; }, error));
	CHECK(error.find("already registered") != std::string::npos);

	engine::script::HostValue result;
	REQUIRE(registry.Call(engine::script::Language::Luau, "Suite.Version", {}, result, error));
	CHECK(result.AsNumber() == 21.0);

	owner.Close();
	CHECK(changes == 2);
	CHECK(registry.Names(engine::script::Language::Luau).empty());
	CHECK_FALSE(registry.Call(engine::script::Language::Luau, "Suite.Version", {}, result, error));
}

TEST_CASE("dynamic Studio bindings cannot hide built-in host calls", "[studio][plugins]") {
	PluginBindingRegistry studioRegistry;
	auto studioOwner = studioRegistry.OpenScope();
	std::string error;
	CHECK_FALSE(studioOwner.Add("Notify", [](auto, auto &, auto &) { return true; }, error));
	CHECK(error.find("built-in Studio host function") != std::string::npos);
	CHECK(studioRegistry.Names(engine::script::Language::Luau).empty());

	PluginBindingRegistry serverRegistry(PluginRunTarget::PlaytestServer);
	auto serverOwner = serverRegistry.OpenScope();
	REQUIRE(serverOwner.Add("Notify", [](auto, auto &, auto &) { return true; }, error));
	CHECK(serverRegistry.Names(engine::script::Language::Luau) == std::vector<std::string>{"Notify"});
	CHECK_FALSE(serverOwner.Add(
		"Bad.Language", [](auto, auto &, auto &) { return true; }, error, static_cast<uint8_t>(0x80)
	));
	CHECK(error.find("unknown script language") != std::string::npos);
}

TEST_CASE("Luau host tables gain and lose native plugin bindings", "[studio][plugins]") {
	class BindingSurface final : public engine::script::HostSurface {
	  public:
		explicit BindingSurface(PluginBindingRegistry &registry) : Registry(registry) {}

		std::string_view GlobalName() const override {
			return "plugin";
		}

		std::vector<std::string> Names() const override {
			return Registry.Names(engine::script::Language::Luau);
		}

		bool Call(
			std::string_view name,
			engine::script::HostArguments arguments,
			engine::script::HostValue &result,
			std::string &failure
		) override {
			return Registry.Call(engine::script::Language::Luau, name, arguments, result, failure);
		}

		PluginBindingRegistry &Registry;
	};

	engine::scene::EnsureClassTree();
	Store store("dynamic-luau-binding");
	PluginBindingRegistry registry;
	BindingSurface surface(registry);
	std::string error;
	auto owner = registry.OpenScope();
	REQUIRE(owner.Add(
		"Echo",
		[](engine::script::HostArguments arguments, engine::script::HostValue &result, std::string &) {
			result = engine::script::HostValue::Of(arguments[0].AsNumber() + 1.0);
			return true;
		},
		error
	));

	engine::script::RuntimeLimits limits;
	limits.Role.Studio = true;
	limits.Origin = engine::script::ScriptOrigin::Plugin;
	auto runtime = engine::script::MakeRuntime(store, engine::script::Language::Luau, limits);
	REQUIRE(runtime != nullptr);
	runtime->SetHost(&surface);
	REQUIRE(runtime->Run("assert(plugin.Echo(41) == 42)", "dynamic-add"));

	owner.Close();
	runtime->SetHost(&surface);
	INFO(runtime->LastError());
	CHECK(runtime->Run("assert(plugin.Echo == nil)", "dynamic-remove"));
}

TEST_CASE("native plugin close runs before its Luau bindings are collected", "[studio][plugins]") {
	Store store("native-plugin");
	PluginBindingRegistry registry;
	bool opened = false;
	bool closedWithBinding = false;
	size_t beats = 0;

	studio::CppPluginDefinition definition;
	definition.Manifest.Name = "Native Suite";
	definition.Manifest.Id = "test.native-suite";
	definition.Open = [&](studio::CppPluginContext &context, std::string &error) {
		opened = true;
		return context.Bindings->Add("Suite.Ping", [](auto, auto &, auto &) { return true; }, error);
	};
	definition.Heartbeat = [&](studio::CppPluginContext &, float, std::string &) {
		beats++;
		return true;
	};
	definition.Close = [&](studio::CppPluginContext &) {
		closedWithBinding = !registry.Names(engine::script::Language::Luau).empty();
	};

	std::vector<LoadedCppPlugin> plugins;
	StartCppPlugins(plugins, {definition}, store, registry);
	REQUIRE(plugins.size() == 1);
	CHECK(opened);
	CHECK(plugins.front().Native);
	CHECK(plugins.front().Running);
	CHECK(BeatCppPlugins(plugins, 1.0f / 60.0f) == 1);
	CHECK(beats == 1);

	StopCppPlugins(plugins);
	CHECK(closedWithBinding);
	CHECK(registry.Names(engine::script::Language::Luau).empty());
}

TEST_CASE("C++ plugin definitions register and unregister dynamically", "[studio][plugins]") {
	studio::CppPluginDefinition definition;
	definition.Manifest.Name = "Registered Native";
	definition.Manifest.Id = "test.registered-native";
	definition.Open = [](studio::CppPluginContext &, std::string &) { return true; };

	const uint64_t before = studio::CppPluginRegistryRevision();
	std::string error;
	studio::CppPluginDefinition invalid = definition;
	invalid.Manifest.Id = "test.invalid-native";
	invalid.Manifest.Runs = static_cast<studio::PluginRunTargets>(0x80);
	auto invalidRegistration = studio::RegisterCppPlugin(invalid, error);
	CHECK_FALSE(invalidRegistration.IsOpen());
	CHECK(error.find("unknown run target") != std::string::npos);

	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	CHECK(studio::CppPluginRegistryRevision() > before);
	const auto registered = studio::RegisteredCppPlugins();
	CHECK(std::any_of(registered.begin(), registered.end(), [](const auto &plugin) {
		return plugin.Manifest.Id == "test.registered-native";
	}));

	const uint64_t loaded = studio::CppPluginRegistryRevision();
	registration.Close();
	CHECK(studio::CppPluginRegistryRevision() > loaded);
	const auto removed = studio::RegisteredCppPlugins();
	CHECK(std::none_of(removed.begin(), removed.end(), [](const auto &plugin) {
		return plugin.Manifest.Id == "test.registered-native";
	}));
}

TEST_CASE("faulted C++ plugins close before losing their bindings", "[studio][plugins]") {
	Store store("faulted-native-plugin");
	PluginBindingRegistry registry;
	bool closed = false;

	studio::CppPluginDefinition definition;
	definition.Manifest.Name = "Faulted Native";
	definition.Manifest.Id = "test.faulted-native";
	definition.Open = [](studio::CppPluginContext &context, std::string &error) {
		return context.Bindings->Add("Faulted.Ping", [](auto, auto &, auto &) { return true; }, error);
	};
	definition.Heartbeat = [](studio::CppPluginContext &, float, std::string &error) {
		error = "beat failed";
		return false;
	};
	definition.Close = [&](studio::CppPluginContext &) {
		closed = !registry.Names(engine::script::Language::Luau).empty();
	};

	std::vector<LoadedCppPlugin> plugins;
	StartCppPlugins(plugins, {definition}, store, registry);
	for (size_t fault = 0; fault < PLUGIN_FAULT_LIMIT; fault++) {
		BeatCppPlugins(plugins, 1.0f / 60.0f);
	}
	REQUIRE(plugins.size() == 1);
	CHECK_FALSE(plugins.front().Running);
	CHECK(closed);
	CHECK(registry.Names(engine::script::Language::Luau).empty());
}

TEST_CASE("C++ plugins start only in their selected playtest role", "[studio][plugins]") {
	Store store("native-playtest-target");
	PluginBindingRegistry registry(PluginRunTarget::PlaytestServer);
	bool studioOpened = false;
	bool serverOpened = false;

	studio::CppPluginDefinition studioDefinition;
	studioDefinition.Manifest.Name = "Studio Native";
	studioDefinition.Manifest.Id = "test.studio-native";
	studioDefinition.Open = [&](studio::CppPluginContext &, std::string &) {
		studioOpened = true;
		return true;
	};

	studio::CppPluginDefinition serverDefinition;
	serverDefinition.Manifest.Name = "Server Native";
	serverDefinition.Manifest.Id = "test.server-native";
	serverDefinition.Manifest.Runs = studio::PluginTarget(PluginRunTarget::PlaytestServer);
	serverDefinition.Open = [&](studio::CppPluginContext &context, std::string &) {
		serverOpened = context.Target == PluginRunTarget::PlaytestServer;
		return true;
	};

	std::vector<LoadedCppPlugin> plugins;
	StartCppPlugins(
		plugins, {studioDefinition, serverDefinition}, store, registry, PluginRunTarget::PlaytestServer
	);
	REQUIRE(plugins.size() == 2);
	CHECK_FALSE(studioOpened);
	CHECK_FALSE(plugins[0].Running);
	CHECK(plugins[0].Error.find("does not run") != std::string::npos);
	CHECK(serverOpened);
	CHECK(plugins[1].Running);
	StopCppPlugins(plugins);
}

TEST_CASE("the default Studio plugin owns the standard toolbar", "[studio][plugins]") {
	const studio::PluginPresentation plugin = DefaultPresentation();

	CHECK(plugin.Builtin);
	CHECK(plugin.Running);
	CHECK(plugin.Manifest.Id == "atomic.default-studio");
	REQUIRE(plugin.Toolbars.size() == 7);
	REQUIRE(plugin.Widgets.size() == 6);

	const std::array expectedWidgets = {
		std::tuple{"explorer", "Explorer", BuiltinStudioPanel::Explorer, PluginDock::Left},
		std::tuple{"properties", "Properties", BuiltinStudioPanel::Properties, PluginDock::Right},
		std::tuple{
			"component-inspector", "Components", BuiltinStudioPanel::ComponentInspector, PluginDock::Right
		},
		std::tuple{"script-editor", "Script Editor", BuiltinStudioPanel::ScriptEditor, PluginDock::Centre},
		std::tuple{"dataset-editor", "Dataset Editor", BuiltinStudioPanel::DatasetEditor, PluginDock::Bottom},
		std::tuple{"roblox-import", "Roblox Import", BuiltinStudioPanel::RobloxImport, PluginDock::Bottom},
	};
	for (size_t index = 0; index < expectedWidgets.size(); index++) {
		const PluginWidget &widget = plugin.Widgets[index];
		const auto &[id, title, panel, dock] = expectedWidgets[index];
		CHECK(widget.Id == id);
		CHECK(widget.Title == title);
		CHECK(widget.BuiltinPanel == panel);
		CHECK(widget.Dock == dock);
		CHECK(
			widget.Open ==
			(panel != BuiltinStudioPanel::DatasetEditor && panel != BuiltinStudioPanel::RobloxImport)
		);
		CHECK(widget.SynchronizedOpen == widget.Open);
		CHECK_FALSE(widget.Render.Valid());
	}

	std::set<std::string> toolbarIds;
	std::set<std::string> controlIds;
	for (const PluginToolbar &toolbar : plugin.Toolbars) {
		CHECK(toolbarIds.insert(toolbar.Id).second);
		CHECK_FALSE(toolbar.Buttons.empty());
		CHECK(toolbar.Rows.size() == 1);
		CHECK(toolbar.Columns.size() == toolbar.Buttons.size());
		for (const PluginButton &button : toolbar.Buttons) {
			CHECK(controlIds.insert(toolbar.Id + "/" + button.Id).second);
			CHECK_FALSE(button.Row.empty());
			CHECK_FALSE(button.Column.empty());
		}
	}
	CHECK(plugin.Toolbars.front().Id == "transport");
	CHECK(plugin.Toolbars.front().Placement == PluginToolbarPlacement::Pinned);
	CHECK(plugin.Toolbars.front().Buttons.size() == 11);

	const auto view =
		std::find_if(plugin.Toolbars.begin(), plugin.Toolbars.end(), [](const PluginToolbar &toolbar) {
			return toolbar.Id == "view";
		});
	REQUIRE(view != plugin.Toolbars.end());
	const std::array expected = {
		std::pair{"grid", BuiltinStudioTool::Grid},
		std::pair{"particles", BuiltinStudioTool::Particles},
		std::pair{"indicator", BuiltinStudioTool::ViewportIndicator},
		std::pair{"cursor", BuiltinStudioTool::Cursor3D},
		std::pair{"orbit", BuiltinStudioTool::OrbitAroundCursor},
		std::pair{"direction-lock", BuiltinStudioTool::DirectionLock},
		std::pair{"explorer", BuiltinStudioTool::ExplorerPanel},
		std::pair{"properties", BuiltinStudioTool::PropertiesPanel},
		std::pair{"output", BuiltinStudioTool::OutputPanel},
		std::pair{"assets", BuiltinStudioTool::AssetsPanel},
		std::pair{"statistics", BuiltinStudioTool::StatisticsPanel},
		std::pair{"frame-graph", BuiltinStudioTool::FrameGraphPanel},
		std::pair{"heap", BuiltinStudioTool::HeapPanel},
		std::pair{"datasets", BuiltinStudioTool::DatasetEditorPanel},
		std::pair{"camera", BuiltinStudioTool::CameraSpeed},
	};
	REQUIRE(view->Buttons.size() == expected.size());
	for (size_t index = 0; index < expected.size(); index++) {
		CHECK(view->Buttons[index].Id == expected[index].first);
		CHECK(view->Buttons[index].Kind == PluginControlKind::Builtin);
		CHECK(view->Buttons[index].Builtin == expected[index].second);
	}
}

TEST_CASE("plugin widget labels keep matching titles separate", "[studio][plugins]") {
	studio::PluginPresentation first;
	first.Root = "plugins/first";
	studio::PluginPresentation second;
	second.Manifest.Id = "tools.second";

	PluginWidget widget;
	widget.Title = "Explorer";
	widget.Id = "tree";

	CHECK(PluginWidgetLabel(first, widget) == "Explorer###plugin.first.tree");
	CHECK(PluginWidgetLabel(second, widget) == "Explorer###plugin.tools.second.tree");
}

TEST_CASE("a disabled Default Studio plugin contributes no toolbar", "[studio][plugins]") {
	studio::PluginPresentation plugin = DefaultPresentation();
	plugin.Manifest.Enabled = false;
	plugin.Running = false;
	std::vector<studio::PluginPresentation *> plugins = {&plugin};

	const studio::ToolbarLayoutView layout = ComposeToolbar(plugins, {});
	CHECK(layout.PinnedRows.empty());
	CHECK(layout.Tabs.empty());
}

TEST_CASE("toolbar composition uses stable overrides", "[studio][plugins]") {
	studio::PluginPresentation plugin = DefaultPresentation();
	std::vector<studio::PluginPresentation *> plugins = {&plugin};

	const std::string moved =
		studio::PluginToolKey(plugin, plugin.Toolbars[0], 0, plugin.Toolbars[0].Buttons[0], 0);
	const std::string hidden =
		studio::PluginToolKey(plugin, plugin.Toolbars[0], 0, plugin.Toolbars[0].Buttons[1], 1);

	ToolbarPreferences preferences;
	preferences.Tabs.push_back(ToolbarTabPreference{"custom", "My Tools", true, true});
	preferences.Items.push_back(ToolbarItemPreference{moved, "custom", true, 12.0f, "custom-row", "left", 0});
	preferences.Items.push_back(ToolbarItemPreference{hidden, "", false, 92.0f, {}, {}, 0});

	const auto composed = ComposeToolbar(plugins, preferences);
	const auto custom = std::find_if(composed.Tabs.begin(), composed.Tabs.end(), [](const auto &tab) {
		return tab.Id == "custom";
	});
	REQUIRE(custom != composed.Tabs.end());
	const auto customItems = ItemsOf(*custom);
	REQUIRE(customItems.size() == 1);
	CHECK(customItems.front().Key == moved);
	CHECK(customItems.front().Width == studio::PLUGIN_TOOL_MINIMUM_WIDTH);
	REQUIRE(custom->Rows.size() == 1);
	CHECK(custom->Rows.front().Id == "custom-row");

	for (const auto &tab : composed.Tabs) {
		const auto items = ItemsOf(tab);
		CHECK(std::none_of(items.begin(), items.end(), [&](const auto &item) { return item.Key == hidden; }));
	}
	CHECK_FALSE(composed.PinnedRows.empty());
}

TEST_CASE("toolbar preferences round trip by stable text keys", "[studio][plugins]") {
	Folder folder;
	const std::filesystem::path path = folder.Root / "toolbar.json";

	ToolbarPreferences saved;
	saved.Tabs.push_back(ToolbarTabPreference{"custom", "Custom", false, true});
	saved.Tabs.push_back(
		ToolbarTabPreference{"pinned", "Pinned", true, false, PluginToolbarPlacement::Pinned, 4}
	);
	saved.Items.push_back(
		ToolbarItemPreference{"plugin/toolbar/tool", "custom", true, 144.0f, "row-a", "column-b", 7}
	);

	std::string error;
	REQUIRE(SaveToolbarPreferences(path, saved, error));
	ToolbarPreferences loaded;
	REQUIRE(LoadToolbarPreferences(path, loaded, error));
	REQUIRE(loaded.Tabs.size() == 2);
	REQUIRE(loaded.Items.size() == 1);
	CHECK(loaded.Tabs[0].Id == "custom");
	CHECK_FALSE(loaded.Tabs[0].Visible);
	CHECK(loaded.Items[0].Key == "plugin/toolbar/tool");
	CHECK(loaded.Items[0].Width == 144.0f);
	CHECK(loaded.Items[0].Row == "row-a");
	CHECK(loaded.Items[0].Column == "column-b");
	CHECK(loaded.Items[0].Order == 7);
	CHECK(loaded.Tabs[1].Placement == PluginToolbarPlacement::Pinned);
	CHECK(loaded.Tabs[1].Order == 4);
}

TEST_CASE("toolbar preferences ignore fields of the wrong type", "[studio][plugins]") {
	Folder folder;
	const std::filesystem::path path = folder.Root / "toolbar.json";
	{
		std::ofstream out(path);
		out << R"({"tabs":[{"id":7,"name":false},{"id":"kept","name":"Kept"}],)"
			   R"("items":[{"key":"kept/tool","tab":9,"visible":"yes","width":[]} ]})";
	}

	ToolbarPreferences loaded;
	std::string error;
	REQUIRE(LoadToolbarPreferences(path, loaded, error));
	REQUIRE(loaded.Tabs.size() == 1);
	CHECK(loaded.Tabs[0].Id == "kept");
	REQUIRE(loaded.Items.size() == 1);
	CHECK(loaded.Items[0].Tab.empty());
	CHECK(loaded.Items[0].Visible);
	CHECK(loaded.Items[0].Width == 92.0f);
}

TEST_CASE("toolbar widths reject non-finite values and clamp bounds", "[studio][plugins]") {
	CHECK(ClampPluginToolWidth(-20.0f) == studio::PLUGIN_TOOL_MINIMUM_WIDTH);
	CHECK(ClampPluginToolWidth(900.0f) == studio::PLUGIN_TOOL_MAXIMUM_WIDTH);
	CHECK(ClampPluginToolWidth(std::numeric_limits<float>::quiet_NaN()) == 92.0f);
	CHECK(ClampPluginToolWidth(std::numeric_limits<float>::infinity()) == 92.0f);
}

TEST_CASE("toolbar grids preserve declared cells and flat plugin compatibility", "[studio][plugins]") {
	LoadedPlugin plugin;
	plugin.Manifest.Id = "grid";
	plugin.Running = true;

	PluginToolbar pinned;
	pinned.Name = "Pinned";
	pinned.Id = "pinned";
	pinned.Placement = PluginToolbarPlacement::Pinned;
	pinned.Rows = {PluginToolbarTrack{"top"}, PluginToolbarTrack{"bottom"}};
	pinned.Columns = {PluginToolbarTrack{"left"}, PluginToolbarTrack{"right", 180.0f}};
	PluginButton a;
	a.Name = "A";
	a.Id = "a";
	a.Row = "bottom";
	a.Column = "right";
	pinned.Buttons.push_back(std::move(a));
	PluginButton b;
	b.Name = "B";
	b.Id = "b";
	b.Row = "top";
	b.Column = "left";
	pinned.Buttons.push_back(std::move(b));
	plugin.Toolbars.push_back(std::move(pinned));

	PluginToolbar legacy;
	legacy.Name = "Legacy";
	legacy.Id = "legacy";
	PluginButton first;
	first.Name = "First";
	legacy.Buttons.push_back(std::move(first));
	PluginButton second;
	second.Name = "Second";
	legacy.Buttons.push_back(std::move(second));
	plugin.Toolbars.push_back(std::move(legacy));

	std::vector<LoadedPlugin> plugins;
	plugins.push_back(std::move(plugin));
	const studio::ToolbarLayoutView layout = ComposeToolbar(plugins, {});
	REQUIRE(layout.PinnedRows.size() == 2);
	CHECK(layout.PinnedRows[0].Id.find("top") != std::string::npos);
	CHECK(layout.PinnedRows[1].Id.find("bottom") != std::string::npos);
	REQUIRE(layout.PinnedRows[1].Cells.size() == 1);
	REQUIRE(layout.PinnedRows[1].Cells.front().Items.size() == 1);
	CHECK(layout.PinnedRows[1].Cells.front().Items.front().Width == 180.0f);
	REQUIRE(layout.Tabs.size() == 1);
	REQUIRE(layout.Tabs.front().Rows.size() == 1);
	REQUIRE(layout.Tabs.front().Rows.front().Cells.size() == 2);
	CHECK(ItemsOf(layout.Tabs.front()).size() == 2);

	ToolbarPreferences hidden;
	hidden.Tabs.push_back(
		ToolbarTabPreference{
			studio::PluginToolbarKey(plugins.front(), plugins.front().Toolbars.front(), 0),
			"Pinned",
			false,
			false,
			PluginToolbarPlacement::Pinned,
			0,
		}
	);
	const studio::ToolbarLayoutView hiddenLayout = ComposeToolbar(plugins, hidden);
	CHECK(hiddenLayout.PinnedRows.empty());
	REQUIRE(hiddenLayout.Tabs.size() == 1);
	CHECK(hiddenLayout.Tabs.front().Name == "Legacy");
}

TEST_CASE("toolbar placement text is strict and stable", "[studio][plugins]") {
	CHECK(std::string(studio::Describe(PluginToolbarPlacement::Tabbed)) == "Tabbed");
	CHECK(std::string(studio::Describe(PluginToolbarPlacement::Pinned)) == "Pinned");
	CHECK(studio::ParsePluginToolbarPlacement("tabbed") == PluginToolbarPlacement::Tabbed);
	CHECK(studio::ParsePluginToolbarPlacement("Pinned") == PluginToolbarPlacement::Pinned);
	CHECK_FALSE(studio::ParsePluginToolbarPlacement("floating").has_value());
}

// --- discovery -----------------------------------------------------------------

TEST_CASE("plugins are discovered in folder order", "[studio][plugins]") {
	Folder folder;
	folder.Add("charlie", R"({"name": "Charlie"})", "return\n");
	folder.Add("alpha", R"({"name": "Alpha"})", "return\n");
	folder.Add("bravo", R"({"name": "Bravo"})", "return\n");

	// A folder with no manifest is somebody's notes, not a broken plugin.
	std::filesystem::create_directories(folder.Root / "notes");

	const std::vector<LoadedPlugin> found = DiscoverPlugins(folder.Root);
	REQUIRE(found.size() == 3);

	// **Sorted, because a directory walk is not.** Plugins run in this order and
	// one may build on what another left in the world.
	CHECK(found[0].Manifest.Name == "Alpha");
	CHECK(found[1].Manifest.Name == "Bravo");
	CHECK(found[2].Manifest.Name == "Charlie");

	// A folder that is not there at all is a fresh install, not an error.
	CHECK(DiscoverPlugins(folder.Root / "nowhere").empty());
}

TEST_CASE("a broken manifest is listed with its reason", "[studio][plugins]") {
	Folder folder;
	folder.Add("good", R"({"name": "Good"})", "return\n");
	folder.Add("broken", "{ this is not json", "return\n");

	const std::vector<LoadedPlugin> found = DiscoverPlugins(folder.Root);
	REQUIRE(found.size() == 2);

	// **Listed rather than skipped**, because a folder with a manifest is a
	// plugin and saying why it did not load is the whole point of walking it.
	const LoadedPlugin *broken = Named(found, "broken");
	REQUIRE(broken != nullptr);
	CHECK_FALSE(broken->Error.empty());
	CHECK_FALSE(broken->Running);
	CHECK_FALSE(broken->DefinitionValid);

	CHECK(Named(found, "Good") != nullptr);
}

TEST_CASE("duplicate plugin identities remain blocked until rediscovery", "[studio][plugins]") {
	Folder folder;
	folder.Add("first", R"({"name":"First","id":"shared"})", "return\n");
	folder.Add("second", R"({"name":"Second","id":"shared"})", "return\n");

	const std::vector<LoadedPlugin> found = DiscoverPlugins(folder.Root);
	REQUIRE(found.size() == 2);
	CHECK_FALSE(found[0].DefinitionValid);
	CHECK_FALSE(found[1].DefinitionValid);
	CHECK(found[0].Error.find("duplicate plugin id") != std::string::npos);
	CHECK(found[1].Error == found[0].Error);
}

// --- running -------------------------------------------------------------------

TEST_CASE("script plugins start only in selected Studio playtest roles", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;
	folder.Add(
		"server",
		R"({"name":"Server Tool","runs":"playtest-server"})",
		"assert(game:GetService('RunService'):IsServer())\n"
	);

	Store studioStore("studio-context");
	std::vector<LoadedPlugin> studioPlugins = DiscoverPlugins(folder.Root);
	StartPlugins(studioPlugins, studioStore);
	REQUIRE(studioPlugins.size() == 1);
	CHECK_FALSE(studioPlugins.front().Running);
	CHECK(studioPlugins.front().Error.find("does not run") != std::string::npos);

	Store serverStore("server-context");
	std::vector<LoadedPlugin> serverPlugins = DiscoverPlugins(folder.Root);
	StartPlugins(serverPlugins, serverStore, {}, PluginRunTarget::PlaytestServer);
	REQUIRE(serverPlugins.size() == 1);
	INFO(serverPlugins.front().Error);
	REQUIRE(serverPlugins.front().Running);
	CHECK(serverPlugins.front().Vm->Role().Server);
	CHECK_FALSE(serverPlugins.front().Vm->Role().Client);
	CHECK(serverPlugins.front().Vm->Role().Studio);

	Folder clientFolder;
	clientFolder.Add(
		"client",
		R"({"name":"Client Tool","runs":"playtest-client"})",
		"assert(game:GetService('RunService'):IsClient())\n"
	);
	Store clientStore("client-context");
	std::vector<LoadedPlugin> clientPlugins = DiscoverPlugins(clientFolder.Root);
	StartPlugins(clientPlugins, clientStore, {}, PluginRunTarget::PlaytestClient);
	REQUIRE(clientPlugins.size() == 1);
	INFO(clientPlugins.front().Error);
	REQUIRE(clientPlugins.front().Running);
	CHECK_FALSE(clientPlugins.front().Vm->Role().Server);
	CHECK(clientPlugins.front().Vm->Role().Client);
	CHECK(clientPlugins.front().Vm->Role().Studio);
}

TEST_CASE("every plugin gets its own globals", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	folder.Add("one", R"({"name": "One"})", "shared = 'one'\n");
	folder.Add("two", R"({"name": "Two"})", "assert(shared == nil, 'saw another plugin\\'s global')\n");

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store);

	// **The assertion is inside the second plugin.** Two plugins sharing a
	// runtime would share a global table, and the one that ran second would see
	// what the first left - so this fails by the second plugin refusing to
	// start rather than by anything here.
	for (const LoadedPlugin &plugin : plugins) {
		INFO(plugin.Manifest.Name << ": " << plugin.Error);
		CHECK(plugin.Running);
	}
}

TEST_CASE("a plugin that will not start does not stop the others", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	folder.Add("alpha", R"({"name": "Alpha"})", "local x = 1\n");
	folder.Add("bravo", R"({"name": "Bravo"})", "this is not luau at all ((\n");
	folder.Add("charlie", R"({"name": "Charlie"})", "local y = 2\n");
	folder.Add("delta", R"({"name": "Delta", "main": "missing.luau"})", "local z = 3\n");
	folder.Add("echo", R"({"name": "Echo", "enabled": false})", "local w = 4\n");

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store);

	CHECK(Named(plugins, "Alpha")->Running);
	CHECK(Named(plugins, "Charlie")->Running);

	// Each failure carries its own reason, which is what tells an author which
	// of five plugins is at fault.
	const LoadedPlugin *bravo = Named(plugins, "Bravo");
	CHECK_FALSE(bravo->Running);
	CHECK_FALSE(bravo->Error.empty());

	const LoadedPlugin *delta = Named(plugins, "Delta");
	CHECK_FALSE(delta->Running);
	CHECK(delta->Error.find("missing.luau") != std::string::npos);

	// Switched off is a state, not a failure - it stays on disk and stays listed.
	const LoadedPlugin *echo = Named(plugins, "Echo");
	CHECK_FALSE(echo->Running);
	CHECK(echo->Error == "switched off");
}

TEST_CASE("a plugin that throws every beat is switched off", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	folder.Add(
		"faulty",
		R"({"name": "Faulty"})",
		"game:GetService('RunService').Heartbeat:Connect(function()\n"
		"  error('every frame')\n"
		"end)\n"
	);
	folder.Add(
		"steady",
		R"({"name": "Steady"})",
		"beats = 0\n"
		"game:GetService('RunService').Heartbeat:Connect(function()\n"
		"  beats += 1\n"
		"end)\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store);

	REQUIRE(Named(plugins, "Faulty")->Running);
	REQUIRE(Named(plugins, "Steady")->Running);

	for (size_t beat = 0; beat < PLUGIN_FAULT_LIMIT + 2; beat++) {
		BeatPlugins(plugins, 1.0f / 60.0f);
	}

	// **Off rather than logged sixty times a second**, which is the whole of the
	// fault limit: a plugin whose heartbeat raises does it again next frame and
	// every frame after.
	const LoadedPlugin *faulty = Named(plugins, "Faulty");
	CHECK_FALSE(faulty->Running);
	CHECK(faulty->Faults >= PLUGIN_FAULT_LIMIT);
	CHECK_FALSE(faulty->Error.empty());

	// And the one beside it is untouched, which is the property that matters.
	CHECK(Named(plugins, "Steady")->Running);

	// A stopped plugin is not beaten again.
	CHECK(BeatPlugins(plugins, 1.0f / 60.0f) == 1);
}

TEST_CASE("restarting a plugin drops callbacks owned by its old runtime", "[studio][plugins]") {
	LoadedPlugin plugin;
	plugin.Manifest.Name = "Rejected";
	plugin.Error = "invalid definition";
	plugin.DefinitionValid = false;
	plugin.OnUndo = engine::script::HostCallback{11};
	plugin.OnRedo = engine::script::HostCallback{12};
	plugin.OnRecordingStarted = engine::script::HostCallback{13};
	plugin.OnRecordingFinished = engine::script::HostCallback{14};

	std::vector<LoadedPlugin> plugins;
	plugins.push_back(std::move(plugin));
	Store store("callbacks");
	StartPlugins(plugins, store);

	CHECK_FALSE(plugins.front().OnUndo.Valid());
	CHECK_FALSE(plugins.front().OnRedo.Valid());
	CHECK_FALSE(plugins.front().OnRecordingStarted.Valid());
	CHECK_FALSE(plugins.front().OnRecordingFinished.Valid());
}

// --- the selection bridge ------------------------------------------------------

TEST_CASE("a plugin reads the selection as a component", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	REQUIRE(RegisterSelectionComponent());

	// **A tag, so the query is the whole API.** There is no selection function
	// for a plugin to call, and this is why: a selection is per-entity state
	// about the world, which is what a component is for.
	const engine::ecs::ComponentId id = engine::ecs::Components::Find(Name(std::string(SELECTED_COMPONENT)));
	REQUIRE(id.IsValid());
	REQUIRE(engine::ecs::Schemas::Of(id) != nullptr);
	CHECK(engine::ecs::Schemas::Of(id)->Fields().empty());

	Store store("plugins");
	const Entity chosen = store.CreateInstance(engine::scene::PartClass(), "Chosen");
	const Entity ignored = store.CreateInstance(engine::scene::PartClass(), "Ignored");

	store.SetComponent(chosen, id, nullptr);

	const engine::ecs::ComponentId terms[] = {id};
	CHECK(store.CountMatching(terms) == 1);

	std::vector<Entity> found;
	store.EachMatching(terms, [&found](Entity entity) { found.push_back(entity); });
	REQUIRE(found.size() == 1);
	CHECK(found.front() == chosen);
	CHECK(found.front() != ignored);

	// And a plugin can change it, which is what makes the bridge two-way.
	Folder folder;
	folder.Add(
		"selector",
		R"({"name": "Selector"})",
		"local chosen = World:Query('studio.Selected')\n"
		"assert(#chosen == 1, 'the selection did not cross, got ' .. #chosen)\n"
		"assert(chosen[1].Name == 'Chosen', 'the wrong instance crossed')\n"
	);

	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store);

	INFO(plugins.front().Error);
	CHECK(plugins.front().Running);
}

// --- the editor's surface ------------------------------------------------------
//
// **A stand-in host rather than a live `Editor`.** Starting one needs a window,
// a renderer and a universe; what these cases are about is the *shape* a plugin
// sees - that a toolbar answers an id, that a button's handler is held and can
// be called, that the widget calls are refused outside a render. The editor's
// own implementation is `PluginSurface.cpp` and is exercised by running it.

namespace {
	using engine::script::HostArguments;
	using engine::script::HostCallback;
	using engine::script::HostSurface;
	using engine::script::HostTag;
	using engine::script::HostValue;

	// The same surface shape `PluginSurface` offers, over a plugin's own record.
	class Surface final : public HostSurface {
	  public:
		explicit Surface(LoadedPlugin &plugin) : Plugin(plugin) {}

		std::string_view GlobalName() const override {
			return "plugin";
		}

		std::vector<std::string> Names() const override {
			return {
				"CreateToolbar",
				"CreateToolbarTab",
				"CreateToolbarRow",
				"CreateToolbarColumn",
				"CreateButton",
				"CreateLabel",
				"SetToolCell",
				"CreateWidget",
				"SetWidgetRender",
				"Label",

				// A dotted name is a service, which is how the editor offers
				// `Selection` - see `PluginSurface.cpp`.
				"Selection.Get",
				"Selection.Set",
			};
		}

		bool Call(
			std::string_view name, HostArguments arguments, HostValue &result, std::string &failure
		) override {
			const auto text = [&](size_t at) {
				return at < arguments.size() ? std::string(arguments[at].AsText()) : std::string{};
			};

			if (name == "CreateToolbar" || name == "CreateToolbarTab") {
				PluginToolbar toolbar;
				toolbar.Name = text(0);
				if (name == "CreateToolbarTab" && text(2) == "Pinned") {
					toolbar.Placement = PluginToolbarPlacement::Pinned;
				}
				Plugin.Toolbars.push_back(std::move(toolbar));
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars.size()));
				return true;
			}
			if (name == "CreateToolbarRow" || name == "CreateToolbarColumn") {
				const auto bar = static_cast<size_t>(arguments[0].AsNumber(0.0));
				if (bar < 1 || bar > Plugin.Toolbars.size()) {
					failure = "no such toolbar";
					return false;
				}
				auto &tracks = name == "CreateToolbarRow" ? Plugin.Toolbars[bar - 1].Rows
														  : Plugin.Toolbars[bar - 1].Columns;
				const float width = name == "CreateToolbarColumn" && arguments.size() > 2 &&
											arguments[2].Tag == HostTag::Number
										? static_cast<float>(arguments[2].Number)
										: 0.0f;
				tracks.push_back(PluginToolbarTrack{text(1), width});
				result = HostValue::Of(static_cast<double>(tracks.size()));
				return true;
			}
			if (name == "CreateButton") {
				const auto bar = static_cast<size_t>(arguments[0].AsNumber(0.0));
				if (bar < 1 || bar > Plugin.Toolbars.size()) {
					failure = "no such toolbar";
					return false;
				}

				PluginButton button;
				button.Name = text(1);
				button.Tooltip = text(2);
				if (arguments.size() > 3 && arguments[3].Tag == HostTag::Callback) {
					button.OnClick = arguments[3].Callback;
				}

				Plugin.Toolbars[bar - 1].Buttons.push_back(button);
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars[bar - 1].Buttons.size()));
				return true;
			}
			if (name == "CreateLabel") {
				const auto bar = static_cast<size_t>(arguments[0].AsNumber(0.0));
				if (bar < 1 || bar > Plugin.Toolbars.size()) {
					failure = "no such toolbar";
					return false;
				}
				PluginButton label;
				label.Name = text(1);
				label.Kind = PluginControlKind::Label;
				Plugin.Toolbars[bar - 1].Buttons.push_back(std::move(label));
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars[bar - 1].Buttons.size()));
				return true;
			}
			if (name == "SetToolCell") {
				const auto bar = static_cast<size_t>(arguments[0].AsNumber(0.0));
				const auto tool = static_cast<size_t>(arguments[1].AsNumber(0.0));
				const auto row = static_cast<size_t>(arguments[2].AsNumber(0.0));
				const auto column = static_cast<size_t>(arguments[3].AsNumber(0.0));
				if (bar < 1 || bar > Plugin.Toolbars.size() || tool < 1 ||
					tool > Plugin.Toolbars[bar - 1].Buttons.size() || row < 1 ||
					row > Plugin.Toolbars[bar - 1].Rows.size() || column < 1 ||
					column > Plugin.Toolbars[bar - 1].Columns.size()) {
					failure = "no such toolbar cell";
					return false;
				}
				PluginButton &button = Plugin.Toolbars[bar - 1].Buttons[tool - 1];
				button.Row = Plugin.Toolbars[bar - 1].Rows[row - 1].Id;
				button.Column = Plugin.Toolbars[bar - 1].Columns[column - 1].Id;
				return true;
			}
			if (name == "CreateWidget") {
				PluginWidget widget;
				widget.Title = text(0);
				widget.Open = true;
				Plugin.Widgets.push_back(std::move(widget));
				result = HostValue::Of(static_cast<double>(Plugin.Widgets.size()));
				return true;
			}
			if (name == "SetWidgetRender") {
				const auto at = static_cast<size_t>(arguments[0].AsNumber(0.0));
				if (at < 1 || at > Plugin.Widgets.size() || arguments[1].Tag != HostTag::Callback) {
					failure = "SetWidgetRender takes a widget and a function";
					return false;
				}
				Plugin.Widgets[at - 1].Render = arguments[1].Callback;
				return true;
			}
			if (name == "Selection.Get") {
				std::vector<HostValue> held;
				for (const std::string &selected : Selected) {
					held.push_back(HostValue::Of(std::string_view(selected)));
				}
				result = HostValue::List(std::move(held));
				return true;
			}
			if (name == "Selection.Set") {
				// The same rule `PluginSurface` applies: an array of the right
				// kind of thing, validated whole before anything changes.
				const HostValue &value = arguments.empty() ? HostValue{} : arguments[0];

				if (value.Tag != HostTag::Array) {
					failure = std::string("Selection:Set expects an array, and was given ") +
							  engine::script::Describe(value.Tag);
					return false;
				}

				// **Skipped rather than refused**, which is the editor's rule:
				// the argument being the wrong shape is the call being wrong,
				// and an item that has gone is the world having moved on.
				std::vector<std::string> wanted;
				for (const HostValue &item : value.Items) {
					if (item.Tag != HostTag::String) {
						Ignored++;
						continue;
					}
					wanted.emplace_back(item.AsText());
				}

				Selected = std::move(wanted);
				return true;
			}
			if (name == "Label") {
				if (!Drawing) {
					failure = "Label may only be called while a widget is drawing";
					return false;
				}
				Drawn.push_back(text(0));
				return true;
			}
			return false;
		}

		LoadedPlugin &Plugin;
		bool Drawing = false;
		std::vector<std::string> Drawn;
		std::vector<std::string> Selected;
		size_t Ignored = 0;
	};
}

TEST_CASE("a plugin creates toolbars and buttons at its top level", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	folder.Add(
		"tools",
		R"({"name": "Tools"})",
		"local bar = plugin.CreateToolbar('My Tools')\n"
		"assert(bar == 1, 'the first toolbar is 1, got ' .. tostring(bar))\n"
		"pressed = 0\n"
		"plugin.CreateButton(bar, 'Align', 'Align the selection', function()\n"
		"  pressed += 1\n"
		"end)\n"
		"plugin.CreateButton(bar, 'Clear', '', function() end)\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);

	Surface *surface = nullptr;
	StartPlugins(plugins, store, [&surface](LoadedPlugin &plugin) {
		auto made = std::make_unique<Surface>(plugin);
		surface = made.get();
		return made;
	});

	REQUIRE(plugins.size() == 1);
	INFO(plugins.front().Error);
	REQUIRE(plugins.front().Running);

	// **The top level is where a plugin builds its toolbar**, which is why the
	// host is installed before the entry script rather than after it.
	REQUIRE(plugins.front().Toolbars.size() == 1);
	CHECK(plugins.front().Toolbars[0].Name == "My Tools");
	REQUIRE(plugins.front().Toolbars[0].Buttons.size() == 2);
	CHECK(plugins.front().Toolbars[0].Buttons[0].Name == "Align");
	CHECK(plugins.front().Toolbars[0].Buttons[0].Tooltip == "Align the selection");

	// The handler crossed and can be called from the editor's frame, which is
	// the whole reason a button is possible at all.
	const HostCallback click = plugins.front().Toolbars[0].Buttons[0].OnClick;
	REQUIRE(click.Valid());
	CHECK(plugins.front().Vm->Invoke(click, {}));
	CHECK(plugins.front().Vm->Invoke(click, {}));

	(void)surface;
}

TEST_CASE("Luau and JavaScript plugins configure the same viewport grid", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;
	folder.AddWithMain(
		"grid-luau",
		R"({"name":"Grid Luau","main":"main.luau"})",
		"main.luau",
		"plugin.SetViewportOption('Grid Step', 2.5)\n"
		"plugin.SetViewportOption('Grid Major', 6)\n"
		"plugin.SetViewportOption('Grid Offset X', 12)\n"
		"plugin.SetViewportOption('Grid Colour', '#336699')\n"
		"plugin.SetViewportOption('Particles', false)\n"
		"local bar = plugin.CreateToolbar('Grid Controls')\n"
		"plugin.CreateDropdown(bar, 'Spacing', '', {'Fine', 'Coarse'}, 1, function() end)\n"
		"assert(plugin.GetViewportOption('Grid Step') == 2.5)\n"
		"assert(plugin.GetViewportOption('Grid Major') == 6)\n"
		"assert(plugin.GetViewportOption('Grid Offset X') == 12)\n"
		"assert(plugin.GetViewportOption('Grid Colour') == '336699FF')\n"
		"assert(plugin.GetViewportOption('Particles') == false)\n"
	);
	folder.AddWithMain(
		"grid-js",
		R"({"name":"Grid JavaScript","main":"main.js"})",
		"main.js",
		"plugin.SetViewportOption('Grid Scale', 3.5);\n"
		"plugin.SetViewportOption('Grid Size', 900);\n"
		"plugin.SetViewportOption('Grid Offset Z', -24);\n"
		"plugin.SetViewportOption('Grid Axis X Color', '#CC3344');\n"
		"const bar = plugin.CreateToolbar('Grid Controls JS');\n"
		"plugin.CreateDropdown(bar, 'Spacing', '', ['Fine', 'Coarse'], 2, function() {});\n"
		"if (plugin.GetViewportOption('Grid Scale') !== 3.5) throw new Error('step');\n"
		"if (plugin.GetViewportOption('Grid Size') !== 900) throw new Error('reach');\n"
		"if (plugin.GetViewportOption('Grid Offset Z') !== -24) throw new Error('offset');\n"
		"if (plugin.GetViewportOption('Grid Axis X Colour') !== 'CC3344FF') throw new Error('colour');\n"
	);

	studio::Editor editor;
	Store store("plugin-grid-options");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store, [&editor](LoadedPlugin &plugin) {
		return studio::MakePluginSurface(editor, plugin);
	});

	REQUIRE(plugins.size() == 2);
	for (const LoadedPlugin &plugin : plugins) {
		INFO(plugin.Manifest.Name << ": " << plugin.Error);
		CHECK(plugin.Running);
		REQUIRE(plugin.Toolbars.size() == 1);
		REQUIRE(plugin.Toolbars.front().Buttons.size() == 1);
		CHECK(plugin.Toolbars.front().Buttons.front().Kind == PluginControlKind::Dropdown);
		CHECK(plugin.Toolbars.front().Buttons.front().Options.size() == 2);
	}
}

TEST_CASE("a Luau plugin declares a pinned toolbar grid and label", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;
	folder.Add(
		"grid-tools",
		R"({"name": "Grid Tools"})",
		"local bar = plugin.CreateToolbarTab('Transport', 'transport', 'Pinned')\n"
		"local row = plugin.CreateToolbarRow(bar, 'primary')\n"
		"local left = plugin.CreateToolbarColumn(bar, 'left')\n"
		"local right = plugin.CreateToolbarColumn(bar, 'right', 240)\n"
		"local action = plugin.CreateButton(bar, 'Act', '', function() end)\n"
		"local status = plugin.CreateLabel(bar, 'Ready')\n"
		"plugin.SetToolCell(bar, action, row, left)\n"
		"plugin.SetToolCell(bar, status, row, right)\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store, [](LoadedPlugin &plugin) { return std::make_unique<Surface>(plugin); });

	INFO(plugins.front().Error);
	REQUIRE(plugins.front().Running);
	REQUIRE(plugins.front().Toolbars.size() == 1);
	const PluginToolbar &toolbar = plugins.front().Toolbars.front();
	CHECK(toolbar.Placement == PluginToolbarPlacement::Pinned);
	REQUIRE(toolbar.Rows.size() == 1);
	REQUIRE(toolbar.Columns.size() == 2);
	REQUIRE(toolbar.Buttons.size() == 2);
	CHECK(toolbar.Buttons[0].Row == "primary");
	CHECK(toolbar.Buttons[0].Column == "left");
	CHECK(toolbar.Columns[1].Width == 240.0f);
	CHECK(toolbar.Buttons[1].Kind == PluginControlKind::Label);
	CHECK(toolbar.Buttons[1].Column == "right");
}

TEST_CASE("a widget renders through its callback and only then", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	folder.Add(
		"panel",
		R"({"name": "Panel"})",
		"local widget = plugin.CreateWidget('Align', true)\n"
		"plugin.SetWidgetRender(widget, function()\n"
		"  plugin.Label('drawn')\n"
		"end)\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);

	Surface *surface = nullptr;
	StartPlugins(plugins, store, [&surface](LoadedPlugin &plugin) {
		auto made = std::make_unique<Surface>(plugin);
		surface = made.get();
		return made;
	});

	REQUIRE(plugins.front().Running);
	REQUIRE(surface != nullptr);
	REQUIRE(plugins.front().Widgets.size() == 1);
	CHECK(plugins.front().Widgets[0].Title == "Align");
	CHECK(plugins.front().Widgets[0].Open);

	const HostCallback render = plugins.front().Widgets[0].Render;
	REQUIRE(render.Valid());

	// **Inside the gate**, which is where the editor calls it from.
	surface->Drawing = true;
	CHECK(plugins.front().Vm->Invoke(render, {}));
	surface->Drawing = false;

	REQUIRE(surface->Drawn.size() == 1);
	CHECK(surface->Drawn.front() == "drawn");

	// **Outside it the same call is refused**, because a `Label` from a
	// heartbeat would draw into whatever window the editor was building. The
	// invoke fails, which is what the fault counter reads.
	CHECK_FALSE(plugins.front().Vm->Invoke(render, {}));
	CHECK(surface->Drawn.size() == 1);
}

TEST_CASE("a plugin that misuses the surface is refused, not crashed", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	// Every one of these is a plugin author's ordinary mistake, and each has to
	// arrive as a script error rather than as an editor that is no longer there.
	folder.Add(
		"wrong",
		R"({"name": "Wrong"})",
		"local ok, message = pcall(function() plugin.CreateButton(99, 'x', '', function() end) end)\n"
		"assert(not ok, 'a button on a toolbar that does not exist was accepted')\n"
		"assert(string.find(message, 'toolbar') ~= nil, 'the reason was lost: ' .. message)\n"
		"\n"
		"local caught = pcall(function() plugin.Label('outside a widget') end)\n"
		"assert(not caught, 'a widget call from the top level was accepted')\n"
		"\n"
		"assert(plugin.NoSuchThing == nil, 'an unlisted name is a member')\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);
	StartPlugins(plugins, store, [](LoadedPlugin &plugin) { return std::make_unique<Surface>(plugin); });

	INFO(plugins.front().Error);
	CHECK(plugins.front().Running);
}

TEST_CASE("the selection is a service, not a plugin call", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	// **Roblox's shape exactly**, which is the point of moving it: somebody who
	// has written a Studio plugin already types this.
	folder.Add(
		"selection",
		R"({"name": "Selection user"})",
		"local Selection = game:GetService('Selection')\n"
		"assert(Selection == _G_unused or type(Selection) == 'table', 'no Selection service')\n"
		"assert(type(Selection.Get) == 'function', 'Selection has no Get')\n"
		"\n"
		"Selection:Set({ 'a', 'b' })\n"
		"local held = Selection:Get()\n"
		"assert(#held == 2, 'the selection did not come back, got ' .. #held)\n"
		"assert(held[1] == 'a', 'the wrong entry')\n"
		"\n"
		"-- **An empty array deselects everything**, which is the whole of what\n"
		"-- it means. It was refused before an empty table read as an array.\n"
		"Selection:Set({})\n"
		"assert(#Selection:Get() == 0, 'an empty array did not deselect')\n"
		"\n"
		"-- And it is not on the plugin table any more.\n"
		"assert(plugin.GetSelection == nil, 'GetSelection is still a plugin call')\n"
		"assert(plugin.SetSelection == nil, 'SetSelection is still a plugin call')\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);

	Surface *surface = nullptr;
	StartPlugins(plugins, store, [&surface](LoadedPlugin &plugin) {
		auto made = std::make_unique<Surface>(plugin);
		surface = made.get();
		return made;
	});

	INFO(plugins.front().Error);
	REQUIRE(plugins.front().Running);
	REQUIRE(surface != nullptr);
	CHECK(surface->Selected.empty());
}

TEST_CASE("Selection refuses the wrong shape and skips the wrong items", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	Folder folder;

	// **Two mistakes, two answers.** The argument being the wrong shape is the
	// call being wrong and is refused; an item that is not selectable is the
	// world having moved on and is skipped with a warning, so a plugin
	// selecting the results of a query it ran three frames ago ends up with the
	// ones that are still there.
	folder.Add(
		"strict",
		R"({"name": "Strict"})",
		"local Selection = game:GetService('Selection')\n"
		"\n"
		"local function refused(body)\n"
		"  local ok, message = pcall(body)\n"
		"  assert(not ok, 'that was accepted and should not have been')\n"
		"  return message\n"
		"end\n"
		"\n"
		"-- The shape. Every one of these is what somebody types before they\n"
		"-- read anything, and each has to arrive as a message.\n"
		"local bare = refused(function() Selection:Set('a') end)\n"
		"assert(string.find(bare, 'array') ~= nil, 'the reason was lost: ' .. bare)\n"
		"assert(string.find(bare, 'string') ~= nil, 'it did not say what it got: ' .. bare)\n"
		"\n"
		"refused(function() Selection:Set(nil) end)\n"
		"refused(function() Selection:Set() end)\n"
		"refused(function() Selection:Set(7) end)\n"
		"refused(function() Selection:Set({ a = 1 }) end)\n"
		"\n"
		"-- The items. A bad one does not fail the call; the good ones land.\n"
		"Selection:Set({ 'a', 7, 'c', false })\n"
		"local held = Selection:Get()\n"
		"assert(#held == 2, 'the good items did not land, got ' .. #held)\n"
		"assert(held[1] == 'a' and held[2] == 'c', 'the wrong ones landed')\n"
		"\n"
		"-- **An array whose every item was skipped clears it**, which is the\n"
		"-- honest reading: the plugin asked for a selection of things that are\n"
		"-- not there.\n"
		"Selection:Set({ 1, 2, 3 })\n"
		"assert(#Selection:Get() == 0, 'a wholly bad list left the old selection')\n"
	);

	Store store("plugins");
	std::vector<LoadedPlugin> plugins = DiscoverPlugins(folder.Root);

	Surface *surface = nullptr;
	StartPlugins(plugins, store, [&surface](LoadedPlugin &plugin) {
		auto made = std::make_unique<Surface>(plugin);
		surface = made.get();
		return made;
	});

	INFO(plugins.front().Error);
	REQUIRE(plugins.front().Running);
	REQUIRE(surface != nullptr);
	CHECK(surface->Selected.empty());

	// Five items were skipped across the two calls, which is what the warning
	// counts.
	CHECK(surface->Ignored == 5);
}
