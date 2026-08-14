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
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <studio/RojoSync.hpp>

TEST_SUITE_ID("studio.rojosync")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::MakeRuntime;
using engine::world::Universe;
using engine::world::WorldId;
using studio::ParseRojoProject;
using studio::ParseRojoUniverse;
using studio::RojoProject;
using studio::RojoSyncReport;
using studio::RojoUniverse;
using studio::RojoUniverseReport;
using studio::SyncRojoProject;
using studio::SyncRojoUniverse;

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
				   ("atomic-rojo-" +
					std::to_string(
						std::filesystem::hash_value(std::filesystem::temp_directory_path() / "rojo")
					));
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

	// A `.rbxm` written by this suite's own generator rather than by Studio.
	//
	// **A blob here and a builder in `bake/tests/RobloxModel.cpp`**, and the split
	// is the same one the two suites have: that one is about the *format* and
	// needs to bend every field, while this one is about the *mapping* and needs
	// one file that is definitely valid. A second writer here would be a second
	// thing to keep true about somebody else's format.
	//
	// It holds a `Model` called `Crate` containing three things, each chosen for
	// what it makes this suite able to assert:
	//
	// - a `Part` called `Lid`, carrying `Anchored`, `Size`, `Transparency`,
	//   `Color`, a **rotated** `CFrame`, a `Material` the reader refuses because
	//   it is an enum, and a `RootPriority` this engine has no property for;
	// - a `Script` called `Boot` whose `Source` is a `ProtectedString`;
	// - a `Chat`, which is a class Roblox has and this engine does not.
	constexpr std::array<uint8_t, 721> MODEL_RBXM{{
		0x3C, 0x72, 0x6F, 0x62, 0x6C, 0x6F, 0x78, 0x21, 0x89, 0xFF, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x4E,
		0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x05, 0x00, 0x00, 0x00, 0x4D, 0x6F, 0x64, 0x65, 0x6C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x49, 0x4E, 0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x50, 0x61, 0x72, 0x74, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x02, 0x49, 0x4E, 0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x53, 0x63, 0x72, 0x69, 0x70,
		0x74, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x49, 0x4E, 0x53, 0x54, 0x00, 0x00, 0x00,
		0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
		0x43, 0x68, 0x61, 0x74, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x50, 0x52, 0x4F, 0x50,
		0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x4E, 0x61, 0x6D, 0x65, 0x01, 0x05, 0x00, 0x00, 0x00, 0x43, 0x72, 0x61, 0x74, 0x65,
		0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x4E, 0x61, 0x6D, 0x65, 0x01, 0x03, 0x00, 0x00, 0x00, 0x4C,
		0x69, 0x64, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x41, 0x6E, 0x63, 0x68, 0x6F, 0x72, 0x65, 0x64,
		0x02, 0x01, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x53, 0x69, 0x7A, 0x65, 0x0E, 0x81, 0x00, 0x00,
		0x00, 0x7F, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00,
		0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x54,
		0x72, 0x61, 0x6E, 0x73, 0x70, 0x61, 0x72, 0x65, 0x6E, 0x63, 0x79, 0x04, 0x7E, 0x00, 0x00, 0x00, 0x50,
		0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x43, 0x6F, 0x6C, 0x6F, 0x72, 0x1A, 0xFF, 0x00, 0x00, 0x50, 0x52,
		0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x06, 0x00, 0x00, 0x00, 0x43, 0x46, 0x72, 0x61, 0x6D, 0x65, 0x10, 0x03, 0x7F, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00, 0x80, 0x80, 0x00, 0x00, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x15,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x4D, 0x61,
		0x74, 0x65, 0x72, 0x69, 0x61, 0x6C, 0x12, 0x00, 0x00, 0x01, 0x00, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00,
		0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00,
		0x00, 0x52, 0x6F, 0x6F, 0x74, 0x50, 0x72, 0x69, 0x6F, 0x72, 0x69, 0x74, 0x79, 0x03, 0x00, 0x00, 0x00,
		0x0E, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x4E, 0x61, 0x6D, 0x65, 0x01, 0x04, 0x00, 0x00, 0x00,
		0x42, 0x6F, 0x6F, 0x74, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x53, 0x6F, 0x75, 0x72, 0x63, 0x65,
		0x1D, 0x0F, 0x00, 0x00, 0x00, 0x70, 0x72, 0x69, 0x6E, 0x74, 0x28, 0x27, 0x68, 0x65, 0x6C, 0x6C, 0x6F,
		0x27, 0x29, 0x0A, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x4E, 0x61, 0x6D, 0x65, 0x01, 0x04, 0x00,
		0x00, 0x00, 0x43, 0x68, 0x61, 0x74, 0x50, 0x52, 0x4E, 0x54, 0x00, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x45, 0x4E, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}};

	// The same container holding two instances at its top level, which Rojo's
	// table maps to nothing: a model file is one instance.
	constexpr std::array<uint8_t, 175> TWO_ROOT_RBXM{{
		0x3C, 0x72, 0x6F, 0x62, 0x6C, 0x6F, 0x78, 0x21, 0x89, 0xFF, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x49, 0x4E, 0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x4D, 0x6F, 0x64, 0x65, 0x6C, 0x00, 0x02, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x50, 0x52, 0x4F, 0x50, 0x00, 0x00,
		0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
		0x00, 0x00, 0x4E, 0x61, 0x6D, 0x65, 0x01, 0x05, 0x00, 0x00, 0x00, 0x46, 0x69, 0x72, 0x73, 0x74,
		0x06, 0x00, 0x00, 0x00, 0x53, 0x65, 0x63, 0x6F, 0x6E, 0x64, 0x50, 0x52, 0x4E, 0x54, 0x00, 0x00,
		0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x45,
		0x4E, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}};

	void WriteBinary(
		const std::filesystem::path &root, const std::string &relative, std::span<const uint8_t> bytes
	) {
		const std::filesystem::path file = root / relative;
		std::filesystem::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary);
		out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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
	engine::scene::EnsureClassTree();

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
	engine::scene::EnsureClassTree();

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
	engine::scene::EnsureClassTree();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "StarterPlayer":{"$className":"StarterPlayer",
		      "Client":{"$className":"Folder","$path":"src/client"}}}})",
		project,
		error
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
	engine::scene::EnsureClassTree();

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
	engine::scene::EnsureClassTree();

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
	engine::scene::EnsureClassTree();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "ReplicatedStorage":{"$className":"ReplicatedStorage",
		      "Shared":{"$className":"Folder","$path":"src/shared"},
		      "Packages":{"$className":"Folder","$path":"Packages"}}}})",
		project,
		error
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
	engine::scene::EnsureClassTree();

	// **`Chat` rather than `Teams`, which this engine gained at v0.15.** The
	// case needs a class Roblox has and this build does not, so the example has
	// to move every time one of them lands — which is the point of the test
	// rather than an annoyance with it.
	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(
		R"({"name":"E","tree":{"$className":"DataModel",
		    "Chat":{"$className":"Chat"}}})",
		project,
		error
	));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity chat = store.FindFirstRoot("Chat");
	REQUIRE(chat != NULL_ENTITY);
	CHECK(store.ClassOf(chat) == studio::FolderClass());

	// Substituted, not silently — an author whose `Chat` behaves like a folder
	// needs to be told rather than left to work it out.
	CHECK_FALSE(report.Notes.empty());
}

TEST_CASE("syncing twice does not duplicate the tree", "[studio][rojosync]") {
	// **`InstallServices` has already put `Workspace` and the rest in the
	// world.** A sync that made a second `ReplicatedStorage` beside the real one
	// would produce a tree where half the game cannot find the other half.
	Tree tree;
	Store store("rojo_test");
	engine::scene::EnsureClassTree();

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
	engine::scene::EnsureClassTree();

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
		project,
		error
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

// --- the rest of Rojo's file table -------------------------------------------
//
// `rojo.space/docs/v7/sync-details` is the table these assert against. The three
// `init` forms are the half that was wrong: only `init.luau` was consumed, so a
// project using `init.server.luau` — which is most of them — got a folder plus a
// stray script called `init`.

namespace {
	// A tree whose only content is the file conventions under test.
	struct MappingTree {
		std::filesystem::path Root;

		MappingTree() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-rojo-mapping-" +
					std::to_string(
						std::filesystem::hash_value(std::filesystem::temp_directory_path() / "rojo-mapping")
					));
			std::filesystem::remove_all(Root);

			Write("src/Module/init.luau", "return {}\n");
			Write("src/Module/Leaf.luau", "return {}\n");
			Write("src/Server/init.server.luau", "print('server')\n");
			Write("src/Client/init.client.luau", "print('client')\n");
			Write("src/Legacy/init.lua", "return {}\n");

			// Rojo says only one may be present. This one breaks that rule on
			// purpose, so the engine's answer to it is pinned rather than left
			// to whichever name sorts first.
			Write("src/Both/init.luau", "return {}\n");
			Write("src/Both/init.server.luau", "print('also')\n");

			// The one mapping this engine still reports rather than builds, and
			// a `.rbxm` beside it that it now does — the pair is what makes the
			// case below able to say the second is *not* reported.
			Write("src/Other/Legacy.rbxmx", "<roblox version=\"4\"/>\n");
			WriteBinary(Root, "src/Other/Crate.rbxm", MODEL_RBXM);
			Write("src/Other/Notes.txt", "text\n");
			Write("src/Other/Strings.csv", "key,value\n");
			Write("src/Other/Data.json", "{}\n");
			Write("src/Other/Thing.model.json", "{}\n");
			Write("src/Other/Leaf.meta.json", "{}\n");
		}

		~MappingTree() {
			std::filesystem::remove_all(Root);
		}

		void Write(const std::string &relative, const char *text) {
			const std::filesystem::path file = Root / relative;
			std::filesystem::create_directories(file.parent_path());
			std::ofstream out(file);
			out << text;
		}
	};

	constexpr const char *MAPPING_PROJECT = R"({
	  "name": "Mapping",
	  "tree": {
	    "$className": "DataModel",
	    "ReplicatedStorage": {
	      "$className": "ReplicatedStorage",
	      "Shared": { "$className": "Folder", "$path": "src" }
	    }
	  }
	})";

	// Whether any note mentions a fragment, so a case can assert that a file
	// was accounted for without pinning the whole sentence.
	bool Noted(const RojoSyncReport &report, std::string_view fragment) {
		for (const std::string &note : report.Notes) {
			if (note.find(fragment) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("each init form decides what its directory becomes", "[studio][rojosync]") {
	MappingTree tree;
	Store store("rojo_mapping");
	engine::scene::EnsureClassTree();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(MAPPING_PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	// `$path` maps a directory's *contents* into the node, so these sit
	// directly under `Shared` rather than under an extra `src` level.
	const Entity src = Child(store, store.FindFirstRoot("ReplicatedStorage"), "Shared");
	REQUIRE(src != NULL_ENTITY);

	// The directory *is* the script, and it keeps its children — which is the
	// whole reason Rojo has the convention.
	const Entity module = Child(store, src, "Module");
	REQUIRE(module != NULL_ENTITY);
	CHECK(store.ClassOf(module) == engine::ecs::Classes::Find(Name("ModuleScript")));
	CHECK(Child(store, module, "Leaf") != NULL_ENTITY);

	// **The suffix decides the class.** These two were the bug: without the
	// init family they were plain `Folder`s holding a script called `init`.
	const Entity server = Child(store, src, "Server");
	REQUIRE(server != NULL_ENTITY);
	CHECK(store.ClassOf(server) == engine::ecs::Classes::Find(Name("Script")));
	CHECK(Child(store, server, "init") == NULL_ENTITY);

	const Entity client = Child(store, src, "Client");
	REQUIRE(client != NULL_ENTITY);
	CHECK(store.ClassOf(client) == engine::ecs::Classes::Find(Name("LocalScript")));

	// `.lua` is accepted wherever `.luau` is.
	const Entity legacy = Child(store, src, "Legacy");
	REQUIRE(legacy != NULL_ENTITY);
	CHECK(store.ClassOf(legacy) == engine::ecs::Classes::Find(Name("ModuleScript")));
}

TEST_CASE("a directory with two init files picks one and says so", "[studio][rojosync]") {
	MappingTree tree;
	Store store("rojo_mapping");
	engine::scene::EnsureClassTree();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(MAPPING_PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	const Entity src = Child(store, store.FindFirstRoot("ReplicatedStorage"), "Shared");
	const Entity both = Child(store, src, "Both");
	REQUIRE(both != NULL_ENTITY);

	// The order is fixed and written down — module, then server, then client —
	// so the class does not depend on which name sorts first.
	CHECK(store.ClassOf(both) == engine::ecs::Classes::Find(Name("ModuleScript")));

	// And the one it ignored is neither silently dropped nor a child called
	// `init`.
	CHECK(Child(store, both, "init") == NULL_ENTITY);
	CHECK(Noted(report, "more than one init file"));
}

TEST_CASE("a mapping this engine cannot build is named by what it is", "[studio][rojosync]") {
	MappingTree tree;
	Store store("rojo_mapping");
	engine::scene::EnsureClassTree();

	RojoProject project;
	std::string error;
	REQUIRE(ParseRojoProject(MAPPING_PROJECT, project, error));

	RojoSyncReport report;
	REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

	// "not a script" is the right thing to say about a stray `.DS_Store` and the
	// wrong thing to say about an `.rbxmx`. One is noise in the project; the
	// other is a gap here, and an author should be able to tell which they have.
	CHECK(Noted(report, "Legacy.rbxmx is an XML Roblox model"));

	// **And the ones that are built are not reported at all**, which is the
	// half of this case that would go stale silently: a mapping that stopped
	// working would come back as a note, and a note nobody asserts the absence
	// of is a regression nobody sees. `.rbxm` moved into this list at v0.15 and
	// the assertion moved with it rather than being added beside the old one.
	CHECK_FALSE(Noted(report, "Crate.rbxm is"));
	CHECK_FALSE(Noted(report, "Notes.txt is"));
	CHECK_FALSE(Noted(report, "Strings.csv is"));
	CHECK_FALSE(Noted(report, "Thing.model.json is"));
	CHECK_FALSE(Noted(report, "Leaf.meta.json is"));
	CHECK_FALSE(Noted(report, "Data.json is"));
}

// --- the rest of the table ----------------------------------------------------
//
// Six mappings landed at v0.12 and each fails in a way a simpler check would
// miss: a model whose properties were parsed but never written, a JSON module
// that emits source Luau will not compile, a sidecar applied to the wrong
// sibling, a nested project that recurses for ever.

namespace {
	struct TableTree {
		std::filesystem::path Root;

		TableTree() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-rojo-table-" +
					std::to_string(
						std::filesystem::hash_value(std::filesystem::temp_directory_path() / "rojo-table")
					));
			std::filesystem::remove_all(Root);

			// A model with properties and a child, which is the whole of
			// `.model.json`.
			Write("src/Crate.model.json", R"({
			  "className": "Part",
			  "name": "Crate",
			  "properties": { "Anchored": true, "Transparency": 0.5,
			                  "Size": [4, 1, 2], "Color": {"R": 1, "G": 0, "B": 0} },
			  "children": [ { "className": "Part", "name": "Lid" } ]
			})");

			// A patch on the instance its sibling produced.
			Write("src/Widget.luau", "return {}\n");
			Write("src/Widget.meta.json", R"({ "properties": { "Disabled": true } })");

			// A document that has to survive being turned into Luau and back:
			// a key that is not an identifier, a string with a quote in it, and
			// a number `%f` would round to nothing.
			Write("src/Data.json", R"({
			  "plain": 1,
			  "not-an-identifier": true,
			  "quoted": "a \"quoted\" word",
			  "tiny": 1e-8,
			  "nested": { "list": [1, 2, 3] }
			})");

			Write("src/Notes.txt", "some text\n");
			Write("src/Strings.csv", "key,en\nhello,Hello\n");

			// A nested project, and one that includes itself.
			Write("src/Package/default.project.json", R"({
			  "name": "Package",
			  "tree": { "$className": "Folder",
			            "Inner": { "$className": "Folder", "$path": "lib" } }
			})");
			Write("src/Package/lib/Helper.luau", "return {}\n");

			Write("src/Loop/default.project.json", R"({
			  "name": "Loop",
			  "tree": { "$className": "Folder",
			            "Again": { "$className": "Folder", "$path": "." } }
			})");

			// **A package as a package manager writes one**: a project whose
			// whole tree is a `$path`, beside the source and tests it was
			// published with. This is the shape every wally dependency has, and
			// the two ways of getting it wrong are both silent — building
			// nothing because the root has no children, or building the folder
			// *as well* and ending up with two copies of every module.
			Write("src/Wally/default.project.json", R"({
			  "name": "Wally",
			  "tree": { "$path": "lib" }
			})");
			Write("src/Wally/lib/Module.luau", "return {}\n");
			Write("src/Wally/lib/Inner/Deep.luau", "return {}\n");
			Write("src/Wally/test/Spec.luau", "return {}\n");

			// **The same awkward cases `Data.json` carries, in TOML.** Rojo maps
			// the two to the same `ModuleScript`, so the emitter is shared and
			// the interesting question is whether the *parse* reaches it intact
			// — plus a date, which is the one TOML type JSON has no answer for.
			Write("src/Config.toml", R"(plain = 1
"not-an-identifier" = true
quoted = 'a "quoted" word'
tiny = 1e-8
stamped = 1979-05-27

[nested]
list = [1, 2, 3]
)");

			// The one still reported rather than built.
			Write("src/Legacy.rbxmx", "<roblox version=\"4\"/>\n");

			// And three binary models: one this engine builds, one cut off part
			// way through, and one holding two instances at its top level. The
			// last two are here rather than in their own tree because what they
			// have to prove is that **one bad model costs its own file** — a
			// refusal that stopped the sync would be invisible in a tree that
			// held nothing else.
			WriteBinary(Root, "src/Chest.rbxm", MODEL_RBXM);
			WriteBinary(Root, "src/Broken.rbxm", std::span(MODEL_RBXM).first(120));
			WriteBinary(Root, "src/Pair.rbxm", TWO_ROOT_RBXM);
		}

		~TableTree() {
			std::filesystem::remove_all(Root);
		}

		void Write(const std::string &relative, const char *text) {
			const std::filesystem::path file = Root / relative;
			std::filesystem::create_directories(file.parent_path());
			std::ofstream out(file);
			out << text;
		}
	};

	constexpr const char *TABLE_PROJECT = R"({
	  "name": "Table",
	  "tree": {
	    "$className": "DataModel",
	    "ReplicatedStorage": {
	      "$className": "ReplicatedStorage",
	      "Shared": { "$className": "Folder", "$path": "src" }
	    }
	  }
	})";

	// The synced `src`, which `$path` maps into `Shared` directly.
	Entity SyncTable(Store &store, const TableTree &tree, RojoSyncReport &report) {
		RojoProject project;
		std::string error;
		REQUIRE(ParseRojoProject(TABLE_PROJECT, project, error));
		REQUIRE(SyncRojoProject(project, tree.Root, store, report, error));

		const Entity shared = Child(store, store.FindFirstRoot("ReplicatedStorage"), "Shared");
		REQUIRE(shared != NULL_ENTITY);
		return shared;
	}

	bool Mentioned(const RojoSyncReport &report, std::string_view fragment) {
		for (const std::string &note : report.Notes) {
			if (note.find(fragment) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("a model file builds its class, its properties and its children", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity crate = Child(store, shared, "Crate");
	REQUIRE(crate != NULL_ENTITY);
	CHECK(store.ClassOf(crate) == engine::ecs::Classes::Find(Name("Part")));

	// **The properties are read back off the store**, not off the document — a
	// version that parsed them and never wrote them passes any check that only
	// looks at the tree.
	bool anchored = false;
	REQUIRE(store.GetProperty(crate, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK(anchored);

	float transparency = 0.0f;
	REQUIRE(store.GetProperty(crate, Name("Transparency"), &transparency, sizeof(transparency)));
	CHECK(transparency == 0.5f);

	// Both spellings Rojo has used: an array for `Size`, named parts for `Color`.
	engine::core::Vector3 size;
	REQUIRE(store.GetProperty(crate, Name("Size"), &size, sizeof(size)));
	CHECK(size.X == 4.0f);
	CHECK(size.Z == 2.0f);

	engine::core::Color3 colour;
	REQUIRE(store.GetProperty(crate, Name("Color"), &colour, sizeof(colour)));
	CHECK(colour.R == 1.0f);
	CHECK(colour.G == 0.0f);

	CHECK(Child(store, crate, "Lid") != NULL_ENTITY);
}

TEST_CASE("a meta.json patches the instance its sibling built", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity widget = Child(store, shared, "Widget");
	REQUIRE(widget != NULL_ENTITY);

	bool disabled = false;
	REQUIRE(store.GetProperty(widget, Name("Disabled"), &disabled, sizeof(disabled)));
	CHECK(disabled);

	// **And the sidecar is not an instance of its own.** A `.meta.json` that
	// became a node would put a `Widget` folder beside the script it was
	// describing.
	CHECK(Child(store, shared, "Widget.meta") == NULL_ENTITY);
	CHECK(Child(store, shared, "Widget.meta.json") == NULL_ENTITY);
}

TEST_CASE("a json file becomes a module whose source compiles", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity data = Child(store, shared, "Data");
	REQUIRE(data != NULL_ENTITY);
	CHECK(store.ClassOf(data) == engine::ecs::Classes::Find(Name("ModuleScript")));

	const auto *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);

	const std::string *staged = cache->Find(Name("src/Data.json"));
	REQUIRE(staged != nullptr);
	const std::string_view source = *staged;

	// **A key that is not an identifier has to be bracketed**, or the chunk is a
	// syntax error — which is the failure a round trip through a table would not
	// have caught, because the table is never built.
	CHECK(source.find("[\"not-an-identifier\"]") != std::string_view::npos);

	// **And the small number has to survive.** `std::to_string` is `%f`, so 1e-8
	// would have been written as "0.000000" and read back as zero.
	CHECK(source.find("1e-08") != std::string_view::npos);

	// The run is what proves it: a chunk that does not compile fails here.
	const auto runtime = MakeRuntime(store, engine::script::Language::Luau);
	INFO(source);
	REQUIRE(runtime->Run(
		// **Through the service, because a bare chunk has no `script`.** That
		// global is written onto the thread by `RunInstance`, and this chunk is
		// not a script instance — the module it wants is still reachable the way
		// any script would reach one in another container.
		"local shared = game:GetService('ReplicatedStorage').Shared\n"
		"local data = require(shared.Data)\n"
		"assert(data.plain == 1, 'plain')\n"
		"assert(data['not-an-identifier'] == true, 'bracketed key')\n"
		"assert(data.quoted == 'a \"quoted\" word', 'escaped quote')\n"
		"assert(data.tiny > 0, 'the small number was rounded away')\n"
		"assert(data.nested.list[2] == 2, 'nested')\n",
		"table-test"
	));
}

TEST_CASE("a text file becomes the instance that holds it", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity notes = Child(store, shared, "Notes");
	REQUIRE(notes != NULL_ENTITY);
	CHECK(store.ClassOf(notes) == engine::ecs::Classes::Find(Name("StringValue")));

	std::string held;
	REQUIRE(store.GetProperty(notes, Name("Value"), &held, sizeof(held)));
	CHECK(held == "some text\n");

	const Entity strings = Child(store, shared, "Strings");
	REQUIRE(strings != NULL_ENTITY);
	CHECK(store.ClassOf(strings) == engine::ecs::Classes::Find(Name("LocalizationTable")));

	REQUIRE(store.GetProperty(strings, Name("Value"), &held, sizeof(held)));
	CHECK(held.find("hello,Hello") != std::string::npos);

	// Both are a `ValueBase`, which is the question a script would ask.
	CHECK(store.IsA(notes, engine::ecs::Classes::Find(Name("ValueBase"))));
	CHECK(store.IsA(strings, engine::ecs::Classes::Find(Name("ValueBase"))));
}

TEST_CASE("a nested project is followed, and a cycle in one is not", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	// The nested project's own tree, built under the folder that held it.
	const Entity package = Child(store, shared, "Package");
	REQUIRE(package != NULL_ENTITY);

	const Entity inner = Child(store, package, "Inner");
	REQUIRE(inner != NULL_ENTITY);
	CHECK(Child(store, inner, "Helper") != NULL_ENTITY);

	// **A project that includes itself terminates and says so**, which is the
	// case that recurses until the stack runs out without the check — a crash
	// with no line number, from a file somebody copy-pasted a `$path` into.
	CHECK(Mentioned(report, "includes itself"));
}

TEST_CASE("a package's project replaces its folder rather than joining it", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity wally = Child(store, shared, "Wally");
	REQUIRE(wally != NULL_ENTITY);

	// **The root `$path` is built into the including node.** A package's project
	// file is a root with a path and no children, so a sync that only walked the
	// children built nothing at all — and the modules a game requires by name
	// were simply absent, which reads as a broken package rather than as a
	// missing rule.
	REQUIRE(Child(store, wally, "Module") != NULL_ENTITY);
	REQUIRE(Child(store, Child(store, wally, "Inner"), "Deep") != NULL_ENTITY);

	// **And the folder beside it is not walked.** `lib/` reached the tree under
	// the package's own name, so a `lib` folder here would be the second copy —
	// two `ModuleScript`s of one file, which are two modules with two states.
	CHECK(Child(store, wally, "lib") == NULL_ENTITY);

	// The package's tests are outside its `$path` and are not a game's problem.
	// Building them would put a test framework's requires into a shipped place.
	CHECK(Child(store, wally, "test") == NULL_ENTITY);
	CHECK(Child(store, wally, "Spec") == NULL_ENTITY);
}

TEST_CASE("the unbuilt mappings are named by what they are", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	(void)SyncTable(store, tree, report);

	// It names the dependency that is missing rather than saying "not a
	// script", so a gap reads as a gap.
	CHECK(Mentioned(report, "Legacy.rbxmx is an XML Roblox model"));

	// And nothing that *is* built is reported as missing. **`.toml` is in this
	// list rather than the one above as of v0.13** — it was the third unbuilt
	// mapping and the only one whose cost was a submodule rather than a format
	// reader, so closing it moved the assertion instead of adding one.
	CHECK_FALSE(Mentioned(report, "Config.toml is"));
	CHECK_FALSE(Mentioned(report, "Crate.model.json"));
	CHECK_FALSE(Mentioned(report, "Data.json is"));
	CHECK_FALSE(Mentioned(report, "Notes.txt is"));
}

// --- Roblox's binary model ----------------------------------------------------
//
// The last row of Rojo's table, closed at v0.15. The reader is `bake`'s and its
// own suite bends the format; what these assert is the half that lives here —
// that a file becomes instances, that the ones it cannot become are named, and
// that a model this engine will not build costs its own file and nothing else.

TEST_CASE("a binary model builds its class, its properties and its children", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	// **Named after the file, not after what the file called it.** The model
	// inside is a `Model` called `Crate`; the file is `Chest.rbxm`, and every
	// other row of Rojo's table takes the name from the path. The children keep
	// their own names, which is the half a blanket rename would lose.
	const Entity chest = Child(store, shared, "Chest");
	REQUIRE(chest != NULL_ENTITY);
	CHECK(store.ClassOf(chest) == engine::ecs::Classes::Find(Name("Model")));

	const Entity lid = Child(store, chest, "Lid");
	REQUIRE(lid != NULL_ENTITY);
	CHECK(store.ClassOf(lid) == engine::ecs::Classes::Find(Name("Part")));

	// **Read back off the store, not off the file.** A version that decoded the
	// properties and never wrote them passes any check that only looks at the
	// tree — which is the same trap the `.model.json` case is written against.
	bool anchored = false;
	REQUIRE(store.GetProperty(lid, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK(anchored);

	float transparency = 0.0f;
	REQUIRE(store.GetProperty(lid, Name("Transparency"), &transparency, sizeof(transparency)));
	CHECK(transparency == Approx(0.5f));

	engine::core::Vector3 size;
	REQUIRE(store.GetProperty(lid, Name("Size"), &size, sizeof(size)));
	CHECK(size.X == Approx(4.0f));
	CHECK(size.Y == Approx(1.0f));
	CHECK(size.Z == Approx(2.0f));

	// The file writes this one as three bytes on 0..255 rather than three floats
	// on 0..1, which is a separate type number and a separate arm of the reader.
	engine::core::Color3 colour;
	REQUIRE(store.GetProperty(lid, Name("Color"), &colour, sizeof(colour)));
	CHECK(colour.R == Approx(1.0f));
	CHECK(colour.G == Approx(0.0f));
	CHECK(colour.B == Approx(0.0f));
}

TEST_CASE("a binary model's rotation survives, unlike a model.json's", "[studio][rojosync]") {
	// **The one property this path carries further than the JSON one.** A
	// `.model.json` writes a `CFrame` as twelve numbers and this module reads
	// only its position; a `.rbxm` states an orientation as one byte naming one
	// of twenty-four, and dropping it would lay every rotated part flat with
	// nothing saying so.
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);
	const Entity lid = Child(store, Child(store, shared, "Chest"), "Lid");
	REQUIRE(lid != NULL_ENTITY);

	engine::core::CFrame frame;
	REQUIRE(store.GetProperty(lid, Name("CFrame"), &frame, sizeof(frame)));
	CHECK(frame.Position.X == Approx(1.0f));
	CHECK(frame.Position.Y == Approx(2.0f));
	CHECK(frame.Position.Z == Approx(3.0f));

	// The file's rotation byte names right = +X, up = +Z. Identity would put the
	// up vector on +Y, so this is the assertion a dropped rotation fails.
	CHECK(frame.UpVector().Z == Approx(1.0f).margin(1e-5));
	CHECK(frame.UpVector().Y == Approx(0.0f).margin(1e-5));
}

TEST_CASE("a script inside a binary model arrives with its program", "[studio][rojosync]") {
	// **Roblox keeps a script's text on the instance and this engine keeps a key
	// into the world's `SourceCache`.** So an import that only made the instance
	// would produce a `Script` that exists, sits in the tree, and never runs —
	// which is the failure that looks exactly like a script with a bug in it.
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity boot = Child(store, Child(store, shared, "Chest"), "Boot");
	REQUIRE(boot != NULL_ENTITY);
	CHECK(store.ClassOf(boot) == engine::ecs::Classes::Find(Name("Script")));

	const auto *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);

	// Keyed by where the instance sits inside the file, so two scripts of one
	// model are two programs.
	const std::string *staged = cache->Find(Name("src/Chest.rbxm/Boot"));
	REQUIRE(staged != nullptr);
	CHECK(*staged == "print('hello')\n");
}

TEST_CASE("a binary model gives the same answers about what it could not build", "[studio][rojosync]") {
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);
	const Entity chest = Child(store, shared, "Chest");
	REQUIRE(chest != NULL_ENTITY);

	// **A class this engine does not have becomes a folder and says so**, which
	// is the answer `SyncRojoProject` already gives for a `$className` — one
	// answer to one question, whichever format asked it.
	const Entity chat = Child(store, chest, "Chat");
	REQUIRE(chat != NULL_ENTITY);
	CHECK(store.ClassOf(chat) == studio::FolderClass());
	CHECK(Mentioned(report, "Chat is not a class here"));

	// An enum is a number naming a member of Roblox's table and this engine
	// names members by string, so the reader refuses one and the note carries
	// both the file and the property.
	CHECK(Mentioned(report, "Chest.rbxm: Part.Material is an Enum"));

	// **A property Roblox has and this engine does not is counted, not listed.**
	// Studio stores every property of every class, so a note each would be a
	// hundred lines saying this engine is smaller than Roblox — and would bury
	// the notes that are about this file.
	CHECK(Mentioned(report, "this engine has no property for"));
	CHECK_FALSE(Mentioned(report, "RootPriority"));
}

TEST_CASE("a binary model this engine will not build costs its own file", "[studio][rojosync]") {
	// Two ways a model file fails, beside one that does not. **A refusal that
	// stopped the sync would be invisible in a tree holding nothing else**,
	// which is why all three are in one project.
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	// Cut off part way through. Named with the reader's own message, because
	// "is not a valid rbxm" sends somebody back to stare at a binary file.
	CHECK(Child(store, shared, "Broken") == NULL_ENTITY);
	CHECK(Mentioned(report, "Broken.rbxm could not be read"));

	// Two instances at the top level, which the container allows and Rojo's file
	// table does not. Refused by name rather than wrapped in a folder nobody
	// wrote.
	CHECK(Child(store, shared, "Pair") == NULL_ENTITY);
	CHECK(Mentioned(report, "Pair.rbxm holds 2 instances at its top level"));

	// And the good one is still there.
	CHECK(Child(store, shared, "Chest") != NULL_ENTITY);
}

TEST_CASE("a toml file becomes a module whose source compiles", "[studio][rojosync]") {
	// **The `.json` case's twin, deliberately**, because Rojo maps the two to
	// one thing and this engine emits them through one function. What is worth
	// asserting separately is the half that is *not* shared: the parse, and the
	// three TOML types JSON cannot express.
	TableTree tree;
	Store store("rojo_table");
	engine::scene::EnsureClassTree();

	RojoSyncReport report;
	const Entity shared = SyncTable(store, tree, report);

	const Entity config = Child(store, shared, "Config");
	REQUIRE(config != NULL_ENTITY);
	CHECK(store.ClassOf(config) == engine::ecs::Classes::Find(Name("ModuleScript")));

	const auto *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);

	const std::string *staged = cache->Find(Name("src/Config.toml"));
	REQUIRE(staged != nullptr);
	const std::string_view source = *staged;

	// The same two hazards the JSON case pins, because they are properties of
	// the emitter and the emitter is shared: a key that is not an identifier is
	// bracketed, and a number `%f` would round away survives.
	CHECK(source.find("[\"not-an-identifier\"]") != std::string_view::npos);
	CHECK(source.find("1e-08") != std::string_view::npos);

	// **A date arrives as its TOML spelling.** There is no JSON type and no Luau
	// one, and the two alternatives are worse in ways an author would have to
	// debug: dropping the key makes it silently absent, and a table of parts
	// invents an interface this engine would then owe them.
	CHECK(source.find("1979-05-27") != std::string_view::npos);

	// The run is what proves it compiles, exactly as the JSON case does.
	const auto runtime = MakeRuntime(store, engine::script::Language::Luau);
	INFO(source);
	REQUIRE(runtime->Run(
		"local shared = game:GetService('ReplicatedStorage').Shared\n"
		"local config = require(shared.Config)\n"
		"assert(config.plain == 1, 'plain')\n"
		"assert(config['not-an-identifier'] == true, 'bracketed key')\n"
		"assert(config.quoted == 'a \"quoted\" word', 'quote')\n"
		"assert(config.nested.list[2] == 2, 'nested array')\n"
		"assert(type(config.stamped) == 'string', 'date is text')\n",
		"toml_module"
	));
}

// --- the universe above them -------------------------------------------------
//
// **The property under test is that the worlds are independent.** One project
// file with a typo in it has to cost its own world and nothing else — a sync
// that stopped at the first bad file would make one mistake look like the whole
// game was broken, and the author would have no way to tell which folder was at
// fault.

namespace {
	constexpr const char *WORLD_PROJECT = R"({
	  "name": "World",
	  "tree": {
	    "$className": "DataModel",
	    "ReplicatedStorage": {
	      "$className": "ReplicatedStorage",
	      "Shared": { "$className": "Folder", "$path": "src" }
	    }
	  }
	})";

	// A universe laid out the way `RojoSync.hpp` describes: a file at the root
	// and an ordinary Rojo project inside each subfolder.
	struct UniverseTree {
		std::filesystem::path Root;

		UniverseTree() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-rojo-universe-" +
					std::to_string(
						std::filesystem::hash_value(std::filesystem::temp_directory_path() / "rojo-universe")
					));
			std::filesystem::remove_all(Root);

			Write("worlds/main/default.project.json", WORLD_PROJECT);
			Write("worlds/main/src/Boot.server.luau", "print('main')\n");

			// The second name, for a folder that is only ever a world of this
			// engine's universe rather than a project Rojo also serves.
			Write("worlds/lobby/main.default.json", WORLD_PROJECT);
			Write("worlds/lobby/src/Boot.server.luau", "print('lobby')\n");

			// The one that should fail on its own.
			Write("worlds/broken/default.project.json", "{ this is not json");
			Write("worlds/broken/src/Boot.server.luau", "print('broken')\n");
		}

		~UniverseTree() {
			std::filesystem::remove_all(Root);
		}

		void Write(const std::string &relative, const char *text) {
			const std::filesystem::path file = Root / relative;
			std::filesystem::create_directories(file.parent_path());
			std::ofstream out(file);
			out << text;
		}
	};

}

TEST_CASE("a universe file names worlds in order", "[studio][rojosync]") {
	RojoUniverse universe;
	std::string error;

	REQUIRE(ParseRojoUniverse(
		R"({ "name": "MyGame", "worlds": { "Main": "worlds/main", "Lobby": "worlds/lobby" } })",
		universe,
		error
	));
	CHECK(universe.Name == "MyGame");
	REQUIRE(universe.Worlds.size() == 2);
}

TEST_CASE("a document with no worlds is refused", "[studio][rojosync]") {
	RojoUniverse universe;
	std::string error;

	CHECK_FALSE(ParseRojoUniverse(R"({"name": "no worlds here"})", universe, error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(ParseRojoUniverse(R"({"worlds": {}})", universe, error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(ParseRojoUniverse("{ this is not json", universe, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("a world entry resolves to a project file under either name", "[studio][rojosync]") {
	UniverseTree tree;

	CHECK(studio::RojoProjectFor(tree.Root, {"Main", "worlds/main"}).filename() == "default.project.json");
	CHECK(studio::RojoProjectFor(tree.Root, {"Lobby", "worlds/lobby"}).filename() == "main.default.json");

	// A path naming the file directly is taken as it is.
	CHECK_FALSE(studio::RojoProjectFor(tree.Root, {"Main", "worlds/main/default.project.json"}).empty());

	// A folder with no project in it, and a folder that is not there.
	CHECK(studio::RojoProjectFor(tree.Root, {"Empty", "worlds"}).empty());
	CHECK(studio::RojoProjectFor(tree.Root, {"Gone", "worlds/nowhere"}).empty());
	CHECK(studio::RojoProjectFor(tree.Root, {"Unnamed", ""}).empty());
}

TEST_CASE("every world in a universe is built into its own store", "[studio][rojosync]") {
	UniverseTree tree;
	engine::scene::EnsureClassTree();

	RojoUniverse universe;
	std::string error;
	REQUIRE(ParseRojoUniverse(
		R"({ "name": "MyGame", "worlds": { "Main": "worlds/main", "Lobby": "worlds/lobby" } })",
		universe,
		error
	));

	Universe worlds;
	RojoUniverseReport report;
	REQUIRE(SyncRojoUniverse(universe, tree.Root, worlds, report, error));

	REQUIRE(report.Worlds.size() == 2);
	CHECK(report.Synced() == 2);
	CHECK(report.Failed() == 0);

	// Each world got its own tree, and only its own — the scripts are staged
	// per store, so a world reading the other's source would show up here.
	for (const auto &synced : report.Worlds) {
		INFO(synced.World << ": " << synced.Error);
		REQUIRE(synced.Synced);
		CHECK(synced.Report.Scripts == 1);

		const WorldId id = worlds.Find(Name(synced.World));
		REQUIRE(id.IsValid());

		bool found = false;
		worlds.Enter(id, [&](Store &store) {
			const Entity storage = store.FindFirstRoot("ReplicatedStorage");
			REQUIRE(storage != NULL_ENTITY);
			found = Child(store, Child(store, storage, "Shared"), "Boot") != NULL_ENTITY;
		});
		CHECK(found);
	}
}

TEST_CASE("one world's bad project file does not stop the rest", "[studio][rojosync]") {
	UniverseTree tree;
	engine::scene::EnsureClassTree();

	RojoUniverse universe;
	std::string error;
	REQUIRE(ParseRojoUniverse(
		R"({ "worlds": {
		      "Broken": "worlds/broken",
		      "Main": "worlds/main",
		      "Missing": "worlds/nowhere"
		    } })",
		universe,
		error
	));

	Universe worlds;
	RojoUniverseReport report;

	// True overall, because something built. That is the contract: a universe
	// sync fails only when *no* world could be built.
	REQUIRE(SyncRojoUniverse(universe, tree.Root, worlds, report, error));
	REQUIRE(report.Worlds.size() == 3);
	CHECK(report.Synced() == 1);
	CHECK(report.Failed() == 2);

	for (const auto &synced : report.Worlds) {
		if (synced.World == "Main") {
			CHECK(synced.Synced);
			CHECK(synced.Error.empty());
			continue;
		}

		// Each failure carries its own reason, which is what tells an author
		// which of five folders is at fault.
		CHECK_FALSE(synced.Synced);
		CHECK_FALSE(synced.Error.empty());
	}

	// The world that failed to parse was never created, and the one that built
	// is there.
	CHECK(worlds.Find(Name("Main")).IsValid());
	CHECK_FALSE(worlds.Find(Name("Missing")).IsValid());
}

TEST_CASE("a universe where nothing builds is a failure", "[studio][rojosync]") {
	UniverseTree tree;
	engine::scene::EnsureClassTree();

	RojoUniverse universe;
	std::string error;
	REQUIRE(ParseRojoUniverse(R"({ "worlds": { "Broken": "worlds/broken" } })", universe, error));

	Universe worlds;
	RojoUniverseReport report;

	CHECK_FALSE(SyncRojoUniverse(universe, tree.Root, worlds, report, error));
	CHECK_FALSE(error.empty());
	CHECK(report.Failed() == 1);
}

TEST_CASE("syncing a universe twice builds into the worlds it made", "[studio][rojosync]") {
	UniverseTree tree;
	engine::scene::EnsureClassTree();

	RojoUniverse universe;
	std::string error;
	REQUIRE(ParseRojoUniverse(R"({ "worlds": { "Main": "worlds/main" } })", universe, error));

	Universe worlds;

	RojoUniverseReport first;
	REQUIRE(SyncRojoUniverse(universe, tree.Root, worlds, first, error));

	RojoUniverseReport second;
	REQUIRE(SyncRojoUniverse(universe, tree.Root, worlds, second, error));

	// One world, not two: `Universe::Create` returns the world already holding
	// a name, and a second world called `Main` would be a game whose halves
	// cannot find each other.
	CHECK(worlds.Worlds().size() == 1);

	// And it reused the existing `ReplicatedStorage` rather than making a
	// second beside it, which is the same rule `BuildNode` follows.
	worlds.Enter(worlds.Find(Name("Main")), [&](Store &store) {
		size_t roots = 0;
		store.EachRoot([&roots](Entity) { roots++; });

		size_t storages = 0;
		store.EachRoot([&](Entity root) {
			if (store.InstanceNameOf(root) == Name("ReplicatedStorage")) {
				storages++;
			}
		});
		CHECK(storages == 1);
		CHECK(roots > 0);
	});
}
