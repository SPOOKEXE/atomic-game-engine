// The path walk behind `require("./x")`, tested over a bare `Store`. What a
// script sees on top of it is covered in `scripthost/tests/RequirePath.cpp`.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/RequirePath.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.requirepath")
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::RequirePathFailure;
using engine::script::WalkRequirePath;

namespace {
	Entity Make(Store &store, engine::ecs::ClassId type, std::string_view name, Entity parent) {
		const Entity instance = store.CreateInstance(type, name);
		REQUIRE(instance != NULL_ENTITY);
		if (parent != NULL_ENTITY) REQUIRE(store.SetParent(instance, parent));
		return instance;
	}

	// Two roots standing in for services:
	//   Workspace (Part)
	//     Game (Part)
	//       Main (Script)
	//       Lib (ModuleScript)
	//         Inner (ModuleScript)
	//       Thing (Part)
	//     Shared (ModuleScript)
	//   ReplicatedStorage (Part)
	//     Config (ModuleScript)
	struct Tree {
		Store World{"require-path"};
		Entity Workspace, Game, Main, Lib, Inner, Thing, Shared, Replicated, Config;

		Tree() {
			engine::scene::EnsureClassTree();
			const auto part = engine::scene::PartClass();
			const auto module = engine::script::ModuleScriptClass();
			Workspace = Make(World, part, "Workspace", NULL_ENTITY);
			Game = Make(World, part, "Game", Workspace);
			Main = Make(World, engine::script::ScriptClass(), "Main", Game);
			Lib = Make(World, module, "Lib", Game);
			Inner = Make(World, module, "Inner", Lib);
			Thing = Make(World, part, "Thing", Game);
			Shared = Make(World, module, "Shared", Workspace);
			Replicated = Make(World, part, "ReplicatedStorage", NULL_ENTITY);
			Config = Make(World, module, "Config", Replicated);
		}

		Entity Walk(Entity from, std::string_view path, Entity root = NULL_ENTITY) {
			const auto found = WalkRequirePath(World, from, path, root);
			return found.Failure == RequirePathFailure::None ? found.Found : NULL_ENTITY;
		}
	};
}

TEST_CASE("a require path walks the tree from the requiring script", "[script][require]") {
	Tree tree;
	CHECK(tree.Walk(tree.Main, "./Lib") == tree.Lib);
	CHECK(tree.Walk(tree.Main, "./Lib.luau") == tree.Lib);
	CHECK(tree.Walk(tree.Main, "./Lib/Inner") == tree.Inner);
	CHECK(tree.Walk(tree.Lib, "@self/Inner") == tree.Inner);
	CHECK(tree.Walk(tree.Inner, "../Lib") == tree.Lib);
	CHECK(tree.Walk(tree.Main, "../Shared") == tree.Shared);
	CHECK(tree.Walk(tree.Main, "./../Shared") == tree.Shared);
	// Above a service is the game, where a name is another service.
	CHECK(tree.Walk(tree.Main, "../../ReplicatedStorage/Config") == tree.Config);
}

TEST_CASE("a require path says why it named nothing", "[script][require]") {
	Tree tree;
	CHECK(WalkRequirePath(tree.World, tree.Main, "Lib", NULL_ENTITY).Failure == RequirePathFailure::Prefix);
	CHECK(WalkRequirePath(tree.World, tree.Main, "/Lib", NULL_ENTITY).Failure == RequirePathFailure::Prefix);

	const auto missing = WalkRequirePath(tree.World, tree.Main, "./Lib/Nope", NULL_ENTITY);
	CHECK(missing.Failure == RequirePathFailure::Missing);
	CHECK(missing.Part == "Nope");

	const auto thing = WalkRequirePath(tree.World, tree.Main, "./Thing", NULL_ENTITY);
	CHECK(thing.Failure == RequirePathFailure::NotModule);
	CHECK(thing.Found == tree.Thing);

	// Past the game, and the game itself, are both outside.
	CHECK(
		WalkRequirePath(tree.World, tree.Main, "../../../x", NULL_ENTITY).Failure ==
		RequirePathFailure::Leaves
	);
	CHECK(WalkRequirePath(tree.World, tree.Main, "../..", NULL_ENTITY).Failure == RequirePathFailure::Leaves);
}

TEST_CASE("a scoped require path cannot leave its root", "[script][require]") {
	Tree tree;
	CHECK(tree.Walk(tree.Main, "./Lib/Inner", tree.Game) == tree.Inner);
	CHECK(
		WalkRequirePath(tree.World, tree.Main, "../Shared", tree.Game).Failure == RequirePathFailure::Leaves
	);
	CHECK(WalkRequirePath(tree.World, tree.Main, "./..", tree.Game).Failure == RequirePathFailure::Leaves);
	// A requirer outside the root is refused before anything is walked.
	CHECK(
		WalkRequirePath(tree.World, tree.Shared, "./Game/Lib", tree.Game).Failure ==
		RequirePathFailure::Leaves
	);
}

TEST_CASE("a game require refuses plugin trees and names its script", "[script][require]") {
	Tree tree;
	const Entity plugins = Make(tree.World, engine::scene::PartClass(), "PluginGuiService", NULL_ENTITY);
	Make(tree.World, engine::script::ModuleScriptClass(), "Hidden", plugins);

	Entity module;
	std::string error;
	CHECK(engine::script::ResolveGameRequire(tree.World, tree.Main, "./Lib", module, error));
	CHECK(module == tree.Lib);

	CHECK_FALSE(
		engine::script::ResolveGameRequire(
			tree.World, tree.Main, "../../PluginGuiService/Hidden", module, error
		)
	);
	CHECK(error.find("leaves the game") != std::string::npos);

	CHECK_FALSE(engine::script::ResolveGameRequire(tree.World, tree.Main, "./Missing", module, error));
	CHECK(error == "require('./Missing') from Workspace.Game.Main: no 'Missing'");

	CHECK_FALSE(engine::script::ResolveGameRequire(tree.World, NULL_ENTITY, "./Lib", module, error));
	CHECK(error.find("no requiring script") != std::string::npos);
}
