#pragma once

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_case_info.hpp>

#include <set>
#include <span>
#include <string_view>

namespace engine::testing::detail {

	// Use compiler paths, including hidden cases: basename matching can hide a
	// missing suite behind an unrelated source with the same filename.
	inline std::set<std::string_view>
	MissingSuiteSources(std::span<const Catch::TestCaseHandle> cases, std::span<const Suite> suites) {
		std::set<std::string_view> declared;
		for (const auto &suite : suites)
			if (!suite.File.empty()) declared.insert(suite.File);

		std::set<std::string_view> missing;
		for (const auto &test : cases) {
			const std::string_view source = test.getTestCaseInfo().lineInfo.file;
			if (!declared.contains(source)) missing.insert(source);
		}
		return missing;
	}
}
