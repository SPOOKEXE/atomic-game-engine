#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.core.metrics")
TEST_DEPENDS("engine.core.name")

using Catch::Approx;
using engine::core::Counter;
using engine::core::Metrics;
using engine::core::Name;
using engine::core::ScopedCount;

namespace {
	// The sink is process-wide, so each case starts from empty and uses a name
	// nothing else will.
	std::string Unique(const char *label) {
		static int counter = 0;
		return std::string("metric.") + label + "." + std::to_string(counter++);
	}

	const Counter *Find(const std::vector<Counter> &counters, const std::string &text) {
		const Name name(text);
		const auto found = std::find_if(counters.begin(), counters.end(), [name](const Counter &counter) {
			return counter.Name == name;
		});
		return found == counters.end() ? nullptr : &*found;
	}
}

TEST_CASE("a count accumulates and samples", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("accumulate");

	Metrics::Count(name, 2.0);
	Metrics::Count(name, 3.0);
	Metrics::Count(name, 5.0);

	const auto drained = Metrics::Drain();
	const Counter *counter = Find(drained, name);

	REQUIRE(counter != nullptr);
	REQUIRE(counter->Value == Approx(10.0));
	// Samples is what turns a total into a mean. Without it a caller cannot
	// tell one big frame from ten small ones.
	REQUIRE(counter->Samples == 3);
	REQUIRE_FALSE(counter->IsTime);
}

TEST_CASE("Drain empties the sink", "[metrics]") {
	Metrics::Clear();
	Metrics::Count(Unique("drained"), 1.0);

	REQUIRE_FALSE(Metrics::Drain().empty());
	// Exactly one reader per frame is what makes these a per-frame rate rather
	// than a number that only goes up.
	REQUIRE(Metrics::Drain().empty());
}

TEST_CASE("Clear discards without reading", "[metrics]") {
	Metrics::Clear();
	Metrics::Count(Unique("cleared"), 1.0);
	Metrics::Clear();

	REQUIRE(Metrics::Drain().empty());
}

TEST_CASE("a time is marked as one", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("timed");

	Metrics::CountTime(name, 1'500'000);

	const auto drained = Metrics::Drain();
	const Counter *counter = Find(drained, name);

	REQUIRE(counter != nullptr);
	// Kept apart from a plain count so that no reader has to guess a unit from
	// a name. The overlay divides these by a million and the others it does not.
	REQUIRE(counter->IsTime);
	REQUIRE(counter->Value == Approx(1'500'000.0));
}

TEST_CASE("one name is one counter", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("interned");

	for (int index = 0; index < 50; index++) {
		Metrics::Count(name, 1.0);
	}

	const auto drained = Metrics::Drain();
	REQUIRE(drained.size() == 1);
	REQUIRE(drained.front().Samples == 50);
}

TEST_CASE("different names are different counters", "[metrics]") {
	Metrics::Clear();
	const std::string first = Unique("a");
	const std::string second = Unique("b");

	Metrics::Count(first, 1.0);
	Metrics::Count(second, 2.0);

	const auto drained = Metrics::Drain();
	REQUIRE(drained.size() == 2);
	REQUIRE(Find(drained, first)->Value == Approx(1.0));
	REQUIRE(Find(drained, second)->Value == Approx(2.0));
}

TEST_CASE("the counter name survives the drain", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("readable");
	Metrics::Count(name, 1.0);

	// The overlay prints Text(). An id with no string behind it would render
	// as a blank row.
	const auto drained = Metrics::Drain();
	REQUIRE(drained.front().Name.Text() == name);
}

TEST_CASE("ScopedCount records elapsed time", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("scoped");

	{
		ScopedCount scope(name);
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}

	const auto drained = Metrics::Drain();
	const Counter *counter = Find(drained, name);

	REQUIRE(counter != nullptr);
	REQUIRE(counter->IsTime);
	REQUIRE(counter->Value >= 2'000'000.0);
}

TEST_CASE("counting from many threads loses nothing", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("contended");

	// L11 reports bytes-per-remote from whatever thread the transport is on,
	// so the sink has to take a write from anywhere.
	std::vector<std::thread> threads;
	for (int index = 0; index < 8; index++) {
		threads.emplace_back([&name] {
			for (int n = 0; n < 200; n++) {
				Metrics::Count(name, 1.0);
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}

	const auto drained = Metrics::Drain();
	REQUIRE(drained.size() == 1);
	REQUIRE(drained.front().Value == Approx(1600.0));
	REQUIRE(drained.front().Samples == 1600);
}

// Fresh names race through the interner while one consumer drains the sink.
// A joined aggregate proves conservation; retained copies and reused names prove
// that neither Drain nor Clear makes an interned name a stale counter handle.
TEST_CASE("fresh concurrent counters survive drains and name reuse", "[metrics]") {
	Metrics::Clear();
	constexpr size_t NAMES = 32;
	constexpr unsigned WORKERS = 4;
	constexpr size_t WRITES = 256;
	std::array<std::string, NAMES> counts;
	std::array<std::string, NAMES> times;
	for (size_t index = 0; index < NAMES; index++) {
		counts[index] = Unique("fresh.count");
		times[index] = Unique("fresh.time");
	}
	std::array<uint64_t, NAMES> countSamples{};
	std::array<uint64_t, NAMES> timeSamples{};
	std::array<double, NAMES> countValues{};
	std::array<double, NAMES> timeValues{};
	const auto accumulate = [&](const std::vector<Counter> &drained) {
		for (const Counter &counter : drained) {
			const std::string_view text = counter.Name.Text();
			bool found = false;
			for (size_t index = 0; index < NAMES; index++) {
				if (text == counts[index]) {
					CHECK_FALSE(counter.IsTime);
					countSamples[index] += counter.Samples;
					countValues[index] += counter.Value;
					found = true;
					break;
				}
				if (text == times[index]) {
					CHECK(counter.IsTime);
					timeSamples[index] += counter.Samples;
					timeValues[index] += counter.Value;
					found = true;
					break;
				}
			}
			CHECK(found);
		}
	};
	std::barrier midpoint(WORKERS + 1);
	std::barrier resume(WORKERS + 1);
	std::atomic<unsigned> completed{0};
	std::vector<std::thread> writers;
	for (unsigned worker = 0; worker < WORKERS; worker++) {
		writers.emplace_back([&] {
			for (size_t write = 0; write < WRITES; write++) {
				if (write == WRITES / 2) {
					midpoint.arrive_and_wait();
					resume.arrive_and_wait();
				}
				const size_t index = write % NAMES;
				Metrics::Count(counts[index], 1.0);
				Metrics::CountTime(times[index], index + 1);
			}
			completed.fetch_add(1, std::memory_order_release);
		});
	}
	midpoint.arrive_and_wait();
	const auto copied = Metrics::Snapshot();
	CHECK(copied.Counters.size() == 2 * NAMES);
	accumulate(Metrics::Drain());
	resume.arrive_and_wait();
	// Bound reader work independently of scheduling; the final joined drain
	// collects any writes remaining after this consumer's polling budget.
	for (unsigned poll = 0; poll < 4096 && completed.load(std::memory_order_acquire) < WORKERS; poll++) {
		accumulate(Metrics::Drain());
		std::this_thread::yield();
	}
	for (auto &writer : writers)
		writer.join();
	accumulate(Metrics::Drain());
	for (size_t index = 0; index < NAMES; index++) {
		CHECK(countSamples[index] == WORKERS * WRITES / NAMES);
		CHECK(timeSamples[index] == WORKERS * WRITES / NAMES);
		CHECK(countValues[index] == static_cast<double>(WORKERS * WRITES / NAMES));
		CHECK(timeValues[index] == static_cast<double>((WORKERS * WRITES / NAMES) * (index + 1)));
		const auto *countCopy = Find(copied.Counters, counts[index]);
		const auto *timeCopy = Find(copied.Counters, times[index]);
		REQUIRE(countCopy != nullptr);
		REQUIRE(timeCopy != nullptr);
		CHECK(countCopy->Samples == WORKERS * WRITES / (2 * NAMES));
		CHECK(countCopy->Value == static_cast<double>(countCopy->Samples));
		CHECK(timeCopy->Samples == WORKERS * WRITES / (2 * NAMES));
		CHECK(timeCopy->Value == static_cast<double>(timeCopy->Samples * (index + 1)));
		CHECK_FALSE(Metrics::Get(counts[index]).has_value());
		CHECK_FALSE(Metrics::Get(times[index]).has_value());
	}
	Metrics::Count(counts[0], 3.0);
	Metrics::CountTime(counts[0], 5);
	Metrics::CountTime(times[0], 7);
	Metrics::Count(times[0], 2.0);
	const auto reused = Metrics::Snapshot();
	const auto count = Metrics::Get(counts[0]);
	const auto time = Metrics::Get(times[0]);
	REQUIRE(count.has_value());
	REQUIRE(time.has_value());
	CHECK_FALSE(count->IsTime);
	CHECK(count->Value == 8.0);
	CHECK(count->Samples == 2);
	CHECK(time->IsTime);
	CHECK(time->Value == 9.0);
	CHECK(time->Samples == 2);
	Metrics::Clear();
	CHECK(Metrics::Snapshot().Counters.empty());
	Metrics::CountTime(counts[0], 11);
	Metrics::Count(times[0], 13.0);
	const auto afterClear = Metrics::Drain();
	const auto *newTime = Find(afterClear, counts[0]);
	const auto *newCount = Find(afterClear, times[0]);
	REQUIRE(newTime != nullptr);
	REQUIRE(newCount != nullptr);
	CHECK(newTime->IsTime);
	CHECK(newTime->Value == 11.0);
	CHECK_FALSE(newCount->IsTime);
	CHECK(newCount->Value == 13.0);
	const auto *oldCount = Find(reused.Counters, counts[0]);
	const auto *oldTime = Find(reused.Counters, times[0]);
	REQUIRE(oldCount != nullptr);
	REQUIRE(oldTime != nullptr);
	CHECK_FALSE(oldCount->IsTime);
	CHECK(oldCount->Value == 8.0);
	CHECK(oldTime->IsTime);
	CHECK(oldTime->Value == 9.0);
}

TEST_CASE("a counter reads back without being drained", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("read");

	Metrics::Count(name, 2.0);
	Metrics::Count(name, 3.0);

	// **To report, never to decide.** Reading must leave the counter exactly
	// where it was, or a reporter and the one drainer per frame would be
	// fighting over each frame's numbers.
	const auto read = Metrics::Get(name);
	REQUIRE(read.has_value());
	CHECK(read->Value == Approx(5.0));
	CHECK(read->Samples == 2);

	const auto again = Metrics::Get(name);
	REQUIRE(again.has_value());
	CHECK(again->Value == Approx(5.0));

	CHECK_FALSE(Metrics::Get("metric.nothing.named.this").has_value());

	const auto drained = Metrics::Drain();
	REQUIRE(Find(drained, name) != nullptr);
	CHECK_FALSE(Metrics::Get(name).has_value());
}

TEST_CASE("a gauge replaces rather than accumulates", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("gauge");

	Metrics::SetGauge(name, 4.0);
	Metrics::SetGauge(name, 7.0);

	const auto read = Metrics::GetGauge(name);
	REQUIRE(read.has_value());
	CHECK(read->Value == Approx(7.0));

	// The write count is the only way to tell a gauge somebody is keeping
	// current from one nothing has touched since startup.
	CHECK(read->Writes == 2);

	// **Not drained**, because "how many clients are connected" has no
	// per-frame meaning and a drain would answer zero for every frame nobody
	// happened to write it in.
	const auto drained = Metrics::Drain();
	CHECK(drained.empty());
	REQUIRE(Metrics::GetGauge(name).has_value());
	CHECK(Metrics::GetGauge(name)->Value == Approx(7.0));
}

TEST_CASE("a histogram keeps the shape and not only the total", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("histogram");

	for (int value = 1; value <= 100; value++) {
		Metrics::Observe(name, static_cast<double>(value));
	}

	const auto read = Metrics::GetHistogram(name);
	REQUIRE(read.has_value());
	CHECK(read->Samples == 100);
	CHECK(read->Sum == Approx(5050.0));
	CHECK(read->Minimum == Approx(1.0));
	CHECK(read->Maximum == Approx(100.0));
	CHECK(read->Mean == Approx(50.5));
	CHECK(read->Retained == 100);

	// Nearest-rank, so every percentile is a reading that actually happened.
	// An interpolated p99 over 1..100 would answer 99.01, which is a value
	// nothing observed.
	//
	// p50 is 51 rather than 50 because the rank is rounded rather than floored,
	// which puts an exact half on the upper reading. That is `FrameGraph`'s
	// convention and `Server::Run`'s, and it is pinned here so that a change to
	// any one of the three is a change that fails a test rather than one that
	// makes two reports disagree by one reading.
	CHECK(read->P50 == Approx(51.0));
	CHECK(read->P95 == Approx(95.0));
	CHECK(read->P99 == Approx(99.0));
}

TEST_CASE("a histogram's window is bounded", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("window");

	// Twice the window, all of the second half larger than all of the first.
	// The percentiles must describe the recent readings, and the exact totals
	// must still describe every one of them.
	const uint32_t window = Metrics::RETAINED_OBSERVATIONS;
	for (uint32_t index = 0; index < window; index++) {
		Metrics::Observe(name, 1.0);
	}
	for (uint32_t index = 0; index < window; index++) {
		Metrics::Observe(name, 9.0);
	}

	const auto read = Metrics::GetHistogram(name);
	REQUIRE(read.has_value());
	CHECK(read->Samples == 2 * window);
	CHECK(read->Retained == window);
	CHECK(read->Minimum == Approx(1.0));
	CHECK(read->Maximum == Approx(9.0));
	CHECK(read->P50 == Approx(9.0));
}

TEST_CASE("ScopedObservation records elapsed time", "[metrics]") {
	Metrics::Clear();
	const std::string name = Unique("observed");

	{
		const engine::core::ScopedObservation scope(name);
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}

	const auto read = Metrics::GetHistogram(name);
	REQUIRE(read.has_value());
	CHECK(read->IsTime);
	CHECK(read->Samples == 1);
	CHECK(read->Maximum >= 2'000'000.0);
}

TEST_CASE("a snapshot carries all three kinds and resets nothing", "[metrics]") {
	Metrics::Clear();
	const std::string counted = Unique("snapshot.counter");
	const std::string level = Unique("snapshot.gauge");
	const std::string shape = Unique("snapshot.histogram");

	Metrics::Count(counted, 3.0);
	Metrics::SetGauge(level, 11.0);
	Metrics::Observe(shape, 5.0);

	const engine::core::MetricsSnapshot taken = Metrics::Snapshot();
	REQUIRE(taken.Counters.size() == 1);
	REQUIRE(taken.Gauges.size() == 1);
	REQUIRE(taken.Histograms.size() == 1);
	CHECK(taken.Counters.front().Value == Approx(3.0));
	CHECK(taken.Gauges.front().Value == Approx(11.0));
	CHECK(taken.Histograms.front().P50 == Approx(5.0));

	// Twice, identically. A report that emptied the sink would be a report
	// nobody could take beside a drain.
	const engine::core::MetricsSnapshot again = Metrics::Snapshot();
	REQUIRE(again.Counters.size() == 1);
	CHECK(again.Counters.front().Value == Approx(3.0));

	Metrics::Clear();
	CHECK(Metrics::Snapshot().Counters.empty());
	CHECK(Metrics::Snapshot().Gauges.empty());
	CHECK(Metrics::Snapshot().Histograms.empty());
}

TEST_CASE("a snapshot is sorted by name", "[metrics]") {
	Metrics::Clear();

	Metrics::Count("metric.sorted.zulu", 1.0);
	Metrics::Count("metric.sorted.alpha", 1.0);
	Metrics::Count("metric.sorted.mike", 1.0);

	// First-seen order is what the interned ids carry and is not an order
	// anybody reading a report expects.
	const engine::core::MetricsSnapshot taken = Metrics::Snapshot();
	REQUIRE(taken.Counters.size() == 3);
	CHECK(taken.Counters[0].Name.Text() == "metric.sorted.alpha");
	CHECK(taken.Counters[1].Name.Text() == "metric.sorted.mike");
	CHECK(taken.Counters[2].Name.Text() == "metric.sorted.zulu");

	Metrics::Clear();
}

TEST_CASE("histogram reports preserve mixed retained lengths and copied percentiles", "[metrics]") {
	Metrics::Clear();
	const std::string full = Unique("z.full");
	const std::string partial = Unique("a.partial");
	const std::string single = Unique("m.time");
	for (uint32_t value = 0; value < Metrics::RETAINED_OBSERVATIONS; ++value)
		Metrics::Observe(full, static_cast<double>(value));
	for (const double value : std::array{-3.0, 9.0, 1.0, 9.0, 1.0})
		Metrics::Observe(partial, value);
	Metrics::ObserveTime(single, 42);

	// Insertion order renders the full window before the short one; canonical
	// report ordering then places the shorter distribution first.
	const auto saved = Metrics::Snapshot();
	REQUIRE(saved.Histograms.size() == 3);
	CHECK(saved.Histograms[0].Name.Text() == partial);
	CHECK(saved.Histograms[1].Name.Text() == single);
	CHECK(saved.Histograms[2].Name.Text() == full);
	const auto &shortShape = saved.Histograms[0];
	CHECK(shortShape.Samples == 5);
	CHECK(shortShape.Retained == 5);
	CHECK(shortShape.Sum == 17.0);
	CHECK(shortShape.Minimum == -3.0);
	CHECK(shortShape.Maximum == 9.0);
	CHECK(shortShape.Mean == Approx(3.4));
	CHECK(shortShape.P50 == 1.0);
	CHECK(shortShape.P95 == 9.0);
	CHECK(shortShape.P99 == 9.0);
	CHECK_FALSE(shortShape.IsTime);
	const auto &timeShape = saved.Histograms[1];
	CHECK(timeShape.Samples == 1);
	CHECK(timeShape.Retained == 1);
	CHECK(timeShape.Sum == 42.0);
	CHECK(timeShape.Minimum == 42.0);
	CHECK(timeShape.Maximum == 42.0);
	CHECK(timeShape.Mean == 42.0);
	CHECK(timeShape.P50 == 42.0);
	CHECK(timeShape.P95 == 42.0);
	CHECK(timeShape.P99 == 42.0);
	CHECK(timeShape.IsTime);
	const auto &longShape = saved.Histograms[2];
	CHECK(longShape.Samples == 1024);
	CHECK(longShape.Retained == 1024);
	CHECK(longShape.Sum == 523776.0);
	CHECK(longShape.Minimum == 0.0);
	CHECK(longShape.Maximum == 1023.0);
	CHECK(longShape.Mean == 511.5);
	CHECK(longShape.P50 == 512.0);
	CHECK(longShape.P95 == 972.0);
	CHECK(longShape.P99 == 1013.0);
	CHECK_FALSE(longShape.IsTime);

	Metrics::Observe(partial, 100.0);
	const auto changed = Metrics::GetHistogram(partial);
	REQUIRE(changed.has_value());
	CHECK(changed->Samples == 6);
	CHECK(changed->Sum == 117.0);
	CHECK(changed->P50 == 9.0);
	CHECK(changed->P95 == 100.0);
	CHECK(changed->P99 == 100.0);
	Metrics::Clear();
	CHECK(shortShape.Samples == 5);
	CHECK(shortShape.Sum == 17.0);
	CHECK(shortShape.P50 == 1.0);
	CHECK(shortShape.P95 == 9.0);
	CHECK(shortShape.P99 == 9.0);
	CHECK(Metrics::Snapshot().Histograms.empty());
}

TEST_CASE("indexed counter rows retain registration order across reversed ids and growth", "[metrics]") {
	Metrics::Clear();
	constexpr size_t ROWS = 300;
	std::vector<std::string> names;
	names.reserve(ROWS);
	for (size_t index = 0; index < ROWS; index++)
		names.push_back(Unique("indexed.row"));

	std::vector<uint32_t> ids(ROWS);
	for (size_t index = ROWS; index > 0; index--)
		ids[index - 1] = Name(names[index - 1]).Id();
	for (size_t index = 1; index < ROWS; index++)
		REQUIRE(ids[index - 1] > ids[index]);

	const std::string sparseName = Unique("indexed.sparse");
	const Name sparseId = Name::Reserve(sparseName, 500'300);
	REQUIRE(sparseId.IsValid());
	CHECK(sparseId.Id() == 500'300);

	for (size_t index = 0; index < ROWS; index++)
		Metrics::Count(names[index], static_cast<double>(index + 1));
	Metrics::Count(sparseName, 901.0);
	for (size_t index = 0; index < ROWS; index++) {
		const auto counter = Metrics::Get(names[index]);
		REQUIRE(counter.has_value());
		CHECK(counter->Value == Approx(static_cast<double>(index + 1)));
		CHECK(counter->Samples == 1);
	}
	const auto sparseCounter = Metrics::Get(sparseName);
	REQUIRE(sparseCounter.has_value());
	CHECK(sparseCounter->Name.Id() == 500'300);
	CHECK(sparseCounter->Value == Approx(901.0));

	const auto drained = Metrics::Drain();
	REQUIRE(drained.size() == ROWS + 1);
	for (size_t index = 0; index < ROWS; index++) {
		CHECK(drained[index].Name.Text() == names[index]);
		CHECK(drained[index].Value == Approx(static_cast<double>(index + 1)));
	}
	CHECK(drained[ROWS].Name.Text() == sparseName);
	CHECK(drained[ROWS].Name.Id() == 500'300);
	CHECK(drained[ROWS].Value == Approx(901.0));
	Metrics::Clear();
}

TEST_CASE("metric kinds have independent rows and missing reads do not insert", "[metrics]") {
	Metrics::Clear();
	const std::string missing = Unique("missing.kind");
	CHECK_FALSE(Metrics::Get(missing).has_value());
	CHECK_FALSE(Metrics::GetGauge(missing).has_value());
	CHECK_FALSE(Metrics::GetHistogram(missing).has_value());
	CHECK_FALSE(Metrics::Get("").has_value());
	CHECK_FALSE(Metrics::GetGauge("").has_value());
	CHECK_FALSE(Metrics::GetHistogram("").has_value());
	const auto empty = Metrics::Snapshot();
	CHECK(empty.Counters.empty());
	CHECK(empty.Gauges.empty());
	CHECK(empty.Histograms.empty());

	const std::string shared = Unique("shared.kind");
	Metrics::Count(shared, 2.0);
	Metrics::SetGauge(shared, 7.0);
	Metrics::Observe(shared, 11.0);
	Metrics::Count("", 5.0);
	Metrics::SetGauge("", 6.0);
	Metrics::Observe("", 7.0);

	const auto emptyCounter = Metrics::Get("");
	const auto emptyGauge = Metrics::GetGauge("");
	const auto emptyHistogram = Metrics::GetHistogram("");
	REQUIRE(emptyCounter.has_value());
	REQUIRE(emptyGauge.has_value());
	REQUIRE(emptyHistogram.has_value());
	CHECK_FALSE(emptyCounter->Name.IsValid());
	CHECK_FALSE(emptyGauge->Name.IsValid());
	CHECK_FALSE(emptyHistogram->Name.IsValid());
	CHECK(emptyCounter->Value == Approx(5.0));
	CHECK(emptyGauge->Value == Approx(6.0));
	CHECK(emptyHistogram->Sum == Approx(7.0));

	const auto counter = Metrics::Get(shared);
	const auto gauge = Metrics::GetGauge(shared);
	const auto histogram = Metrics::GetHistogram(shared);
	REQUIRE(counter.has_value());
	REQUIRE(gauge.has_value());
	REQUIRE(histogram.has_value());
	CHECK(counter->Name.Text() == shared);
	CHECK(counter->Value == Approx(2.0));
	CHECK(gauge->Name.Text() == shared);
	CHECK(gauge->Value == Approx(7.0));
	CHECK(histogram->Name.Text() == shared);
	CHECK(histogram->Samples == 1);
	CHECK(histogram->Sum == Approx(11.0));
	const auto populated = Metrics::Snapshot();
	CHECK(populated.Counters.size() == 2);
	CHECK(populated.Gauges.size() == 2);
	CHECK(populated.Histograms.size() == 2);
	Metrics::Clear();
}

TEST_CASE("first writer sets time kind and drain and clear reset indexed rows", "[metrics]") {
	Metrics::Clear();
	const std::string countFirst = Unique("count.first");
	const std::string timeFirst = Unique("time.first");
	const std::string histogramFirst = Unique("histogram.first");
	const std::string timedHistogramFirst = Unique("histogram.time.first");
	const std::string gaugeName = Unique("gauge.reset");

	Metrics::Count(countFirst, 1.5);
	Metrics::CountTime(countFirst, 4);
	Metrics::CountTime(timeFirst, 7);
	Metrics::Count(timeFirst, 2.0);
	Metrics::Observe(histogramFirst, 3.0);
	Metrics::ObserveTime(histogramFirst, 5);
	Metrics::ObserveTime(timedHistogramFirst, 7);
	Metrics::Observe(timedHistogramFirst, 2.0);
	Metrics::SetGauge(gaugeName, 3.0);

	const auto plainCounter = Metrics::Get(countFirst);
	const auto timedCounter = Metrics::Get(timeFirst);
	const auto plainHistogram = Metrics::GetHistogram(histogramFirst);
	const auto timedHistogram = Metrics::GetHistogram(timedHistogramFirst);
	REQUIRE(plainCounter.has_value());
	REQUIRE(timedCounter.has_value());
	REQUIRE(plainHistogram.has_value());
	REQUIRE(timedHistogram.has_value());
	CHECK_FALSE(plainCounter->IsTime);
	CHECK(plainCounter->Value == Approx(5.5));
	CHECK(plainCounter->Samples == 2);
	CHECK(timedCounter->IsTime);
	CHECK(timedCounter->Value == Approx(9.0));
	CHECK(timedCounter->Samples == 2);
	CHECK_FALSE(plainHistogram->IsTime);
	CHECK(plainHistogram->Sum == Approx(8.0));
	CHECK(plainHistogram->Samples == 2);
	CHECK(timedHistogram->IsTime);
	CHECK(timedHistogram->Sum == Approx(9.0));
	CHECK(timedHistogram->Samples == 2);

	const auto drained = Metrics::Drain();
	REQUIRE(drained.size() == 2);
	CHECK_FALSE(Metrics::Get(countFirst).has_value());
	Metrics::CountTime(countFirst, 11);
	const auto refilled = Metrics::Get(countFirst);
	REQUIRE(refilled.has_value());
	CHECK(refilled->IsTime);
	CHECK(refilled->Value == Approx(11.0));
	CHECK(refilled->Samples == 1);

	REQUIRE(Metrics::GetGauge(gaugeName).has_value());
	Metrics::Clear();
	CHECK_FALSE(Metrics::GetGauge(gaugeName).has_value());
	CHECK_FALSE(Metrics::Get(countFirst).has_value());
	CHECK_FALSE(Metrics::GetHistogram(timedHistogramFirst).has_value());
	const auto cleared = Metrics::Snapshot();
	CHECK(cleared.Counters.empty());
	CHECK(cleared.Gauges.empty());
	CHECK(cleared.Histograms.empty());
	Metrics::Count(countFirst, 13.0);
	Metrics::Observe(timedHistogramFirst, 17.0);
	Metrics::SetGauge(gaugeName, 19.0);
	const auto afterClearCounter = Metrics::Get(countFirst);
	const auto afterClearHistogram = Metrics::GetHistogram(timedHistogramFirst);
	const auto afterClearGauge = Metrics::GetGauge(gaugeName);
	REQUIRE(afterClearCounter.has_value());
	REQUIRE(afterClearHistogram.has_value());
	REQUIRE(afterClearGauge.has_value());
	CHECK_FALSE(afterClearCounter->IsTime);
	CHECK(afterClearCounter->Value == Approx(13.0));
	CHECK_FALSE(afterClearHistogram->IsTime);
	CHECK(afterClearHistogram->Sum == Approx(17.0));
	CHECK(afterClearGauge->Value == Approx(19.0));
	CHECK(afterClearGauge->Writes == 1);
	Metrics::Clear();
}
