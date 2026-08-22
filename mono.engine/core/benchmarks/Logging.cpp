// What a log statement costs when nobody is listening.
//
// **The number that matters is the first one**, because it is the one paid by
// the 711 call sites in the tree on every frame they are not switched on. A
// statement that is off has to cost a branch and nothing else, or the engine
// pays for diagnostics it is not producing - and until v0.19 it did: the macros
// evaluated their arguments whether the level was on or not, so a disabled
// `ENGINE_TRACE("{} {}", store.Size(), name.Text())` still called both.
//
// Four rows, and they are meant to be read as ratios rather than as absolute
// times:
//
// - **compiled out** - below `ENGINE_LOG_COMPILED_LEVEL`, so the preprocessor
//   removed it. This should be indistinguishable from the empty loop, and if it
//   is not, the `sizeof` in `ENGINE_LOG_OMITTED` is evaluating something.
// - **disabled** - compiled in, switched off. A guard-variable check, a relaxed
//   byte load and a compare.
// - **enabled** - formatted and handed to a sink. Into a null sink rather than
//   to the terminal, because a benchmark that writes to a pipe measures the
//   pipe: what is wanted here is the cost of the facility, and what a terminal
//   costs on top is the same for every logging library ever written.
// - **throttled while quiet** - the disabled row plus one clock read and a
//   compare, which is the price of not printing sixty lines a second.
//
// Measured at `preset=bench` (release flags, `-O3`, compiled floor `debug`),
// 2026-08-22, seven samples per row:
//
//     ENGINE_TRACE · compiled out                    0 ns/call
//     ENGINE_INFO · disabled                         0 ns/call
//     ENGINE_INFO · disabled, with an argument       0 ns/call
//     LogCategory::Enabled                           0 ns/call
//     ENGINE_INFO · enabled, into a null sink       84 ns/call
//     ENGINE_INFO_EVERY · throttled while quiet     20 ns/call
//
// **The zeroes are the result, not a broken measurement.** The report is in
// whole nanoseconds, so a row under one reads as zero - and these are under
// one because the guard is a relaxed byte load the compiler is free to hoist
// out of a loop, after which the loop body is empty and goes with it.
//
// The row that proves the change landed is the third one. Its argument reads a
// `volatile`, which the optimiser may not remove if it runs - so if the guard
// were not stopping argument evaluation, that row would separate from the
// second. It does not.
//
// Against those, an enabled statement into a *null* sink is 84 ns: formatting
// two arguments, the category prefix, the file and line, and the sink's own
// lock. A throttled statement while it is quiet is 20 ns, which is one
// `Clock::Nanoseconds` (16 ns on this machine) and a compare - the price of
// not printing sixty lines a second.

#include <engine/core/Log.hpp>
#include <engine/testing/Bench.hpp>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.core.bench.logging")

using engine::core::Log;
using engine::core::LogCategory;
using engine::core::LogLevel;
using engine::testing::Consume;

namespace logging_bench {

	// Statements per row. Small enough that the enabled row does not take a
	// second on its own, large enough that the disabled rows are not measuring
	// the loop's own overhead.
	constexpr size_t STATEMENTS = 200'000;

	// Swaps the process logger's sinks for a null one, and puts them back.
	//
	// **Because a benchmark that writes to a terminal measures the terminal.**
	// The enabled row is here to say what formatting and dispatch cost, and
	// those are the parts this module is responsible for.
	struct NullSink {
		NullSink() {
			Log::Initialise("bench");
			Previous = Log::Logger().sinks();
			Log::Logger().sinks() = {std::make_shared<spdlog::sinks::null_sink_mt>()};
		}
		~NullSink() {
			Log::Logger().sinks() = Previous;
		}
		std::vector<spdlog::sink_ptr> Previous;
	};

	// A value a log argument can read, so that "evaluates its arguments" is a
	// measurable thing rather than a constant the optimiser folds away.
	//
	// Volatile on purpose: an argument the compiler can prove is a literal is
	// an argument whose evaluation costs nothing whether it happens or not,
	// which would make the disabled-with-argument row meaningless.
	volatile int Reading = 41;

	int Expensive() {
		return Reading + 1;
	}
}

using namespace logging_bench;

// --- statements nobody is listening to ----------------------------------------

BENCH("ENGINE_TRACE · compiled out", STATEMENTS) {
	// Under the `bench` preset's `debug` floor this is removed by the
	// preprocessor, so the row is the empty loop. Under a `trace` floor it is
	// the disabled row instead, which is why the two are next to each other.
	Log::SetLevel(LogLevel::Error);
	for (size_t index = 0; index < STATEMENTS; index++) {
		ENGINE_TRACE("bench: never {}", Expensive());
	}
}

BENCH("ENGINE_INFO · disabled", STATEMENTS) {
	Log::SetLevel(LogLevel::Error);
	for (size_t index = 0; index < STATEMENTS; index++) {
		ENGINE_INFO("bench: never");
	}
}

BENCH("ENGINE_INFO · disabled, with an argument", STATEMENTS) {
	// Against the row above: if there is a gap, the guard is not stopping
	// argument evaluation and the whole change did not land.
	Log::SetLevel(LogLevel::Error);
	for (size_t index = 0; index < STATEMENTS; index++) {
		ENGINE_INFO("bench: never {}", Expensive());
	}
}

BENCH("LogCategory::Enabled", STATEMENTS) {
	// The guard on its own, with no statement around it. Everything above is
	// this plus whatever the macro adds.
	const LogCategory category("bench");
	Log::SetLevel("bench", LogLevel::Error);

	int passed = 0;
	for (size_t index = 0; index < STATEMENTS; index++) {
		passed += category.Enabled(LogLevel::Info) ? 1 : 0;
	}
	Consume(passed);
}

// --- statements somebody is listening to --------------------------------------

BENCH("ENGINE_INFO · enabled, into a null sink", STATEMENTS) {
	const NullSink quiet;
	Log::SetLevel(LogLevel::Info);

	for (size_t index = 0; index < STATEMENTS; index++) {
		ENGINE_INFO("bench: a line with two arguments, {} and {}", Expensive(), index);
	}

	Log::SetLevel(LogLevel::Info);
}

BENCH("ENGINE_INFO_EVERY · throttled while quiet", STATEMENTS) {
	const NullSink quiet;
	Log::SetLevel(LogLevel::Info);

	// One line written, and STATEMENTS - 1 refused. The refusal is the cost
	// worth knowing, because that is what a per-frame fault pays.
	for (size_t index = 0; index < STATEMENTS; index++) {
		ENGINE_INFO_EVERY(3600.0, "bench: a throttled line {}", Expensive());
	}
}
