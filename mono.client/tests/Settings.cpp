#include <engine/core/Flags.hpp>
#include <engine/core/Paths.hpp>
#include <engine/parallel/Capture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Settings.hpp>

TEST_SUITE_ID("client.settings")
TEST_DEPENDS("engine.core.flags")

namespace {
	struct FlagScope {
		FlagScope() {
			engine::core::Flags::Reset();
		}
		~FlagScope() {
			engine::core::Flags::Reset();
		}
	};
}

TEST_CASE("client defaults remove presentation pacing while retaining the fixed tick", "[client][settings]") {
	const FlagScope flags;
	CHECK(client::OptionsFromFlags().Uncapped);
	REQUIRE(client::DeclareFlags());
	const client::Options settings = client::OptionsFromFlags();
	CHECK(settings.Uncapped);
	CHECK(settings.MaximumFrameRate == 0);
	CHECK(settings.TickRate == 60.0);
}

TEST_CASE("persisted frame pacing follows configuration precedence", "[client][settings]") {
	using engine::core::Flags;
	using engine::core::FlagSource;
	using engine::core::FlagStatus;
	const FlagScope flags;
	REQUIRE(client::DeclareFlags());
	REQUIRE(Flags::Set("client.uncapped", "false", FlagSource::ConfigFile) == FlagStatus::Applied);
	REQUIRE(Flags::Set("client.max-fps", "144", FlagSource::ConfigFile) == FlagStatus::Applied);
	CHECK_FALSE(client::OptionsFromFlags().Uncapped);
	CHECK(client::OptionsFromFlags().MaximumFrameRate == 144);

	REQUIRE(Flags::Set("client.uncapped", "true", FlagSource::Environment) == FlagStatus::Applied);
	REQUIRE(Flags::Set("client.max-fps", "240", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(client::OptionsFromFlags().Uncapped);
	CHECK(client::OptionsFromFlags().MaximumFrameRate == 240);
	CHECK(Flags::Set("client.max-fps", "30", FlagSource::ConfigFile) == FlagStatus::Outranked);
	CHECK(client::OptionsFromFlags().MaximumFrameRate == 240);
	CHECK(client::OptionsFromFlags().TickRate == 60.0);
}

TEST_CASE(
	"client CLI documents VSync and rejects conflicting present modes before startup", "[client][settings]"
) {
	const auto program =
		engine::core::Paths::Base().parent_path() / "client" / engine::core::Paths::Program("client");
	REQUIRE(std::filesystem::exists(program));
	const auto help = engine::parallel::Capture({program.string(), "--help"});
	REQUIRE(help.Started);
	REQUIRE(help.ExitCode == 0);
	CHECK(help.Output.find("--vsync") != std::string::npos);
	CHECK(help.Output.find("--max-fps") != std::string::npos);

	const auto conflict = engine::parallel::Capture({program.string(), "--uncapped", "--vsync"});
	REQUIRE(conflict.Started);
	CHECK(conflict.ExitCode == 2);
	CHECK(conflict.Output.find("--uncapped and --vsync cannot be combined") != std::string::npos);
}
