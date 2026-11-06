// Process-wide state every Studio test case starts without.
//
// Catch shuffles cases from every file into one order, so anything a case
// leaves behind reaches whichever case runs next.
//
// - **A home directory of its own.** `studio::ConfigPath` defaults to
//   `$HOME/Documents/atomic-game-engine/studio`, and a fixture that resets the
//   root with `SetConfigRoot({})` lands back on that default. Without this a
//   suite run wrote keybinds and preferences into the developer's real config.
// - **No current imgui context.** `ImGui::CreateContext` only makes the new
//   context current when none is, so a fixture that relies on that drew into
//   whatever context an earlier case left current.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <string>
#include <studio/Config.hpp>
#include <system_error>

TEST_SUITE_ID("studio.test_isolation")

namespace {
	// The developer's real home, read before any case replaces it.
	const std::optional<std::string> &RealHome() {
		static const std::optional<std::string> home = []() -> std::optional<std::string> {
			if (const char *value = std::getenv("HOME"); value != nullptr) return std::string(value);
			return std::nullopt;
		}();
		return home;
	}

	// Points `HOME` at a fresh directory for each case, removes it after, and
	// clears the current imgui context on both sides of the case.
	class TestIsolation : public Catch::EventListenerBase {
	  public:
		using Catch::EventListenerBase::EventListenerBase;

		static std::string getDescription() {
			return "Give each Studio test case a temporary home and no current imgui context";
		}

		void testCaseStarting(const Catch::TestCaseInfo &) override {
			(void)RealHome();
			std::error_code error;
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			Home = std::filesystem::temp_directory_path(error) /
				   ("atomic-studio-test-home-" + std::to_string(nonce));
			std::filesystem::create_directories(Home, error);
			::setenv("HOME", Home.c_str(), 1);
			studio::SetConfigRoot({});
			ImGui::SetCurrentContext(nullptr);
		}

		void testCaseEnded(const Catch::TestCaseStats &) override {
			ImGui::SetCurrentContext(nullptr);
			studio::SetConfigRoot({});
			std::error_code error;
			if (!Home.empty()) std::filesystem::remove_all(Home, error);
			Home.clear();
		}

	  private:
		std::filesystem::path Home;
	};
}

CATCH_REGISTER_LISTENER(TestIsolation)

TEST_CASE("a Studio test never resolves config under the real home", "[studio][config]") {
	const std::filesystem::path path = studio::ConfigPath("keybinds.json");
	REQUIRE(RealHome().has_value());
	const std::filesystem::path real = std::filesystem::path(*RealHome()) / "Documents";
	const auto relative = path.lexically_relative(real);
	CHECK((relative.empty() || relative.native().starts_with("..")));
}
