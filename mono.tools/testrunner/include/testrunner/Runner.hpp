#pragma once

// The selective test runner.
//
// Re-runs the suites a change could have affected, and skips the rest. The
// property that matters is the cascading signature hash: a suite's signature is
// a hash of its own source, every header that source includes, and the
// signatures of every suite it declares a dependency on. A change at the bottom
// of the stack therefore changes the signature of everything transitively above
// it and of nothing else — which is what a timestamp cannot give, and what lets
// CI and a laptop share a cache.
//
// Hand-declared identifiers, derived file sets. A hand-written list of files a
// test touches goes stale silently, and a stale list means a skipped test that
// should have run — the worst failure mode a runner has.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace testrunner {

	struct Suite {
		std::string Id;
		std::filesystem::path Source;
		std::vector<std::string> Depends;

		// The binary that contains it, and the file-stem filter that selects
		// only this suite's cases within it.
		std::filesystem::path Binary;
	};

	// source path -> every file that translation unit included.
	using DependencyClosures = std::map<std::filesystem::path, std::vector<std::filesystem::path>>;

	// Every executable staged under <build>/tests/.
	std::vector<std::filesystem::path> FindTestBinaries(const std::filesystem::path &build);

	// Asks a binary what it contains, rather than scanning sources for the
	// macro and hoping the pattern holds.
	std::vector<Suite> ReadSuites(const std::filesystem::path &binary);

	// Read back out of Ninja's dependency database. CMake tells the compiler to
	// write .d files and then hands them to Ninja, which consumes and deletes
	// them; `ninja -t deps` is the supported way to get the data afterwards.
	DependencyClosures ReadDependencyClosures(const std::filesystem::path &build);

	// suite id -> signature. Cycles and unknown dependencies are reported and
	// broken rather than allowed to recurse.
	std::map<std::string, std::string> ComputeSignatures(
		const std::vector<Suite> &suites,
		const DependencyClosures &closures,
		std::vector<std::string> &warnings
	);

	struct CacheEntry {
		std::string Signature;
		bool Passed = false;
	};

	// .cache/smart-tests.txt — a text file, and actually text: one tab-separated
	// line per suite, so that a diff of it is readable and a human can delete a
	// line by hand.
	std::map<std::string, CacheEntry> LoadCache(const std::filesystem::path &path);
	bool SaveCache(const std::filesystem::path &path, const std::map<std::string, CacheEntry> &cache);
}
