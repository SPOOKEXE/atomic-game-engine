// `require("./x")` from game scripts, in both languages and both roles. The
// walk itself is covered in `script/tests/RequirePath.cpp`; this is what a
// Script, LocalScript and ModuleScript see on top of it.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

TEST_SUITE_ID("engine.scripthost.requirepath")
TEST_DEPENDS("engine.script.requirepath")
TEST_DEPENDS("engine.scene.services")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::Language;

namespace {
	void Stage(Store &store, const std::string &path, std::string_view source) {
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(engine::core::Name(path), source);
	}

	Entity
	Module(Store &store, Language language, std::string_view name, std::string_view source, Entity parent) {
		const std::string path = std::string(name) + (language == Language::Luau ? ".luau" : ".js");
		Stage(store, path, source);
		const Entity module = engine::script::MakeModule(store, path, name);
		REQUIRE(module != NULL_ENTITY);
		REQUIRE(store.SetParent(module, parent));
		return module;
	}

	Entity Script(
		Store &store,
		Language language,
		std::string_view name,
		std::string_view source,
		Entity parent,
		bool local
	) {
		const std::string path = std::string(name) + (language == Language::Luau ? ".luau" : ".js");
		Stage(store, path, source);
		const Entity script = engine::script::MakeScript(store, path, name, local);
		REQUIRE(script != NULL_ENTITY);
		REQUIRE(store.SetParent(script, parent));
		return script;
	}

	Entity Folder(Store &store, std::string_view name, Entity parent) {
		const Entity folder = store.CreateInstance(engine::scene::PartClass(), name);
		REQUIRE(folder != NULL_ENTITY);
		REQUIRE(store.SetParent(folder, parent));
		return folder;
	}
}

TEST_CASE("game scripts require by path relative to themselves", "[scripting][require]") {
	const Language language = GENERATE(Language::Luau, Language::JavaScript);
	const bool luau = language == Language::Luau;
	INFO((luau ? "luau" : "javascript"));
	engine::scene::EnsureClassTree();
	engine::script::RegisterScriptComponents();
	Store store("require-path");
	engine::scene::InstallServices(store);
	const Entity workspace = engine::scene::WorkspaceOf(store);
	const Entity game = Folder(store, "Game", workspace);

	Module(
		store,
		language,
		"Lib",
		luau ? "local inner = require('@self/Inner')\n"
			   "return {answer = inner.value + require('../Shared').base}\n"
			 : "const inner = require('@self/Inner');\n"
			   "return {answer: inner.value + require('../Shared').base};\n",
		game
	);
	Module(
		store,
		language,
		"Inner",
		luau ? "return {value = 40}\n" : "return {value: 40};\n",
		store.FindFirstChild(game, "Lib")
	);
	Module(store, language, "Shared", luau ? "return {base = 2}\n" : "return {base: 2};\n", workspace);
	Folder(store, "Thing", game);

	const char *server =
		luau ? "local lib = require('./Lib')\n"
			   "assert(lib.answer == 42)\n"
			   "assert(require(script.Parent.Lib) == lib, 'a path and an instance are one module')\n"
			   "assert(require('./Lib.luau') == lib)\n"
			   "local ok, e = pcall(require, '../../../Nowhere')\n"
			   "assert(not ok and string.find(e, 'leaves the game'), e)\n"
			   "ok, e = pcall(require, './Missing')\n"
			   "assert(not ok and string.find(e, \"no 'Missing'\"), e)\n"
			   "ok, e = pcall(require, './Thing')\n"
			   "assert(not ok and string.find(e, 'not a ModuleScript'), e)\n"
			   "ok, e = pcall(require, 'Lib')\n"
			   "assert(not ok and string.find(e, 'must start with'), e)\n"
			   "workspace:SetAttribute('Server', true)\n"
			 : "const lib = require('./Lib');\n"
			   "if (lib.answer !== 42) throw new Error('answer');\n"
			   "if (require(script.Parent.FindFirstChild('Lib')) !== lib) throw new Error('two modules');\n"
			   "let refused = '';\n"
			   "try { require('../../../Nowhere'); } catch (e) { refused = String(e); }\n"
			   "if (!refused.includes('leaves the game')) throw new Error(refused);\n"
			   "workspace.SetAttribute('Server', true);\n";
	const char *client = luau ? "assert(require('./Lib').answer == 42)\n"
							  : "if (require('./Lib').answer !== 42) throw new Error('client');\n";
	Script(store, language, "Main", server, game, false);
	Script(store, language, "Local", client, game, true);

	const auto runtime =
		engine::script::MakeRuntime(store, language, {.Role = engine::script::HostRole::OfBoth()});
	REQUIRE(runtime != nullptr);
	// Both the Script and the LocalScript ran without error.
	CHECK(runtime->RunWorldScripts() == 2);
	INFO(runtime->LastError());
	CHECK(runtime->LastError().empty());
	CHECK(runtime->Run(
		luau ? "assert(workspace:GetAttribute('Server'))\n"
			 : "if (!workspace.GetAttribute('Server')) throw new Error('ran');\n"
	));
}
