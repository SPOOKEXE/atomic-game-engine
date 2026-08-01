#pragma once

// The metrics sink.
//
// This is a seam, not a convenience. It exists so that `net` at L11 can report
// bytes-per-remote to the userland profiler at L13 without `net` depending on
// `script`. Writers name a counter; the sink does not know or care who reads.
//
// There is deliberately no Get(name). See core/AGENTS.md.
//
// @tier L0 · shared

#include <engine/core/Name.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::core {

	// One named total drained from the process-wide metrics sink.
	struct Counter {
		// A name, interned once. Two counters are the same counter when their
		// ids match, which is an integer compare rather than a string one —
		// see core/Name.hpp for why the string is still the thing that would
		// be serialized.
		core::Name Name;
		// The sum of amounts or nanoseconds recorded under `Name`.
		double Value = 0.0;
		// The number of calls contributing to `Value`.
		uint32_t Samples = 0;
		// Times are accumulated in nanoseconds and displayed as milliseconds.
		// Kept apart from plain counts so a reader never has to guess a unit
		// from a name.
		bool IsTime = false;
	};

	// A thread-safe, write-only sink for per-frame named totals.
	//
	// Use each name consistently with either Count() or CountTime(); the first
	// call for a name decides whether its Counter is marked as time.
	//
	// @threadsafe
	class Metrics {
	  public:
		// Adds `amount` and one sample to the counter named `name`.
		static void Count(std::string_view name, double amount);

		// Adds `nanoseconds` and one sample to a time counter named `name`.
		static void CountTime(std::string_view name, uint64_t nanoseconds);

		// Takes everything accumulated and resets. Exactly one reader per
		// frame, which is the property that makes the values a per-frame rate
		// rather than a number that only goes up.
		static std::vector<Counter> Drain();

		// Discards every accumulated counter without returning them.
		static void Clear();
	};

	// Times a block into a named counter. Cheap enough to leave in a hot path;
	// it is one clock read at each end and an accumulate.
	class ScopedCount {
	  public:
		// Starts timing a counter name that must remain alive through destruction.
		explicit ScopedCount(std::string_view name);

		// Adds the elapsed nanoseconds and one sample to the named time counter.
		~ScopedCount();

		// Scoped timers cannot be copied because each instance records once.
		ScopedCount(const ScopedCount &) = delete;

		// Scoped timers cannot be copy-assigned because each instance records once.
		ScopedCount &operator=(const ScopedCount &) = delete;

	  private:
		std::string_view CounterName;
		uint64_t StartNanoseconds;
	};
}
