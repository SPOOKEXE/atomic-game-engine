// The two halves that can only fail against a real process.
//
// **Everything else in this module is a pure function and tested as one.** What
// is left is `--describe` actually round-tripping through a child's stdout, and
// a `Supervisor` actually reaping something - and both of those are exactly the
// kind of thing that passes as a mock and fails as a program.
//
// The subject is **this launcher's own binary**, which is the one program
// guaranteed to be staged wherever these tests are: they are configured by the
// same `MONO_BUILD_CLIENT` that configures it. Pointing them at the client
// would make them fail in a tree where somebody built one target.

#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <launcher/Description.hpp>
#include <launcher/Programs.hpp>
#include <launcher/Supervisor.hpp>
#include <thread>

TEST_SUITE_ID("launcher.live")

using namespace launcher;

namespace {
	// The staged tree, from where a test binary is run.
	//
	// Tests stage into `<stage>/tests/`, which is a sibling of every program's
	// directory - so the same `StageRoot` the launcher uses at runtime works
	// from here without a second rule.
	//
	// **`Paths::Base` and not the working directory.** The test runner is
	// started from the repository root, so a working-directory answer sent
	// every case below down its "not staged" branch and the suite passed
	// without running any of it.
	std::filesystem::path Stage() {
		return StageRoot(engine::core::Paths::Base());
	}

	std::filesystem::path Self() {
		return ProgramPath(Stage(), "launcher");
	}
}

TEST_CASE("a staged program describes itself over a pipe", "[launcher][live]") {
	if (!ProgramPresent(Self())) {
		SUCCEED("not run from a staged tree");
		return;
	}

	std::string failure;
	const auto description = ReadDescription(Self(), failure);

	REQUIRE(description.has_value());
	CHECK(failure.empty());
	CHECK(description->Program == "launcher");

	// Every program declares these three, because `Arguments` does it rather
	// than each `main`. If this ever fails, a program has been given a
	// hand-rolled parser and the launcher has stopped being able to see it.
	REQUIRE(description->Option("help") != nullptr);
	REQUIRE(description->Option("describe") != nullptr);
	REQUIRE(description->Option("mode") != nullptr);
	CHECK(description->Option("mode")->TakesValue);

	// **The version, because a stale staged tree is what this field is for.**
	// It is not compared against anything here: the number comes from the root
	// VERSION file and a test repeating it would need editing every release to
	// keep saying nothing.
	CHECK_FALSE(description->Version.empty());
}

TEST_CASE("asking a path that is not a program says so", "[launcher][live]") {
	std::string failure;
	CHECK_FALSE(ReadDescription(Stage() / "no-such-program", failure).has_value());
	CHECK(failure.find("not staged") != std::string::npos);

	// The path is in the message. A launcher reporting "no client" without
	// saying where it looked is one nobody can debug from a screenshot.
	CHECK(failure.find("no-such-program") != std::string::npos);
}

TEST_CASE("a child is started, reaped and reported", "[launcher][live]") {
	if (!ProgramPresent(Self())) {
		SUCCEED("not run from a staged tree");
		return;
	}

	Supervisor supervisor;
	CHECK(supervisor.State() == ChildState::Idle);

	std::string failure;
	REQUIRE(supervisor.Start(Self(), {"--version"}, failure));
	CHECK(failure.empty());
	CHECK(supervisor.Id() != 0);

	// `--version` prints a line and returns zero, so this settles quickly. The
	// loop is bounded rather than a wait, because a `Poll` that never reports
	// the exit is precisely the bug this is here to catch.
	for (int attempt = 0; attempt < 400 && supervisor.Running(); attempt++) {
		supervisor.Poll(engine::core::Clock::Seconds());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	REQUIRE_FALSE(supervisor.Running());
	CHECK(supervisor.State() == ChildState::Ended);
	CHECK(supervisor.Summary() == "exited 0");

	// A finished child can be dismissed, and a running one cannot - a launcher
	// that could forget a live server is one that loses track of a port.
	supervisor.Clear();
	CHECK(supervisor.State() == ChildState::Idle);
}

TEST_CASE("a child that refuses its arguments is a failure, not an end", "[launcher][live]") {
	if (!ProgramPresent(Self())) {
		SUCCEED("not run from a staged tree");
		return;
	}

	Supervisor supervisor;
	std::string failure;

	// `core::Arguments` exits 2 on an option it never declared. The two states
	// have to differ, because one of them puts a red line and a Restart button
	// on the screen and the other quietly goes back to the form.
	REQUIRE(supervisor.Start(Self(), {"--no-such-option"}, failure));
	for (int attempt = 0; attempt < 400 && supervisor.Running(); attempt++) {
		supervisor.Poll(engine::core::Clock::Seconds());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	REQUIRE_FALSE(supervisor.Running());
	CHECK(supervisor.State() == ChildState::Failed);
	CHECK(supervisor.Summary() == "exited 2");
}

TEST_CASE("starting something that is not there fails loudly", "[launcher][live]") {
	Supervisor supervisor;
	std::string failure;

	CHECK_FALSE(supervisor.Start(Stage() / "no-such-program", {}, failure));
	CHECK_FALSE(failure.empty());
	CHECK(supervisor.State() == ChildState::Failed);
}
