#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sourcecheck/Rules.hpp>
#include <sourcecheck/Source.hpp>
#include <string>
#include <string_view>
#include <vector>

// Scan, check, print. Every decision that is a decision lives in the library;
// what is left here is which tree to read and how the table looks.

namespace {

	const char *Describe(sourcecheck::State state) {
		switch (state) {
		case sourcecheck::State::Waived:
			return "waived";
		case sourcecheck::State::Known:
			return "KNOWN";
		case sourcecheck::State::Open:
			break;
		}
		return "open";
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"sourcecheck",
		"Checks the four architecture rules that live in source text rather than in\n"
		"CMake's target graph - a private copy of data the ECS owns, a pointer crossing\n"
		"a world boundary, a core::Name serialised as its Id(), and a header in include/\n"
		"that nothing outside its module includes. Give it the repository root.\n"
		"\n"
		"  sourcecheck . --rule name-id\n"
		"\n"
		"Switch a rule off at one declaration with a comment above it, reason and all:\n"
		"  // arch-waiver ecs-copy: the compositor composes this; the store holds no\n"
		"  //                       composed camera"
	);
	arguments.Value(
		"rule", "NAME", "One of ecs-copy, world-pointer, name-id, public-header. May be repeated"
	);
	arguments.Flag("verbose", "List waived findings as well as open ones");

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
		std::cerr << "sourcecheck: " << parsed.Error << "\n";
		return 2;
	}

	std::vector<std::string_view> wanted = arguments.GetAll("rule");
	for (const std::string_view rule : wanted) {
		const std::vector<std::string> known = sourcecheck::RuleNames();
		if (std::find(known.begin(), known.end(), rule) == known.end()) {
			std::cerr << "sourcecheck: no rule named '" << rule << "'.\n";
			return 2;
		}
	}

	std::vector<std::string_view> roots = arguments.Positional();
	if (roots.empty()) {
		roots.emplace_back(".");
	}
	const std::string root(roots.front());
	if (!std::filesystem::is_directory(root)) {
		std::cerr << "sourcecheck: '" << root << "' is not a directory.\n";
		return 2;
	}

	const sourcecheck::Tree tree = sourcecheck::Scan(root);
	if (tree.Files.empty()) {
		// The failure the architecture fixtures exist for, one tool along: a
		// scanner that read nothing reports nothing wrong. Say so and fail
		// rather than printing four zeroes and a tick.
		std::cerr << "sourcecheck: no first-party sources under '" << root << "'.\n";
		return 2;
	}

	const sourcecheck::Report report = sourcecheck::Check(tree);
	const bool verbose = arguments.Has("verbose");

	size_t gating = 0;
	for (const std::string &rule : sourcecheck::RuleNames()) {
		if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), rule) == wanted.end()) {
			continue;
		}

		size_t open = 0;
		size_t known = 0;
		size_t waived = 0;
		std::vector<const sourcecheck::Finding *> shown;
		for (const sourcecheck::Finding &finding : report.Findings) {
			if (finding.Rule != rule) {
				continue;
			}
			switch (finding.Status) {
			case sourcecheck::State::Open:
				open++;
				shown.push_back(&finding);
				break;
			case sourcecheck::State::Known:
				known++;
				shown.push_back(&finding);
				break;
			case sourcecheck::State::Waived:
				waived++;
				if (verbose) {
					shown.push_back(&finding);
				}
				break;
			}
		}

		std::printf("\n%s - %zu open, %zu known, %zu waived\n", rule.c_str(), open, known, waived);
		for (const sourcecheck::Finding *finding : shown) {
			std::printf(
				"  %-6s %s:%zu\n         %s\n",
				Describe(finding->Status),
				finding->Path.c_str(),
				finding->Line,
				finding->Message.c_str()
			);
			if (!finding->Reason.empty()) {
				std::printf("         waiver: %s\n", finding->Reason.c_str());
			}
		}

		if (open != 0 && sourcecheck::Gating(rule)) {
			gating += open;
		}
	}

	std::printf(
		"\nscanned %zu file(s), %zu registered component(s), %zu crossing type(s), %zu public header(s)\n",
		report.Scanned,
		report.Components,
		report.Crossings,
		report.PublicHeaders
	);

	if (gating != 0) {
		std::printf("sourcecheck FAILED - %zu open finding(s) on a gating rule.\n", gating);
		return 1;
	}
	std::printf("sourcecheck ok\n");
	return 0;
}
