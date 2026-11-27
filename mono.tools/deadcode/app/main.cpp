#include <engine/core/Arguments.hpp>

#include <deadcode/Elf.hpp>
#include <deadcode/Graph.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Find the objects, sort them into programs, tests and vendor, and print what
// the library says nothing reaches. The reachability itself lives in
// `deadcode/Graph.hpp`.

namespace {

	namespace fs = std::filesystem;

	struct Located {
		std::string Source;
		deadcode::Role Kind = deadcode::Role::Ship;
	};

	// `mono.engine/core/CMakeFiles/engine_core.dir/src/Base64.cpp.o` is
	// `mono.engine/core/src/Base64.cpp`, owned by target `engine_core`.
	// Anything outside a `mono.*` directory is a configure probe and is
	// skipped.
	bool Locate(const std::string &relative, Located &out) {
		if (!relative.starts_with("mono.")) {
			return false;
		}
		if (relative.starts_with("mono.vendor")) {
			out = {relative, deadcode::Role::Vendor};
			return true;
		}
		const size_t files = relative.find("/CMakeFiles/");
		if (files == std::string::npos) {
			return false;
		}
		const size_t targetStart = files + 12;
		const size_t dir = relative.find(".dir/", targetStart);
		if (dir == std::string::npos) {
			return false;
		}
		const std::string target = relative.substr(targetStart, dir - targetStart);
		std::string rest = relative.substr(dir + 5);
		rest.resize(rest.size() - 2);

		out.Source = relative.substr(0, files) + "/" + rest;
		const bool test = target.starts_with("test_") || target == "mono_testmain" || target == "mono_suites" ||
			rest.starts_with("tests/");
		out.Kind = test ? deadcode::Role::Test : deadcode::Role::Ship;
		return true;
	}

	bool ReadFile(const fs::path &path, std::string &out) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return false;
		}
		std::ostringstream contents;
		contents << file.rdbuf();
		out = contents.str();
		return true;
	}

	void PrintGroup(const char *heading, const std::vector<deadcode::Function> &functions,
		const std::map<std::string, size_t> &perSource, bool list) {
		std::map<std::string, std::vector<const deadcode::Function *>> bySource;
		uint64_t bytes = 0;
		for (const deadcode::Function &function : functions) {
			bySource[function.Source].push_back(&function);
			bytes += function.Size;
		}

		// Whole sources first: a file nothing reaches is the biggest single
		// thing to delete.
		std::vector<std::string> whole;
		std::vector<std::string> partial;
		for (const auto &[source, entries] : bySource) {
			size_t own = 0;
			for (const deadcode::Function *function : entries) {
				own += function->Inline ? 0 : 1;
			}
			const auto total = perSource.find(source);
			const bool all = total != perSource.end() && total->second > 0 && own == total->second;
			(all ? whole : partial).push_back(source);
		}

		std::printf("%s: %zu function(s), %llu bytes, in %zu source(s), %zu of them entirely\n", heading,
			functions.size(), static_cast<unsigned long long>(bytes), bySource.size(), whole.size());

		auto print = [&](const std::string &source, bool entire) {
			const auto &entries = bySource[source];
			const auto total = perSource.find(source);
			if (entire) {
				std::printf("  %s  whole file, %zu function(s)\n", source.c_str(), total->second);
			} else {
				std::printf("  %s  %zu of %zu\n", source.c_str(), entries.size(),
					total == perSource.end() ? entries.size() : total->second);
			}
			if (!list) {
				return;
			}
			for (const deadcode::Function *function : entries) {
				std::printf("      %s%s\n", deadcode::Demangle(function->Name).c_str(),
					function->Inline ? "  [inline]" : "");
			}
		};
		for (const std::string &source : whole) {
			print(source, true);
		}
		for (const std::string &source : partial) {
			print(source, false);
		}
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"deadcode",
		"Reports first-party functions that no program can reach, from the\n"
		"relocations in a build's object files. Give it a build directory built\n"
		"without optimisation (the dev preset); with no argument it reads\n"
		".cache/build/dev.\n"
		"\n"
		"  deadcode .cache/build/dev --list"
	);
	arguments.Flag("list", "Print every function under its source, not just the counts");
	arguments.Flag("tests", "Also report functions only tests reach");
	arguments.Value("filter", "TEXT", "Only report sources whose path contains TEXT");
	arguments.Flag("fail", "Exit 1 when anything is dead");

	const auto parsed = arguments.Parse(argc, argv);
	if (parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}
	if (!parsed.Ok) {
		std::cerr << "deadcode: " << parsed.Error << "\n";
		return 2;
	}

	const std::vector<std::string_view> &positional = arguments.Positional();
	const fs::path build = positional.empty() ? fs::path(".cache/build/dev") : fs::path(positional[0]);
	std::error_code error;
	if (!fs::is_directory(build, error)) {
		std::cerr << "deadcode: no build directory at " << build.string() << "\n";
		return 1;
	}

	// Ship objects first, so a symbol defined by both a program and a test is
	// owned by the program.
	std::vector<deadcode::Input> inputs;
	size_t skipped = 0;
	fs::recursive_directory_iterator walk(build, fs::directory_options::skip_permission_denied, error);
	for (auto entry = walk; entry != fs::recursive_directory_iterator(); entry.increment(error)) {
		if (error) {
			std::cerr << "deadcode: " << error.message() << "\n";
			return 1;
		}
		if (!entry->is_regular_file() || entry->path().extension() != ".o") {
			continue;
		}
		const std::string relative = fs::relative(entry->path(), build, error).generic_string();
		Located located;
		if (!Locate(relative, located)) {
			continue;
		}
		// An object whose source is gone is an orphan `just orphan-check`
		// reports. Counting it would report code that no longer exists.
		if (located.Kind != deadcode::Role::Vendor && !fs::exists(located.Source, error) &&
			!fs::exists(build / located.Source, error)) {
			skipped += 1;
			continue;
		}

		std::string bytes;
		deadcode::Input input;
		std::string problem;
		if (!ReadFile(entry->path(), bytes) || !deadcode::ReadObject(bytes, input.Object, problem)) {
			std::cerr << "deadcode: cannot read " << relative << " " << problem << "\n";
			continue;
		}
		input.Source = std::move(located.Source);
		input.Kind = located.Kind;
		inputs.push_back(std::move(input));
	}
	std::stable_sort(inputs.begin(), inputs.end(), [](const deadcode::Input &a, const deadcode::Input &b) {
		return static_cast<int>(a.Kind) < static_cast<int>(b.Kind);
	});

	// An object with several functions in one section hides the calls
	// between them, so its live code can look dead. One odd target (a fixture
	// built with its own flags) is named and kept; a whole build like that is
	// the wrong build and is refused, because a long wrong list reads exactly
	// like a long right one.
	size_t ship = 0;
	std::vector<std::string> coarse;
	for (const deadcode::Input &input : inputs) {
		if (input.Kind == deadcode::Role::Ship) {
			ship += 1;
			if (deadcode::SharesCodeSection(input.Object)) {
				coarse.push_back(input.Source);
			}
		}
	}
	if (!coarse.empty() && coarse.size() * 10 > ship) {
		std::cerr << "deadcode: " << coarse.size() << " of " << ship << " object(s) under " << build.string()
				  << " put several functions in one section, so calls between them are invisible.\n"
				  << "Use an unoptimised build, which compiles with -ffunction-sections.\n";
		return 1;
	}
	for (const std::string &source : coarse) {
		std::cerr << "deadcode: " << source << " shares code sections, so its results may list live code\n";
	}
	if (ship == 0) {
		std::cerr << "deadcode: no first-party objects under " << build.string() << "\n";
		return 1;
	}

	deadcode::Analysis analysis = deadcode::Analyse(inputs);

	if (const auto filter = arguments.Get("filter")) {
		auto drop = [&](std::vector<deadcode::Function> &functions) {
			std::erase_if(functions, [&](const deadcode::Function &function) {
				return function.Source.find(*filter) == std::string::npos;
			});
		};
		drop(analysis.Dead);
		drop(analysis.TestOnly);
	}

	std::printf("read %zu object(s), %zu of them program code", inputs.size(), ship);
	if (skipped > 0) {
		std::printf(", skipped %zu orphan(s)", skipped);
	}
	std::printf("\n");
	PrintGroup("unreachable", analysis.Dead, analysis.FunctionsPerSource, arguments.Has("list"));
	if (arguments.Has("tests")) {
		PrintGroup("test-only", analysis.TestOnly, analysis.FunctionsPerSource, arguments.Has("list"));
	}

	return arguments.Has("fail") && !analysis.Dead.empty() ? 1 : 0;
}
