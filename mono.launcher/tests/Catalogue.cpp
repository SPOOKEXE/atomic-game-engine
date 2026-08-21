// The modes, and what they promise about the programs beside them.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <launcher/Catalogue.hpp>
#include <set>

TEST_SUITE_ID("launcher.catalogue")

using namespace launcher;

TEST_CASE("every mode has a distinct id and a program", "[launcher]") {
	const auto modes = Modes();
	REQUIRE_FALSE(modes.empty());

	std::set<std::string> ids;
	for (const Mode &mode : modes) {
		CHECK_FALSE(mode.Id.empty());
		CHECK_FALSE(mode.Label.empty());
		CHECK_FALSE(mode.Blurb.empty());
		CHECK_FALSE(mode.Program.empty());

		// `--mode play` is answered by the id, and rule 4 is about exactly
		// this: two modes sharing one would make which of them opens depend on
		// the order of this list.
		CHECK(ids.insert(mode.Id).second);
	}
}

TEST_CASE("the programs asked for are each asked once", "[launcher]") {
	const auto modes = Modes();
	const auto programs = ProgramsOf(modes);

	// Play and Join are both the client. Describing it twice would double the
	// slowest part of startup for nothing.
	std::set<std::string> seen(programs.begin(), programs.end());
	CHECK(seen.size() == programs.size());

	for (const Mode &mode : modes) {
		CHECK(seen.count(mode.Program) == 1);
	}
}

TEST_CASE("a service mode is supervised and a session mode is not", "[launcher]") {
	const auto modes = Modes();

	// The difference is whether the launcher is any use after the child
	// starts. A host and an origin are things you watch, stop and restart; a
	// game and an editor take the display.
	REQUIRE(FindMode(modes, "host") != nullptr);
	REQUIRE(FindMode(modes, "cdn") != nullptr);
	CHECK(FindMode(modes, "host")->After == Lifetime::Supervise);
	CHECK(FindMode(modes, "cdn")->After == Lifetime::Supervise);

	REQUIRE(FindMode(modes, "play") != nullptr);
	REQUIRE(FindMode(modes, "studio") != nullptr);
	CHECK(FindMode(modes, "play")->After == Lifetime::HandOver);
	CHECK(FindMode(modes, "studio")->After == Lifetime::HandOver);
}

TEST_CASE("finding a mode that is not there answers null", "[launcher]") {
	CHECK(FindMode(Modes(), "nonsense") == nullptr);
}
