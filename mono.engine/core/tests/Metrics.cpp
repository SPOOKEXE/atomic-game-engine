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
		const auto found = std::find_if(counters.begin(), counters.end(),
			[name](const Counter &counter) { return counter.Name == name; });
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
