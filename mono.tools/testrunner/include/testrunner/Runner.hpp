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

	// One test suite, as the binary holding it describes itself.
	//
	// Built by asking a binary what it contains rather than by scanning sources,
	// so this is a report rather than a guess. See ReadSuites.
	struct Suite {
		// The identifier `TEST_SUITE_ID` declares, such as `engine.ecs.store`.
		//
		// Hand-written, and the only hand-written thing here — a name survives a
		// file being renamed or moved, which is exactly what the cache needs it
		// to do. AGENTS.md rule 4.
		std::string Id;

		// The .cpp the suite was compiled from. The root of its dependency
		// closure, and therefore of its signature.
		std::filesystem::path Source;

		// Suite ids this one is declared to depend on.
		//
		// The cascade walks these, so a signature covers everything below it and
		// a change at the bottom of the stack re-runs everything above it.
		std::vector<std::string> Depends;

		// The binary that contains it, and the file-stem filter that selects
		// only this suite's cases within it.
		std::filesystem::path Binary;
	};

	// source path -> every file that translation unit included.
	using DependencyClosures = std::map<std::filesystem::path, std::vector<std::filesystem::path>>;

	// Every executable staged under `<build>/<directory>/`.
	//
	// Parameterised because the benchmark runner wants exactly this over
	// `bench/`. The alternative — a second copy that walks a different
	// directory — is the copy that would stop matching the day this one learned
	// something.
	//
	// @param build     A configured build directory.
	// @param directory The subdirectory to walk, such as `tests` or `bench`.
	// @return The executables, sorted, so discovery order is not the
	//         filesystem's opinion.
	std::vector<std::filesystem::path>
	FindBinaries(const std::filesystem::path &build, const std::string &directory);

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

	// What the last run knew about one suite.
	//
	// The first two fields are needed to decide whether to skip. A matching
	// signature says the suite is unchanged; it does not say the suite was
	// passing.
	struct CacheEntry {
		// The suite's cascading signature the last time it ran.
		std::string Signature;

		// Whether it passed then.
		//
		// A suite that failed is re-run even when its signature still matches,
		// because the alternative is a red suite going quiet on the next
		// invocation and staying quiet until something below it happens to move.
		bool Passed = false;

		// What it counted then, so that a suite the cascade skipped today still
		// has a row in test-output.md rather than a hole.
		//
		// These decide nothing — the runner never reads them back to choose what
		// to run. They are here because the alternative is a report that covers
		// only the handful of suites one change happened to touch, which is the
		// report nobody wants: the whole point is the shape of the tree.
		// Test cases that passed when it last ran.
		unsigned CasesPassed = 0;

		// Test cases that failed then. Zero for every entry the runner is
		// willing to skip, since a failing suite is always re-run.
		unsigned CasesFailed = 0;

		// Test cases Catch2 skipped then.
		unsigned CasesSkipped = 0;

		// Assertions that held then.
		unsigned AssertionsPassed = 0;

		// Assertions that did not hold then.
		unsigned AssertionsFailed = 0;

		// What the suite cost then, in microseconds, measured around the process
		// rather than inside it.
		//
		// Per-case timings are deliberately not here. smart-tests.txt is one
		// line per suite and a person can diff it; one line per *case* would be
		// a different file with a different contract, and the report says which
		// suites it therefore cannot break down.
		unsigned long long Microseconds = 0;
	};

	// .cache/smart-tests.txt — a text file, and actually text: one tab-separated
	// line per suite, so that a diff of it is readable and a human can delete a
	// line by hand.
	std::map<std::string, CacheEntry> LoadCache(const std::filesystem::path &path);

	// Writes the cache back, replacing whatever was there.
	//
	// Ordered by suite id, because the map is, so two runs that learned the same
	// thing produce the same file and a diff shows only what moved.
	//
	// @param path  File to write. Parent directories are created.
	// @param cache Every suite worth remembering, not only the ones that ran.
	// @return False if the file could not be written. The caller decides what
	//         that is worth — a cache that failed to save costs time on the next
	//         run and nothing else, so it is not a reason to fail the tests.
	bool SaveCache(const std::filesystem::path &path, const std::map<std::string, CacheEntry> &cache);
}
