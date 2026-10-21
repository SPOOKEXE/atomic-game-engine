#pragma once

#include <engine/testing/Scenario.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace client::tests {
	// Match the existing Approx comparisons without calling Catch on a worker.
	inline bool Near(double actual, double expected, double margin = 0.0) {
		const double epsilon = static_cast<double>(std::numeric_limits<float>::epsilon()) * 100.0;
		const double tolerance = epsilon * std::abs(std::isinf(expected) ? 0.0 : expected);
		const auto within = [actual, expected](double distance) {
			return expected + distance >= actual && actual + distance >= expected;
		};
		return within(margin) || within(tolerance);
	}

	// A filtered case still pays for its compatible scenario's full timeline.
	// Only the selected original case reports its findings, on the owner thread.
	inline void CheckScenario(const engine::testing::ScenarioReport &report, std::string_view name) {
		for (const auto &failure : report.Failures) {
			INFO(failure);
			CHECK(false);
		}
		const auto found = std::ranges::find(report.Checks, name, &engine::testing::CheckResult::Name);
		REQUIRE(found != report.Checks.end());
		for (const auto &finding : found->Findings) {
			INFO(finding.Property << ": " << finding.Message);
			CHECK(finding.Passed);
		}
		CHECK(found->Passed);
	}
}
