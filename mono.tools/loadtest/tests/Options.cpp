// The flag table, and that what it declares is what `Options` defaults to.
//
// **The failure this catches is a flag that was parsed into a field nothing
// read.** `server/tests` has the same case for the same reason: a load test run
// with `--clients 200` that silently opened eight is a measurement of the wrong
// thing, and nothing about the run says so.

#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <loadtest/Options.hpp>
#include <string>

TEST_SUITE_ID("tools.loadtest.options")

using engine::core::Flags;
using engine::core::FlagSource;

namespace {
	// The flag table is process-wide, so a case starts from an empty one.
	void Fresh() {
		Flags::Reset();
		REQUIRE(loadtest::DeclareFlags());
	}
}

TEST_CASE("an undeclared table leaves the built-in defaults alone", "[loadtest]") {
	Flags::Reset();

	// A program that never declared them still runs, on the member initialisers
	// in the header. Reading a dead flag would answer zero for every one of
	// them, which is a hundred clients turning into none.
	const loadtest::Options options = loadtest::OptionsFromFlags();
	REQUIRE(options.Clients == 200);
	REQUIRE(options.TickRate == 30.0);
}

TEST_CASE("every option is reachable by its flag", "[loadtest]") {
	Fresh();

	REQUIRE(
		Flags::Set("loadtest.clients", "64", FlagSource::CommandLine) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.address", "10.0.0.2", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.port", "9999", FlagSource::CommandLine) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.tick-rate", "60", FlagSource::CommandLine) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.seconds", "12.5", FlagSource::CommandLine) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.ticks", "900", FlagSource::CommandLine) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.connects-per-tick", "3", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.input-every-ticks", "4", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.stall-seconds", "5.5", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("loadtest.profile-out", "out.folded", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);

	const loadtest::Options options = loadtest::OptionsFromFlags();
	REQUIRE(options.Clients == 64);
	REQUIRE(options.Address == "10.0.0.2");
	REQUIRE(options.Port == 9999);
	REQUIRE(options.TickRate == 60.0);
	REQUIRE(options.Seconds == 12.5);
	REQUIRE(options.Ticks == 900);
	REQUIRE(options.ConnectsPerTick == 3);
	REQUIRE(options.InputEveryTicks == 4);
	REQUIRE(options.StallSeconds == 5.5);
	REQUIRE(options.ProfilePath == "out.folded");

	Flags::Reset();
}

TEST_CASE("the declared defaults are the header's defaults", "[loadtest]") {
	Fresh();

	// The table is built from a default-constructed `Options`, so this holds by
	// construction - and it is checked because the way it stops holding is
	// somebody typing a literal into the table beside a member that says
	// something else.
	const loadtest::Options declared = loadtest::OptionsFromFlags();
	const loadtest::Options defaults;
	REQUIRE(declared.Clients == defaults.Clients);
	REQUIRE(declared.TickRate == defaults.TickRate);
	REQUIRE(declared.ConnectsPerTick == defaults.ConnectsPerTick);
	REQUIRE(declared.InputEveryTicks == defaults.InputEveryTicks);
	REQUIRE(declared.StallSeconds == defaults.StallSeconds);
	REQUIRE(declared.Address == defaults.Address);

	Flags::Reset();
}
