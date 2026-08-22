#pragma once

// The metrics sink.
//
// This is a seam. It exists so that `net` at L11 can report bytes-per-remote to
// the userland profiler at L13 without `net` depending on `script`. Writers
// name a counter; the sink does not know or care who reads.
//
// ## It has a read side now, and what that changed
//
// Until v0.19 there was no `Get`, no gauge, no histogram and one reader:
// `Drain`, which empties the sink. `core/AGENTS.md` said not to add a `Get`,
// with a good argument - the moment one subsystem *steers on* another
// subsystem's counter, the sink is a global variable with extra steps.
//
// **That argument is about control flow, and it is still the rule.** What it
// also prevented was *reporting*, which is not the same thing and was the
// actual gap: the headless server had counted things since v0.9 and never once
// read them, `core::FrameGraph` dropped every span opened on a job worker and
// counted the drops into a number nothing outside the F5 overlay could reach,
// and there were four counter mechanisms in the tree with four ways to read
// them. So the rule is now the one it always meant:
//
//     Read to report. Never read to decide.
//
// A caller that branches on `Get` is doing the thing `AGENTS.md` refuses. A
// caller that prints, draws, folds or exports is what the read side is for, and
// `Snapshot` is deliberately shaped for that: it takes no lock beyond the one
// call, resets nothing, and hands back the whole sink at once.
//
// ## Three kinds, because they answer different questions
//
// - **A counter** accumulates and is drained. Its value is a rate: "bytes since
//   the last frame". `Drain` is what makes it one - exactly one reader per
//   frame, which is the property that keeps the number from being a total that
//   only goes up.
// - **A gauge** is a level, most recently set. `SetGauge` replaces rather than
//   adds, and nothing drains it, because "how many clients are connected" has
//   no per-frame meaning.
// - **A histogram** is a shape. A mean and a maximum are two numbers and the
//   distribution is a third thing: a server holding its tick rate on average
//   while a fiftieth of its ticks miss the budget is what a player feels, and
//   only a percentile says so. Percentiles are nearest-rank over the retained
//   window, for `FrameGraph`'s reason - an interpolated percentile reports a
//   reading that never happened.
//
// @tier L0 · shared

#include <engine/core/Name.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::core {

	// One named total drained from the process-wide metrics sink.
	struct Counter {
		// A name, interned once. Two counters are the same counter when their
		// ids match, which is an integer compare rather than a string one -
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

	// One named level, as last set.
	struct Gauge {
		// A name, interned once, exactly as a counter's is.
		core::Name Name;

		// The value most recently written. Not a sum: a gauge that added would
		// be a counter with a misleading name.
		double Value = 0.0;

		// How many times it has been written, which is the only way to tell a
		// gauge that is being kept current from one nothing has touched since
		// startup.
		uint64_t Writes = 0;
	};

	// One named distribution, and the percentiles over its retained window.
	struct Histogram {
		// A name, interned once, exactly as a counter's is.
		core::Name Name;

		// Every observation ever recorded under `Name`.
		uint64_t Samples = 0;

		// Exact over every observation, not only the retained ones.
		//@{
		double Sum = 0.0;
		double Minimum = 0.0;
		double Maximum = 0.0;
		double Mean = 0.0;
		//@}

		// Nearest-rank over the **retained window**, which is the most recent
		// `Metrics::RETAINED_OBSERVATIONS` observations rather than all of
		// them. A window rather than everything for `FrameGraph`'s reason: an
		// unbounded history is unbounded memory, and the interesting shape is
		// the recent one.
		//@{
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
		//@}

		// How many observations the percentiles above were taken over.
		uint32_t Retained = 0;

		// Times are observed in nanoseconds and displayed as milliseconds, for
		// `Counter::IsTime`'s reason.
		bool IsTime = false;
	};

	// Everything in the sink, taken at one instant and resetting nothing.
	struct MetricsSnapshot {
		// Accumulating totals. Unlike `Drain`, reading these leaves them.
		std::vector<Counter> Counters;

		// Levels, as last set.
		std::vector<Gauge> Gauges;

		// Distributions, with percentiles already taken.
		std::vector<Histogram> Histograms;
	};

	// A thread-safe sink for per-frame named totals, levels and distributions.
	//
	// Use each name consistently with one of Count(), CountTime(), SetGauge()
	// and Observe(); the first call for a name decides what kind it is and
	// whether it is marked as time.
	//
	// @threadsafe
	class Metrics {
	  public:
		// How many observations a histogram keeps for its percentiles.
		//
		// **Bounded, and the bound is what makes the memory a number somebody
		// chose.** A thousand readings is a fixed 8 KiB per histogram and is
		// deep enough for a p99 to mean something; keeping every observation
		// would make the sink's footprint a function of how long the process
		// has been up.
		static constexpr uint32_t RETAINED_OBSERVATIONS = 1024;

		// Adds `amount` and one sample to the counter named `name`.
		static void Count(std::string_view name, double amount);

		// Adds `nanoseconds` and one sample to a time counter named `name`.
		static void CountTime(std::string_view name, uint64_t nanoseconds);

		// Sets the gauge named `name` to `value`, replacing what was there.
		//
		// @param name  The gauge.
		// @param value Its new level.
		static void SetGauge(std::string_view name, double value);

		// Records one observation into the histogram named `name`.
		//
		// @param name  The histogram.
		// @param value The observation, in whatever unit the name implies.
		static void Observe(std::string_view name, double value);

		// Records one duration into a time histogram named `name`.
		//
		// @param name        The histogram.
		// @param nanoseconds The duration.
		static void ObserveTime(std::string_view name, uint64_t nanoseconds);

		// One counter, without draining it or anything else.
		//
		// **To report, never to decide.** See the file comment: a caller that
		// branches on this has turned the sink into a global variable.
		//
		// @param name The counter.
		// @return Its accumulated value, or nothing if no such counter exists.
		static std::optional<Counter> Get(std::string_view name);

		// One gauge, as last set.
		//
		// @param name The gauge.
		// @return Its level, or nothing if no such gauge exists.
		static std::optional<Gauge> GetGauge(std::string_view name);

		// One histogram, with its percentiles taken at the moment of the call.
		//
		// @param name The histogram.
		// @return Its shape, or nothing if no such histogram exists.
		static std::optional<Histogram> GetHistogram(std::string_view name);

		// Takes every counter and resets them. Exactly one reader per frame,
		// which is the property that makes the values a per-frame rate rather
		// than a number that only goes up.
		//
		// **Counters only.** A gauge is a level and has nothing to drain; a
		// histogram's window is what its percentiles are over, and emptying it
		// every frame would leave a p99 taken over one frame's worth of
		// readings.
		static std::vector<Counter> Drain();

		// Everything in the sink, resetting nothing.
		//
		// The shape a periodic report and an exporter both want: a drain would
		// make the two of them fight over who gets each frame's counters.
		//
		// @return The counters, gauges and histograms, each sorted by name.
		static MetricsSnapshot Snapshot();

		// Discards every accumulated counter, gauge and histogram.
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

	// Times a block into a named histogram, so its shape is kept and not only
	// its total.
	//
	// **The one to reach for on anything paced.** A tick, a frame, a request:
	// what matters is not the mean but how often it was much worse than the
	// mean, and a `ScopedCount` cannot answer that. Costs one more branch than
	// `ScopedCount` and the same two clock reads.
	class ScopedObservation {
	  public:
		// Starts timing a histogram name that must remain alive through destruction.
		explicit ScopedObservation(std::string_view name);

		// Records the elapsed nanoseconds into the named time histogram.
		~ScopedObservation();

		// Scoped timers cannot be copied because each instance records once.
		ScopedObservation(const ScopedObservation &) = delete;

		// Scoped timers cannot be copy-assigned because each instance records once.
		ScopedObservation &operator=(const ScopedObservation &) = delete;

	  private:
		std::string_view HistogramName;
		uint64_t StartNanoseconds;
	};
}
