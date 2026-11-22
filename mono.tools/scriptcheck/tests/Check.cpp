#include "Fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <scriptcheck/Check.hpp>

TEST_SUITE_ID("tools.scriptcheck")
TEST_TIER("fast")

namespace {

	bool Has(const std::vector<scriptcheck::Diagnostic> &found, const std::string &code, int line) {
		for (const scriptcheck::Diagnostic &diagnostic : found) {
			if (diagnostic.Code == code && diagnostic.Line == line) {
				return true;
			}
		}
		return false;
	}

	void Load(scriptcheck::Checker &checker) {
		std::vector<scriptcheck::Diagnostic> failures;
		REQUIRE(checker.Load(fixture::DECLARATIONS, failures));
		REQUIRE(failures.empty());
	}
}

TEST_CASE("a plugin folder is its plugin, and only its plugin") {
	const fixture::Plugin plugin;
	const scriptcheck::Target target = scriptcheck::Discover(plugin.Folder);

	REQUIRE(target.Roots.size() == 1);
	CHECK(target.Roots[0].filename() == "plugin");
	CHECK(target.Files.size() == 4);
	for (const std::filesystem::path &file : target.Files) {
		CHECK(file.generic_string().find("/tests/") == std::string::npos);
		CHECK(file.filename() != "outside.luau");
	}
}

TEST_CASE("a type error crosses a string require") {
	const fixture::Plugin plugin;
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	const auto found = checker.Check(plugin.At("plugin/src/main.luau"));
	REQUIRE(found.size() == 1);
	CHECK(found[0].Code == "TypeError");
	CHECK(found[0].Line == 3);
	CHECK(found[0].File == std::filesystem::absolute(plugin.At("plugin/src/main.luau")).lexically_normal());
	CHECK(checker.Check(plugin.At("plugin/src/core/Add.luau")).empty());
}

TEST_CASE("a type error crosses a script.Parent require") {
	const fixture::Plugin plugin;
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	CHECK(Has(checker.Check(plugin.At("plugin/src/Chain.luau")), "TypeError", 3));
}

TEST_CASE("an error in a required module is reported under that module") {
	const fixture::Plugin plugin;
	plugin.Write(
		"plugin/src/core/deep/Broken.luau", "--!strict\nlocal x: number = 1\nlocal y: string = x\nreturn y\n"
	);
	plugin.Write(
		"plugin/src/Uses.luau", "--!strict\nlocal Broken = require(\"./core/deep/Broken\")\nprint(Broken)\n"
	);
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	const auto found = checker.Check(plugin.At("plugin/src/Uses.luau"));
	REQUIRE(found.size() == 1);
	CHECK(found[0].Line == 3);
	CHECK(
		found[0].File ==
		std::filesystem::absolute(plugin.At("plugin/src/core/deep/Broken.luau")).lexically_normal()
	);
}

TEST_CASE("script is typed from the folder tree") {
	const fixture::Plugin plugin;
	plugin.Write(
		"plugin/src/Tree.luau",
		"--!strict\nlocal core = script.Parent.core\nlocal add: ModuleScript = core.Add\n"
		"local name: string = script.Parent.Parent.Name\nprint(core, add, name, script.Parent.Nope)\n"
		"print(script.Parent.Parent.Parent.Anything)\n"
	);
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	// Line 5 names a sibling that does not exist. Line 6 climbs above the
	// plugin, where the tree is unknown and anything goes.
	const auto found = checker.Check(plugin.At("plugin/src/Tree.luau"));
	REQUIRE(found.size() == 1);
	CHECK(found[0].Line == 5);
	CHECK(found[0].Message.find("Nope") != std::string::npos);

	// A file added later is seen by the next check.
	plugin.Write("plugin/src/Nope.luau", "return 1\n");
	CHECK(checker.Check(plugin.At("plugin/src/Tree.luau")).empty());
}

TEST_CASE("a require leaving the plugin is refused") {
	const fixture::Plugin plugin;
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	const auto found = checker.Check(plugin.At("plugin/src/Escape.luau"));
	REQUIRE(Has(found, "RequireEscape", 2));
	CHECK(found[0].Message.find("../../outside") != std::string::npos);
}

TEST_CASE("game scripts in a project folder resolve string requires like plugins") {
	const fixture::Plugin files;
	files.Write("game/src/server/Lib.luau", "--!strict\nreturn { name = \"lib\" }\n");
	files.Write(
		"game/src/server/Main.server.luau",
		"--!strict\nlocal Lib = require(\"./Lib\")\nlocal count: number = Lib.name\nprint(count)\n"
	);
	files.Write(
		"game/src/client/Hud.client.luau",
		"--!strict\nlocal Lib = require(\"../server/Lib\")\nprint(Lib.name)\n"
		"local Gone = require(\"../../../outside\")\nprint(Gone)\n"
	);
	const scriptcheck::Target target = scriptcheck::Discover(files.At("game"));
	REQUIRE(target.Roots.size() == 1);

	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(target.Roots);

	const auto server = checker.Check(files.At("game/src/server/Main.server.luau"));
	REQUIRE(server.size() == 1);
	CHECK(server[0].Code == "TypeError");
	CHECK(server[0].Line == 3);

	const auto client = checker.Check(files.At("game/src/client/Hud.client.luau"));
	REQUIRE(client.size() == 1);
	CHECK(Has(client, "RequireEscape", 4));
}

TEST_CASE("a missing require is an error unless a draft asked otherwise") {
	const fixture::Plugin plugin;
	plugin.Write("plugin/src/Missing.luau", "--!strict\nlocal Gone = require(\"./Gone\")\nprint(Gone)\n");
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	CHECK(Has(checker.Check(plugin.At("plugin/src/Missing.luau")), "UnknownRequire", 2));

	checker.SetReportMissingRequires(false);
	checker.Forget(plugin.At("plugin/src/Missing.luau"));
	CHECK(checker.Check(plugin.At("plugin/src/Missing.luau")).empty());
}

TEST_CASE("an unsaved buffer replaces the file until forgotten") {
	const fixture::Plugin plugin;
	scriptcheck::Checker checker;
	Load(checker);
	checker.SetRoots(scriptcheck::Discover(plugin.Folder).Roots);

	const std::filesystem::path main = plugin.At("plugin/src/main.luau");
	checker.SetText(main, "--!strict\nlocal Add = require(\"./core/Add\")\nprint(Add.add(1, 2))\n");
	CHECK(checker.Check(main).empty());

	checker.Forget(main);
	CHECK(checker.Check(main).size() == 1);
}

TEST_CASE("declarations that do not parse are reported, not checked against") {
	scriptcheck::Checker checker;
	std::vector<scriptcheck::Diagnostic> failures;
	CHECK_FALSE(checker.Load("declare extern type with", failures));
	CHECK_FALSE(failures.empty());
}

TEST_CASE("diagnostics format as compiler lines and as JSON") {
	scriptcheck::Diagnostic diagnostic;
	diagnostic.File = "/x/a.luau";
	diagnostic.Line = 3;
	diagnostic.Column = 7;
	diagnostic.Code = "TypeError";
	diagnostic.Message = "bad";

	CHECK(scriptcheck::FormatText(diagnostic).ends_with("a.luau(3,7): TypeError: bad"));
	const std::string json = scriptcheck::FormatJson({diagnostic});
	CHECK(json.find("\"line\": 3") != std::string::npos);
	CHECK(json.find("\"code\": \"TypeError\"") != std::string::npos);
}
