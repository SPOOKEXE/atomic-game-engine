// The suite registry, shared by every binary that declares suites.
//
// Split out of `TestMain.cpp` because a benchmark binary declares suites the
// same way and is discovered by the same `--mono-suites` listing, but links no
// Catch2 - a benchmark measures, it does not assert. One registry rather than
// two means one cascading-signature mechanism and one thing to keep correct.

#include <engine/testing/Suite.hpp>

#include <algorithm>

namespace engine::testing {

	// Function-local, so that a suite declared during static initialisation in
	// another translation unit cannot race the container's construction.
	static std::vector<Suite> &Suites() {
		static std::vector<Suite> suites;
		return suites;
	}

	Suite &Registry::Declare(std::string_view id, std::string_view file) {
		auto &suites = Suites();
		auto existing =
			std::find_if(suites.begin(), suites.end(), [id](const Suite &suite) { return suite.Id == id; });
		if (existing != suites.end()) {
			return *existing;
		}

		suites.push_back(Suite{id, file, {}});
		return suites.back();
	}

	const std::vector<Suite> &Registry::All() {
		return Suites();
	}
}
