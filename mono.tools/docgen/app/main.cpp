#include <engine/core/Arguments.hpp>

#include <docgen/Filter.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

// Doxygen's INPUT_FILTER contract: it runs `docgen <file>` and reads the
// rewritten source on stdout. Nothing else is passed and nothing else is read,
// so the whole program is a file, a function and a write.

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"docgen",
		"Rewrites a source file's plain // comments into the /// form Doxygen reads,\n"
		"and writes the result to stdout. Wired up as INPUT_FILTER by `just docs`;\n"
		"run it by hand to see what Doxygen is being shown."
	);

	const auto parsed = arguments.Parse(argc, argv);
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (!parsed.Ok) {
		std::cerr << "docgen: " << parsed.Error << "\n";
		return 2;
	}

	const auto &positional = arguments.Positional();
	if (positional.size() != 1) {
		std::cerr << "docgen: expected one file, got " << positional.size() << "\n";
		return 2;
	}

	const std::string path(positional[0]);
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		// Writing nothing would leave Doxygen documenting an empty file and
		// saying so nowhere. Fail loudly instead — the exit code is the only
		// channel back.
		std::cerr << "docgen: cannot read " << path << "\n";
		return 1;
	}

	std::ostringstream source;
	source << file.rdbuf();

	std::cout << docgen::Promote(source.str());
	return 0;
}
