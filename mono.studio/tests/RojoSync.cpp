// Reading a Rojo project and building the tree it describes.
//
// **The parser and the builder are the halves that can be silently wrong**, and
// they fail in the way that is hardest to notice: a sync that produced half a
// tree looks exactly like a project that only had half a tree in it. So the
// cases below assert what came out rather than that nothing threw.
//
// The tree is built on a scratch directory laid out the way a real project is,
// rather than against a checked-in fixture — a fixture would be a second copy of
// somebody else's format and would go stale the first time Rojo changed.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <studio/RojoSync.hpp>

TEST_SUITE_ID("studio.rojosync")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using studio::ParseRojoProject;
using studio::RojoProject;
using studio::RojoSyncReport;
using studio::SyncRojoProject;

namespace {
	constexpr const char *PROJECT = R"({
	  "name": "Example",
	  "tree": {
	    "$className": "DataModel",
	    "ReplicatedStorage": {
	      "$className": "ReplicatedStorage",
	      "Shared": { "$className": "Folder", "$path": "src/shared" }
	    },
	    "ServerScriptService": {
	      "$className": "ServerScriptService",
	      "Server": { "$className": "Folder", "$path": "src/server" }
	    }
	  }
	})";

	struct Tree {
		std::filesystem::path Root;

		Tree() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-rojo-" + std::to_string(std::filesystem::hash_value(
										 std::filesystem::temp_directory_path() / "rojo")));
			std::filesystem::remove_all(Root);

			Write("src/shared/Util.luau", "return {}\n");
			Write("src/shared/State/Machine.luau", "return {}\n");
			Write("src/shared/Config/init.luau", "return {}\n");
			Write("src/shared/Config/Values.luau", "return {}\n");
			Write("src/server/Boot.server.luau", "print('server')\n");
			Write("src/client/Hud.client.luau", "print('client')\n");
			Write("src/shared/notes.txt", "not a script\n");
		}

		~Tree() {
			std::filesystem::remove_all(Root);
		}

		void Write(const std::string &relative, const char *text) {
			const std::filesystem::path file = Root / relative;
			std::filesystem::create_directories(file.parent_path());
			std::ofstream out(file);
			out << text;
		}
	};

	// A child by name, so a case can say where something landed rather than
	// counting rows.
	Entity Child(const Store &store, Entity parent, const char *name) {
		return store.FindFirstChild(parent, name);
	}
}

TEST_CASE("a project file's tree is read in order", "[studio][rojosync]") {
	RojoProject project;
	std::string error;

	REQUIRE(ParseRojoProject(PROJECT, project, error));
	CHECK(error.empty());
	CHECK(project.Name == "Example");

	// **Order is kept.** Two syncs of one file have to create instances in one
	// order or their entity ids differ, and an id that moves is a saved
	// reference pointing somewhere else.
	REQUIRE(project.Tree.Children.size() == 2);
	CHECK(project.Tree.Children[0].Name == "ReplicatedStorage");
	CHECK(project.Tree.Children[1].Name == "ServerScriptService");

	// `$className` and `$path` are directives, not children.
	REQUIRE(project.Tree.Children[0].Children.size() == 1);
	CHECK(project.Tree.Children[0].Children[0].Name == "Shared");
	CHECK(project.Tree.Children[0].Children[0].Path == "src/shared");
}

TEST_CASE("a document with no tree is refused", "[studio][rojosync]") {
	// The direction to fail in: reading a `package.json` as a project file has
	// to say so rather than building an empty world and calling it a sync.
	RojoProject project;
	std::string error;

	CHECK_FALSE(ParseRojoProject(R"({"name": "no tree here"})", project, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("malformed json is refused rather than half-read", "[studio][rojosync]") {
	RojoProject project;
	std::string error;

	CHECK_FALSE(ParseRojoProject("{ this is not json", project, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("a directory becomes a folder and its scripts become scripts", "[studio][rojosync]") {
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));
	INFO(error);

	const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
	REQUIRE(replicated != NULL_ENTITY);

	const Entity shared = Child(store, replicated, "Shared");
	REQUIRE(shared != NULL_ENTITY);

	// **A plain `.luau` is a `ModuleScript`, not a `Script`.** Only the suffixed
	// files are programs a host runs; everything else in a project is something
	// a program requires, and mapping them all to `Script` would make a synced
	// project execute every library it contains.
	const Entity util = Child(store, shared, "Util");
	REQUIRE(util != NULL_ENTITY);
	CHECK(store.ClassOf(util) == engine::ecs::Classes::Find(Name("ModuleScript")));

	// A subdirectory is a folder, and its scripts hang under it rather than
	// being flattened into the parent.
	const Entity state = Child(store, shared, "State");
	REQUIRE(state != NULL_ENTITY);
	CHECK(Child(store, state, "Machine") != NULL_ENTITY);

	// And nothing was flattened: `Machine` is under `State`, not under `Shared`.
	CHECK(Child(store, shared, "Machine") == NULL_ENTITY);
}

TEST_CASE("init.luau makes the directory itself the script", "[studio][rojosync]") {
	// **The convention that looks like a special case and is not.** Without it a
	// module with sub-modules would be a folder beside a script and every path
	// in the project would gain a level.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity shared = Child(store, store.FindFirstRoot("ReplicatedStorage"), "Shared");
	REQUIRE(shared != NULL_ENTITY);

	const Entity config = Child(store, shared, "Config");
	REQUIRE(config != NULL_ENTITY);

	// It is a module, not a folder — and it kept its children. `init` is how a
	// module gets children, not how a folder becomes a program.
	CHECK(store.ClassOf(config) == engine::ecs::Classes::Find(Name("ModuleScript")));
	CHECK(Child(store, config, "Values") != NULL_ENTITY);

	// The `init.luau` did not also appear as a child called `init`.
	CHECK(Child(store, config, "init") == NULL_ENTITY);
}

TEST_CASE("a .client suffix makes a LocalScript", "[studio][rojosync]") {
	// The intent is in the file name, which is Rojo's convention — and a rule
	// based on which folder it sat in would disagree with the same file moved.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "StarterPlayer":{"$className":"StarterPlayer",
		      "Client":{"$className":"Folder","$path":"src/client"}}}})",
		project, error
	));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity client = Child(store, store.FindFirstRoot("StarterPlayer"), "Client");
	REQUIRE(client != NULL_ENTITY);

	// Named without the suffix, and a `LocalScript` rather than a `Script`.
	const Entity hud = Child(store, client, "Hud");
	REQUIRE(hud != NULL_ENTITY);
	CHECK(store.ClassOf(hud) == engine::ecs::Classes::Find(Name("LocalScript")));
}

TEST_CASE("a .server suffix makes a Script", "[studio][rojosync]") {
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity server = Child(store, store.FindFirstRoot("ServerScriptService"), "Server");
	REQUIRE(server != NULL_ENTITY);

	const Entity boot = Child(store, server, "Boot");
	REQUIRE(boot != NULL_ENTITY);
	CHECK(store.ClassOf(boot) == engine::ecs::Classes::Find(Name("Script")));
}

TEST_CASE("a script's text is staged where the runtime will look", "[studio][rojosync]") {
	// **The reason a synced project runs without anything being copied.** A Rojo
	// project lives wherever its author keeps it, which is not under the assets
	// root — and `ReadSource` checks the world's own table first.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const auto *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);

	const std::string *text = cache->Find(Name("src/shared/Util.luau"));
	REQUIRE(text != nullptr);
	CHECK(*text == "return {}\n");
}

TEST_CASE("a path the project names and disk does not have is reported", "[studio][rojosync]") {
	// **Reported, not fatal.** A project commonly names `Packages` before
	// anything has installed one, and refusing the other nine tenths over it
	// would make the feature unusable on a fresh clone.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "ReplicatedStorage":{"$className":"ReplicatedStorage",
		      "Shared":{"$className":"Folder","$path":"src/shared"},
		      "Packages":{"$className":"Folder","$path":"Packages"}}}})",
		project, error
	));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	// The present half built, and the absent half named.
	CHECK(report.Instances > 0);
	REQUIRE(report.Missing.size() == 1);
	CHECK(report.Missing.front() == "Packages");
}

TEST_CASE("a class this engine does not have becomes a folder, and says so", "[studio][rojosync]") {
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "Teams":{"$className":"Teams"}}})",
		project, error
	));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity teams = store.FindFirstRoot("Teams");
	REQUIRE(teams != NULL_ENTITY);
	CHECK(store.ClassOf(teams) == studio::FolderClass());

	// Substituted, not silently — an author whose `Teams` behaves like a folder
	// needs to be told rather than left to work it out.
	CHECK_FALSE(report.Notes.empty());
}

TEST_CASE("syncing twice does not duplicate the tree", "[studio][rojosync]") {
	// **`InstallServices` has already put `Workspace` and the rest in the
	// world.** A sync that made a second `ReplicatedStorage` beside the real one
	// would produce a tree where half the game cannot find the other half.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(PROJECT, project, error));

	RojoSyncReport first;
	REQUIRE(SyncRojoProject(project, tree.Root, store, first, error));

	size_t roots = 0;
	store.EachRoot([&roots](Entity) { roots++; });

	RojoSyncReport second;
	REQUIRE(SyncRojoProject(project, tree.Root, store, second, error));

	size_t after = 0;
	store.EachRoot([&after](Entity) { after++; });

	CHECK(after == roots);
}


TEST_CASE("the three script classes come from the three file shapes", "[studio][rojosync]") {
	// **The mapping in one case, because it is the whole contract with Rojo.**
	// Getting any row of this wrong is a project that runs the wrong half of
	// itself, and nothing about the tree would look unusual.
	Tree tree;
	Store store("rojo_test");
	(void)engine::scene::PartClass();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "ReplicatedStorage":{"$className":"ReplicatedStorage",
		      "Shared":{"$className":"Folder","$path":"src/shared"}},
		    "ServerScriptService":{"$className":"ServerScriptService",
		      "Server":{"$className":"Folder","$path":"src/server"}},
		    "StarterPlayer":{"$className":"StarterPlayer",
		      "Client":{"$className":"Folder","$path":"src/client"}}}})",
		project, error
	));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const auto classOf = [&](Entity parent, const char *name) {
		const Entity found = Child(store, parent, name);
		REQUIRE(found != NULL_ENTITY);
		return store.ClassOf(found);
	};

	const Entity shared = Child(store, store.FindFirstRoot("ReplicatedStorage"), "Shared");
	const Entity server = Child(store, store.FindFirstRoot("ServerScriptService"), "Server");
	const Entity client = Child(store, store.FindFirstRoot("StarterPlayer"), "Client");
	REQUIRE(shared != NULL_ENTITY);
	REQUIRE(server != NULL_ENTITY);
	REQUIRE(client != NULL_ENTITY);

	CHECK(classOf(shared, "Util") == engine::ecs::Classes::Find(Name("ModuleScript")));
	CHECK(classOf(server, "Boot") == engine::ecs::Classes::Find(Name("Script")));
	CHECK(classOf(client, "Hud") == engine::ecs::Classes::Find(Name("LocalScript")));
}

