#include <engine/core/Log.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

// **The only include of spdlog outside `Log.cpp`, and it is here on purpose.**
// `Log.hpp` forward-declares `spdlog::logger` so that the two hundred and fifty
// translation units which only want to log do not pay for the library. A caller
// that wants to do something *with* the logger - install a sink, as
// `mono.studio` does, or call a method on it, as the first case below does -
// completes the type itself. That this file has to is the invariant working,
// not a gap in it.
#include <spdlog/spdlog.h>

TEST_SUITE_ID("engine.core.log")
TEST_DEPENDS("engine.core.name")

using engine::core::Log;
using engine::core::LogCategory;
using engine::core::LogLevel;
using engine::core::LogThrottle;

namespace {
	// How many times an argument to a log statement has been evaluated.
	//
	// **The whole point of the guard, as a number.** Before v0.19 the macros
	// evaluated their arguments whether the level was on or not, so this would
	// count up on a statement nobody could see.
	int Evaluations = 0;

	int Counted() {
		Evaluations++;
		return Evaluations;
	}
}

TEST_CASE("Logger works before Initialise", "[log]") {
	// A log line during static initialisation must not reach an unconfigured
	// sink. Losing the line would be worse than the bug that produced it, so
	// Logger falls back rather than crashing.
	REQUIRE_NOTHROW(Log::Logger().info("before initialise"));
}

TEST_CASE("Initialise is idempotent", "[log]") {
	Log::Initialise("test");
	auto &first = Log::Logger();

	// Every program's main calls it, and so does the test main. Calling twice
	// must not replace the sink underneath whoever already holds a reference.
	Log::Initialise("test");
	Log::Initialise("something else");

	REQUIRE(&Log::Logger() == &first);
}

TEST_CASE("every level has a name and reads back from it", "[log]") {
	for (auto level :
		 {LogLevel::Trace,
		  LogLevel::Debug,
		  LogLevel::Info,
		  LogLevel::Warning,
		  LogLevel::Error,
		  LogLevel::Off}) {
		LogLevel read = LogLevel::Info;
		REQUIRE(engine::core::LevelFromName(engine::core::Describe(level), read));
		CHECK(read == level);
	}

	LogLevel read = LogLevel::Info;
	CHECK(engine::core::LevelFromName("WARN", read));
	CHECK(read == LogLevel::Warning);

	// Refused rather than defaulted: a misspelled level that silently means
	// info is a deployment that thinks it turned tracing on and did not.
	CHECK_FALSE(engine::core::LevelFromName("trce", read));
	CHECK_FALSE(engine::core::LevelFromName("", read));
}

TEST_CASE("the macros compile and route to the logger", "[log]") {
	Log::Initialise("test");
	Log::SetLevel(LogLevel::Error);

	// Formatting happens inside fmt, so a mismatched argument count is a
	// compile error here rather than a runtime surprise in a rare branch. The
	// statements are written bare rather than inside `REQUIRE_NOTHROW`, because
	// a log statement is a statement now and not an expression - which is what
	// lets it be a guarded block.
	ENGINE_TRACE("trace {}", 1);
	ENGINE_DEBUG("debug {}", 1);
	ENGINE_INFO("info {} {}", 1, "two");
	ENGINE_WARN("warn {:.2f}", 1.5);
	ENGINE_ERROR("error");

	Log::SetLevel(LogLevel::Info);
	SUCCEED("every macro compiled and ran");
}

TEST_CASE("a disabled statement evaluates nothing", "[log]") {
	Log::Initialise("test");
	Log::SetLevel(LogLevel::Error);

	Evaluations = 0;
	ENGINE_INFO("never printed {}", Counted());
	ENGINE_WARN("never printed {}", Counted());
	CHECK(Evaluations == 0);

	// And the other half: an enabled statement still evaluates exactly once.
	Log::SetLevel(LogLevel::Warning);
	ENGINE_WARN("test: an enabled statement evaluates its arguments once ({})", Counted());
	CHECK(Evaluations == 1);

	Log::SetLevel(LogLevel::Info);
}

TEST_CASE("a statement below the compiled floor evaluates nothing", "[log]") {
	Log::SetLevel(LogLevel::Trace);

	Evaluations = 0;

	// `ENGINE_TRACE` rather than `ENGINE_LOG`, because only the named macros
	// can be compile-time filtered: `ENGINE_LOG` takes its level as an
	// expression and the preprocessor cannot see one.
	ENGINE_TRACE("test: trace {}", Counted());

	// The floor is `trace` in every preset that builds tests, so the statement
	// above normally runs. Written as a comparison rather than as a constant so
	// that a build configured with `MONO_LOG_LEVEL=info` checks the other half
	// of the claim instead of failing.
	if (Log::CompiledFloor() > LogLevel::Trace) {
		CHECK(Evaluations == 0);
	} else {
		CHECK(Evaluations == 1);
	}

	Log::SetLevel(LogLevel::Info);
}

TEST_CASE("a category can be turned up on its own", "[log]") {
	Log::SetLevel(LogLevel::Warning);
	Log::SetLevel("net", LogLevel::Trace);

	CHECK(Log::Enabled(LogLevel::Trace, "net"));
	CHECK_FALSE(Log::Enabled(LogLevel::Trace, "render"));
	CHECK(Log::Enabled(LogLevel::Warning, "render"));

	// The handle form is the one every macro uses, and it must agree with the
	// by-name form it is an optimisation of.
	const LogCategory net("net");
	CHECK(net.Enabled(LogLevel::Trace));
	CHECK(net.Text() == "net");
	CHECK(net.IsValid());

	const LogCategory nothing;
	CHECK_FALSE(nothing.IsValid());
	CHECK_FALSE(nothing.Enabled(LogLevel::Error));

	// A bare level means every category, including one that was overridden. A
	// person typing `--log trace` after a config file said `net=warning` means
	// trace, and anything else is a setting that appears to do nothing.
	Log::SetLevel(LogLevel::Error);
	CHECK_FALSE(net.Enabled(LogLevel::Trace));

	Log::SetLevel(LogLevel::Info);
}

TEST_CASE("Configure reads a level, a category or both", "[log]") {
	std::string_view unknown;

	REQUIRE(Log::Configure("warning", &unknown));
	CHECK(Log::Level() == LogLevel::Warning);

	REQUIRE(Log::Configure("info,net=trace,physics=off", &unknown));
	CHECK(Log::Level() == LogLevel::Info);
	CHECK(Log::LevelOf("net") == LogLevel::Trace);
	CHECK(Log::LevelOf("physics") == LogLevel::Off);
	CHECK(Log::LevelOf("render") == LogLevel::Info);

	// Order must not matter. A bare term applies to every category, so writing
	// it after a named one would otherwise undo the named one.
	REQUIRE(Log::Configure("net=trace,warning", &unknown));
	CHECK(Log::Level() == LogLevel::Warning);
	CHECK(Log::LevelOf("net") == LogLevel::Trace);

	// Whitespace, and an empty specification, are both what a config file
	// hands over rather than an error.
	REQUIRE(Log::Configure(" info , net = debug ", &unknown));
	CHECK(Log::LevelOf("net") == LogLevel::Debug);
	REQUIRE(Log::Configure("", &unknown));
	CHECK(Log::Level() == LogLevel::Info);

	// A misspelling applies nothing at all, so that a setting cannot half work.
	CHECK_FALSE(Log::Configure("net=trace,physics=trce", &unknown));
	CHECK(unknown == "physics=trce");
	CHECK(Log::LevelOf("net") == LogLevel::Debug);

	Log::Configure("info", &unknown);
}

TEST_CASE("every registered category can be listed", "[log]") {
	Log::SetLevel("engine.core.test.listed", LogLevel::Error);

	bool found = false;
	for (uint32_t index = 0; index < Log::CategoryCount(); index++) {
		const LogCategory category = Log::CategoryAt(index);
		REQUIRE(category.IsValid());
		if (category.Text() == "engine.core.test.listed") {
			found = true;
			CHECK(Log::LevelOf(category.Text()) == LogLevel::Error);
		}
	}
	CHECK(found);

	// The registry never removes an entry, so an index past the end is the only
	// way to get an invalid handle back.
	CHECK_FALSE(Log::CategoryAt(Log::CategoryCount()).IsValid());
}

TEST_CASE("a throttle counts what it suppressed", "[log]") {
	LogThrottle throttle;

	// The first statement always passes, standing for itself and nothing else.
	REQUIRE(throttle.Due(0.010) == 1);
	CHECK(throttle.Due(0.010) == 0);
	CHECK(throttle.Due(0.010) == 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(25));

	// One plus the two it refused, so the line it writes can say what it stands
	// for rather than pretending it was the only one.
	CHECK(throttle.Due(0.010) == 3);

	// A zero gap is not a throttle at all, which is what makes `_EVERY(0, ...)`
	// mean "every time" rather than "never".
	LogThrottle open;
	CHECK(open.Due(0.0) == 1);
	CHECK(open.Due(0.0) == 1);
}

TEST_CASE("a throttled statement evaluates nothing while it is quiet", "[log]") {
	Log::Initialise("test");
	Log::SetLevel(LogLevel::Warning);

	Evaluations = 0;
	for (int pass = 0; pass < 100; pass++) {
		ENGINE_WARN_EVERY(30.0, "test: a throttled warning ({})", Counted());
	}

	// Once for the line that was written, and not ninety-nine more times for
	// the ones that were not. A per-frame fault costing a format per frame is
	// the thing the throttle exists to stop.
	CHECK(Evaluations == 1);

	Log::SetLevel(LogLevel::Info);
}
