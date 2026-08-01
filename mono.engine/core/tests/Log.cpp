#include <engine/core/Log.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.log")

using engine::core::Log;
using engine::core::LogLevel;

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

TEST_CASE("every level maps to something", "[log]") {
	Log::Initialise("test");

	for (auto level : {LogLevel::Trace, LogLevel::Info, LogLevel::Warning, LogLevel::Error}) {
		REQUIRE_NOTHROW(Log::SetLevel(level));
	}

	// Left where the rest of the suite expects it: a trace-level logger makes
	// every other test's output unreadable.
	Log::SetLevel(LogLevel::Info);
}

TEST_CASE("the macros compile and route to the logger", "[log]") {
	Log::Initialise("test");
	Log::SetLevel(LogLevel::Error);

	// Formatting happens inside spdlog, so a mismatched argument count is a
	// compile error here rather than a runtime surprise in a rare branch.
	REQUIRE_NOTHROW(ENGINE_TRACE("trace {}", 1));
	REQUIRE_NOTHROW(ENGINE_INFO("info {} {}", 1, "two"));
	REQUIRE_NOTHROW(ENGINE_WARN("warn {:.2f}", 1.5));
	REQUIRE_NOTHROW(ENGINE_ERROR("error"));

	Log::SetLevel(LogLevel::Info);
}
