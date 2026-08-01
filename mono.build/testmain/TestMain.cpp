#include <engine/testing/Suite.hpp>

#include <catch2/catch_session.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace engine::testing {

	// Function-local, so that a suite declared during static initialisation in
	// another translation unit cannot race the container's construction.
	static std::vector<Suite> &Suites() {
		static std::vector<Suite> suites;
		return suites;
	}

	Suite &Registry::Declare(std::string_view id, std::string_view file) {
		auto &suites = Suites();
		auto existing = std::find_if(suites.begin(), suites.end(), [id](const Suite &suite) {
			return suite.Id == id;
		});
		if (existing != suites.end()) {
			return *existing;
		}

		suites.push_back(Suite { id, file, {} });
		return suites.back();
	}

	const std::vector<Suite> &Registry::All() {
		return Suites();
	}
}

int main(int argc, char **argv) {
	// Handled before Catch2 sees the command line, and it is the only argument
	// this main understands. Output is one suite per line:
	//
	//     engine.ecs.column.core<TAB>/abs/path/Column.cpp<TAB>engine.core.memory.arena
	for (int index = 1; index < argc; index++) {
		if (std::strcmp(argv[index], "--mono-suites") != 0) {
			continue;
		}

		for (const auto &suite : engine::testing::Registry::All()) {
			std::cout << suite.Id << '\t' << suite.File << '\t';
			for (size_t depth = 0; depth < suite.Depends.size(); depth++) {
				if (depth > 0) {
					std::cout << ',';
				}
				std::cout << suite.Depends[depth];
			}
			std::cout << '\n';
		}
		return 0;
	}

	return Catch::Session().run(argc, argv);
}
