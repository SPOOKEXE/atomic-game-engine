// Reading what a program said about itself.
//
// **The input is another process's stdout, which is the one input this program
// has no control over.** A half-built tree, a binary older than `--describe`, a
// log line printed before the object - each of them has to leave a message on
// the screen rather than take the launcher down, and none of them is visible in
// a screenshot of a launcher that started successfully.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <launcher/Description.hpp>

TEST_SUITE_ID("launcher.description")

using launcher::ParseDescription;

namespace {
	const std::string GOOD = R"({
		"program": "client",
		"summary": "atomic - runs a game.",
		"version": "0.18.0",
		"options": [
			{"name": "stats", "takesValue": false, "valueName": "", "description": "Open the panels"},
			{"name": "game", "takesValue": true, "valueName": "PATH", "description": "Game file to play"}
		],
		"settings": [
			{"name": "content.gif", "kind": "boolean", "default": "false", "description": "Decode GIFs"}
		]
	})";
}

TEST_CASE("a described program yields its two surfaces", "[launcher]") {
	std::string failure;
	const auto description = ParseDescription(GOOD, failure);

	REQUIRE(description.has_value());
	CHECK(failure.empty());
	CHECK(description->Program == "client");
	CHECK(description->Version == "0.18.0");
	REQUIRE(description->Options.size() == 2);
	REQUIRE(description->Settings.size() == 1);

	// The field the whole form hangs off: a checkbox or a text box, and there
	// is nothing else in the object that answers it.
	CHECK_FALSE(description->Options[0].TakesValue);
	CHECK(description->Options[1].TakesValue);
	CHECK(description->Options[1].ValueName == "PATH");

	CHECK(description->Settings[0].Kind == "boolean");
	CHECK(description->Settings[0].Default == "false");
}

TEST_CASE("options can be looked up by name", "[launcher]") {
	std::string failure;
	const auto description = ParseDescription(GOOD, failure);
	REQUIRE(description.has_value());

	REQUIRE(description->Option("game") != nullptr);
	CHECK(description->Option("game")->ValueName == "PATH");
	CHECK(description->Option("no-such-option") == nullptr);
}

TEST_CASE("output that is not an object is refused with a reason", "[launcher]") {
	std::string failure;

	CHECK_FALSE(ParseDescription("this is a log line", failure).has_value());
	CHECK_FALSE(failure.empty());

	failure.clear();
	CHECK_FALSE(ParseDescription("[1, 2, 3]", failure).has_value());
	CHECK_FALSE(failure.empty());

	// **Truncated rather than malformed**, which is what a program killed
	// halfway through printing produces. It must not parse into a half-object.
	failure.clear();
	CHECK_FALSE(ParseDescription(GOOD.substr(0, GOOD.size() / 2), failure).has_value());
	CHECK_FALSE(failure.empty());
}

TEST_CASE("an object with no options is a program too old to launch", "[launcher]") {
	std::string failure;
	const auto description = ParseDescription(R"({"program": "client"})", failure);

	// A binary from before v0.18 answers `--describe` by refusing an unknown
	// option, so this shape only arrives from something stranger - but the
	// message has to name the likely cause either way, because the person in
	// front of the launcher can fix a stale tree and cannot fix a parser.
	CHECK_FALSE(description.has_value());
	CHECK(failure.find("older") != std::string::npos);
}

TEST_CASE("a missing settings array is empty rather than fatal", "[launcher]") {
	std::string failure;
	const auto description =
		ParseDescription(R"({"program": "t", "options": [{"name": "a", "takesValue": false}]})", failure);

	// Empty and absent must look the same to whoever is drawing the tab, or a
	// program with no flag table would be reported as broken.
	REQUIRE(description.has_value());
	CHECK(description->Settings.empty());
	CHECK(description->Options.size() == 1);
}

TEST_CASE("an option with no name is dropped rather than emitted", "[launcher]") {
	std::string failure;
	const auto description = ParseDescription(
		R"({"options": [{"takesValue": true}, {"name": "keep", "takesValue": false}]})", failure
	);

	// A nameless row would become `--` on a command line, which the child's own
	// parser reads as "end of options" and everything after it as positional.
	REQUIRE(description.has_value());
	REQUIRE(description->Options.size() == 1);
	CHECK(description->Options[0].Name == "keep");
}
