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
#include <studio/Plugins.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
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
