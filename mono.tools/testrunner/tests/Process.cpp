#include <engine/testing/Suite.hpp>
#include <testrunner/Process.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("tools.testrunner.process")

using testrunner::Run;

TEST_CASE("a program that does not exist did not start", "[process]") {
	const auto result = Run({ "/nonexistent/definitely-not-a-program" });

	// Distinct from a non-zero exit, which is the program running and
	// disagreeing with you. Conflating the two turns a typo in a path into
	// "your tests failed".
	REQUIRE_FALSE(result.Started);
}

TEST_CASE("an empty argument list does nothing", "[process]") {
	REQUIRE_FALSE(Run({}).Started);
}

TEST_CASE("this test binary can list its own suites", "[process]") {
	// The runner's discovery path, exercised against the one binary that is
	// guaranteed to be present and executable while this test is running.
	const auto self = Run({ "/proc/self/exe", "--mono-suites" });

	if (!self.Started) {
		// No /proc — a BSD or a container without it. The rest of the suite
		// still covers the parsing.
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	REQUIRE(self.ExitCode == 0);
	REQUIRE(self.Output.find("tools.testrunner.process") != std::string::npos);
	// Tab-separated: id, source, dependencies.
	REQUIRE(self.Output.find('\t') != std::string::npos);
}

TEST_CASE("a non-zero exit is reported without losing the output", "[process]") {
	const auto result = Run({ "/proc/self/exe", "--this-is-not-a-catch2-option" });
	if (!result.Started) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	REQUIRE(result.ExitCode != 0);
	// Output has to survive a failure, or a failing suite reports nothing
	// useful — which is precisely when the output is wanted.
	REQUIRE_FALSE(result.Output.empty());
}

TEST_CASE("output larger than a pipe buffer does not deadlock", "[process]") {
	// Waiting for the child before draining the pipe deadlocks as soon as it
	// writes more than 64 KiB, which for a verbose test run is immediately.
	// This lists every assertion in this binary, which is comfortably past it.
	const auto result = Run({ "/proc/self/exe", "--list-tests", "--verbosity", "high" });
	if (!result.Started) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	REQUIRE(result.ExitCode == 0);
	REQUIRE(result.Output.size() > 200);
}
