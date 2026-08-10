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
#include <engine/testing/Suite.hpp>
#include <studio/Config.hpp>
#include <engine/script/Host.hpp>
#include <studio/Plugins.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <memory>
#include <vector>

TEST_SUITE_ID("studio.plugins")
TEST_DEPENDS("engine.script.scripting")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using studio::BeatPlugins;
using studio::DiscoverPlugins;
using studio::LoadedPlugin;
using studio::ParsePluginManifest;
using studio::PluginManifest;
using studio::PLUGIN_FAULT_LIMIT;
using studio::RegisterSelectionComponent;
using studio::SELECTED_COMPONENT;
using studio::PluginButton;
using studio::PluginToolbar;
using studio::PluginWidget;
using studio::StartPlugins;

namespace {
	// A plugins folder, written per case and removed after it.
	struct Folder {
		std::filesystem::path Root;

		Folder() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-studio-plugins-" +
					std::to_string(std::filesystem::hash_value(
						std::filesystem::temp_directory_path() / "studio-plugins"
					)));
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
	};

	const LoadedPlugin *Named(const std::vector<LoadedPlugin> &plugins, std::string_view name) {
		for (const LoadedPlugin &plugin : plugins) {
			if (plugin.Manifest.Name == name) {
				return &plugin;
			}
		}
		return nullptr;
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

	CHECK(Named(found, "Good") != nullptr);
}

// --- running -------------------------------------------------------------------

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
	// what the first left — so this fails by the second plugin refusing to
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

	// Switched off is a state, not a failure — it stays on disk and stays listed.
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

// --- the selection bridge ------------------------------------------------------

TEST_CASE("a plugin reads the selection as a component", "[studio][plugins]") {
	engine::scene::EnsureClassTree();
	REQUIRE(RegisterSelectionComponent());

	// **A tag, so the query is the whole API.** There is no selection function
	// for a plugin to call, and this is why: a selection is per-entity state
	// about the world, which is what a component is for.
	const engine::ecs::ComponentId id =
		engine::ecs::Components::Find(Name(std::string(SELECTED_COMPONENT)));
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
// sees — that a toolbar answers an id, that a button's handler is held and can
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
				"CreateButton",
				"CreateWidget",
				"SetWidgetRender",
				"Label",

				// A dotted name is a service, which is how the editor offers
				// `Selection` — see `PluginSurface.cpp`.
				"Selection.Get",
				"Selection.Set",
			};
		}

		bool Call(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure)
			override {
			const auto text = [&](size_t at) {
				return at < arguments.size() ? std::string(arguments[at].AsText()) : std::string{};
			};

			if (name == "CreateToolbar") {
				Plugin.Toolbars.push_back(PluginToolbar{text(0), {}});
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars.size()));
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
			if (name == "CreateWidget") {
				Plugin.Widgets.push_back(PluginWidget{text(0), true, {}});
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
				Selected.clear();
				if (!arguments.empty()) {
					for (const HostValue &item : arguments[0].Items) {
						Selected.emplace_back(item.AsText());
					}
				}
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
	CHECK(surface->Selected.size() == 2);
}
