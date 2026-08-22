#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
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
