#pragma once

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_case_info.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <string>
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

	// Match Catch's filename tag, including same-stem sources and hidden cases.
	// Unusual source names remain eligible rather than risking a missed test.
	inline bool HasSlowCases(std::span<const Catch::TestCaseHandle> cases, std::string_view suiteFile) {
		const std::filesystem::path source{std::string(suiteFile)};
		// The runner resolves source paths relative to its build directory, which
		// is unavailable here. Do not guess for relative paths.
		if (!source.is_absolute()) return true;
		std::error_code error;
		const auto canonical = std::filesystem::weakly_canonical(source, error);
		if (error) return true;
		const auto suiteStem = canonical.stem().string();
		if (suiteStem.empty()) return true;
		const Catch::Tag suiteTag{Catch::StringRef{suiteStem}};
		const Catch::Tag slowTag{Catch::StringRef{"slow"}};
		for (const auto &test : cases) {
			const auto &info = test.getTestCaseInfo();
			if (std::find(info.tags.begin(), info.tags.end(), slowTag) == info.tags.end()) continue;
			const std::string_view file = info.lineInfo.file;
			const auto dot = file.find_last_of('.');
			if (dot == std::string_view::npos) return true;
			const auto separator = file.find_last_of("/\\", dot);
			const auto start = separator == std::string_view::npos ? 0 : separator + 1;
			if (dot <= start) return true;
			const auto stem = file.substr(start, dot - start);
			if (Catch::Tag{Catch::StringRef{stem.data(), stem.size()}} == suiteTag) return true;
		}
		return false;
	}
}
