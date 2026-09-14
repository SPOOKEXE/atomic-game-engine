// The persisted server data-factory option reaches the same Options field the
// command-line entry point uses. A flag that only appears in a config table is
// an inert setting, so this stays beside the host lifecycle tests.

#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <server/Settings.hpp>

TEST_SUITE_ID("server.settings")
TEST_DEPENDS("engine.core.flags")

using engine::core::Flags;
using engine::core::FlagSource;

TEST_CASE("data factory host mode is available through server settings", "[server][settings][data-factory]") {
	Flags::Reset();
	REQUIRE(server::DeclareFlags());
	CHECK_FALSE(server::OptionsFromFlags().DataFactory);
	REQUIRE(
		Flags::Set("server.data-factory", "true", FlagSource::ConfigFile) == engine::core::FlagStatus::Applied
	);
	REQUIRE(
		Flags::Set("server.control-port", "8734", FlagSource::CommandLine) ==
		engine::core::FlagStatus::Applied
	);
	const server::Options options = server::OptionsFromFlags();
	CHECK(options.DataFactory);
	CHECK(options.ControlPort == 8734);
	Flags::Reset();
}
