#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linecount/Counter.hpp>
#include <linecount/Report.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Walk, read, count, print. Every decision that is a decision lives in the
// library; what is left here is which files to hand it and where the markdown
// goes.

namespace {

	namespace fs = std::filesystem;

	// C and C++ translation units and headers. `.inl` is here because the
	// engine has a few, and a file full of code that no count includes is worse
	// than one counted under a heading somebody disagrees with.
	//
	// Anything else - CMake, GLSL, markdown - is a different language with
	// different comment rules, and counting it with C++ rules would produce a
	// number that looks right.
	bool IsSource(const fs::path &path) {
		static const std::vector<std::string> EXTENSIONS = {
			".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".hxx", ".h", ".inl"
		};

		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return std::find(EXTENSIONS.begin(), EXTENSIONS.end(), extension) != EXTENSIONS.end();
	}

	// Relative to where the command was run, with `/` separators, so that a
	// report generated on Windows and one generated on Linux are the same
	// document. A path outside the working directory keeps the form it was
	// given rather than becoming a row of `../..`.
	std::string Display(const fs::path &path) {
		std::error_code error;
		const fs::path relative = fs::relative(path, fs::current_path(), error);
		if (!error && !relative.empty() && relative.begin()->string() != "..") {
			return relative.generic_string();
		}
		return path.lexically_normal().generic_string();
	}

	bool Excluded(const std::string &path, const std::vector<std::string> &patterns) {
		for (const std::string &pattern : patterns) {
			if (path.find(pattern) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	// A directory nobody asked to be counted.
	//
	// The dot rule is the one that matters. `.cache/build/<preset>/` holds
	// generated headers and a configured vendor tree, so a walk of the
	// repository root without it counts the build twice over - once as sources
	// and once as whatever CMake copied - and the number moves depending on
	// which presets happen to be configured on the machine.
	bool SkipDirectory(const fs::path &path) {
		const std::string name = path.filename().string();
		return name.size() > 1 && name[0] == '.';
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
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"linecount",
		"Counts empty, comment and code lines across C and C++ sources and writes the\n"
		"breakdown as markdown on stdout. Give it directories or files; with no\n"
		"arguments it walks the current directory.\n"
		"\n"
		"  linecount mono.engine > LINECOUNT.md"
	);
	arguments.Value("depth", "N", "How many path segments a directory row groups by (default 2)");
	arguments.Flag("files", "Append a row per file, not just per directory");
	arguments.Value("exclude", "TEXT", "Skip any path containing TEXT. May be given more than once");
	arguments.Flag("vendor", "Count mono.vendor too, which is excluded by default");

	const auto parsed = arguments.Parse(argc, argv);
	if (parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (!parsed.Ok) {
		std::cerr << "linecount: " << parsed.Error << "\n";
		return 2;
	}

	// **Excluded unless asked for.** A walk of the repository root that
	// includes mono.vendor is SDL, imgui, Tracy, Crypto++ and shaderc, which
	// together are an order of magnitude more code than the engine - so the
	// default report would answer a question about somebody else's project and
	// look exactly like the one that was wanted.
	std::vector<std::string> excludes;
	if (!arguments.Has("vendor")) {
		excludes.emplace_back("mono.vendor");
	}
	for (const std::string_view pattern : arguments.GetAll("exclude")) {
		excludes.emplace_back(pattern);
	}

	std::vector<std::string_view> roots = arguments.Positional();
	if (roots.empty()) {
		roots.push_back(".");
	}

	std::vector<linecount::FileCounts> files;
	for (const std::string_view root : roots) {
		const fs::path path(root);
		std::error_code error;

		if (!fs::exists(path, error)) {
			std::cerr << "linecount: no such path - " << root << "\n";
			return 1;
		}

		std::vector<fs::path> candidates;
		if (fs::is_directory(path, error)) {
			// skip_permission_denied rather than a throwing iterator: a tree
			// with one unreadable directory in it should still produce the
			// count of everything else.
			fs::recursive_directory_iterator walk(path, fs::directory_options::skip_permission_denied, error);
			if (error) {
				std::cerr << "linecount: cannot read " << root << " - " << error.message() << "\n";
				return 1;
			}
			for (auto entry = walk; entry != fs::recursive_directory_iterator(); entry.increment(error)) {
				if (error) {
					std::cerr << "linecount: " << error.message() << "\n";
					return 1;
				}
				if (entry->is_directory() && SkipDirectory(entry->path())) {
					entry.disable_recursion_pending();
					continue;
				}
				if (entry->is_regular_file() && IsSource(entry->path())) {
					candidates.push_back(entry->path());
				}
			}
		} else {
			// A file named outright is counted whatever its extension is. The
			// filter exists to decide what a *walk* picks up; naming one file
			// is not ambiguous, and refusing it would be the tool arguing with
			// somebody who was more specific than it is.
			candidates.push_back(path);
		}

		for (const fs::path &candidate : candidates) {
			const std::string display = Display(candidate);
			if (Excluded(display, excludes)) {
				continue;
			}

			std::string source;
			if (!ReadFile(candidate, source)) {
				// Said out loud rather than silently dropped. A file missing
				// from a total is invisible, and the total still looks like an
				// answer.
				std::cerr << "linecount: cannot read " << display << ", skipping\n";
				continue;
			}
			files.push_back({display, linecount::Count(source)});
		}
	}

	// Sorted before grouping so that the report does not depend on the order
	// the filesystem happened to hand the entries over in.
	std::sort(
		files.begin(),
		files.end(),
		[](const linecount::FileCounts &left, const linecount::FileCounts &right) {
			return left.Path < right.Path;
		}
	);

	linecount::ReportOptions options;
	options.GroupDepth = static_cast<size_t>(std::max<int64_t>(0, arguments.GetInteger("depth", 2)));
	options.IncludeFiles = arguments.Has("files");

	std::cout << linecount::Markdown(files, options);
	return 0;
}
