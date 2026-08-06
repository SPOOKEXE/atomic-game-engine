#include <engine/replication/Rewind.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.replication.rewind")

using Catch::Approx;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::replication::Rewind;
using engine::replication::RewindSettings;

namespace {
	Entity At(uint32_t id) {
		Entity entity;
		entity.Id = id;
		return entity;
	}

	Rewind Walked(uint64_t ticks, size_t history = 32) {
		RewindSettings settings;
		settings.HistoryTicks = history;

		Rewind rewind(settings);
		for (uint64_t tick = 1; tick <= ticks; tick++) {
			REQUIRE(rewind.Begin(tick));
			REQUIRE(rewind.Record(At(1), Vector3{static_cast<float>(tick), 0.0f, 0.0f}));
		}
		return rewind;
	}
}

TEST_CASE("a placement comes back at the tick it was recorded", "[replication][rewind]") {
	const Rewind rewind = Walked(5);

	Vector3 at;
	REQUIRE(rewind.Sample(3.0, At(1), at));
	CHECK(at.X == Approx(3.0f));

	REQUIRE(rewind.Sample(1.0, At(1), at));
	CHECK(at.X == Approx(1.0f));
}

TEST_CASE("a fractional tick interpolates, because a client's view is fractional", "[replication][rewind]") {
	const Rewind rewind = Walked(5);

	Vector3 at;
	REQUIRE(rewind.Sample(3.5, At(1), at));
	CHECK(at.X == Approx(3.5f));

	REQUIRE(rewind.Sample(2.25, At(1), at));
	CHECK(at.X == Approx(2.25f));
}

TEST_CASE("a tick past what is held is refused", "[replication][rewind]") {
	const Rewind rewind = Walked(5);

	Vector3 at;

	CHECK_FALSE(rewind.Sample(6.0, At(1), at));

	const Rewind shallow = Walked(20, 4);
	CHECK(shallow.Depth() == 4);
	CHECK(shallow.Oldest() == 17);
	CHECK_FALSE(shallow.Sample(10.0, At(1), at));
	CHECK(shallow.Sample(18.0, At(1), at));
}

TEST_CASE("a fraction past the newest tick answers with the newest", "[replication][rewind]") {
	const Rewind rewind = Walked(5);

	Vector3 at;
	REQUIRE(rewind.Sample(5.5, At(1), at));
	CHECK(at.X == Approx(5.0f));
}

TEST_CASE("an entity absent from a tick is not invented", "[replication][rewind]") {
	Rewind rewind;
	REQUIRE(rewind.Begin(1));
	REQUIRE(rewind.Record(At(1), Vector3{1.0f, 0.0f, 0.0f}));

	REQUIRE(rewind.Begin(2));
	REQUIRE(rewind.Record(At(1), Vector3{2.0f, 0.0f, 0.0f}));
	REQUIRE(rewind.Record(At(2), Vector3{9.0f, 0.0f, 0.0f}));

	Vector3 at;

	CHECK_FALSE(rewind.Sample(1.0, At(2), at));
	CHECK(rewind.Sample(2.0, At(2), at));

	CHECK_FALSE(rewind.Sample(2.0, At(3), at));
}

TEST_CASE("an entity that stops being recorded keeps its last known frame", "[replication][rewind]") {
	Rewind rewind;
	REQUIRE(rewind.Begin(1));
	REQUIRE(rewind.Record(At(1), Vector3{1.0f, 0.0f, 0.0f}));
	REQUIRE(rewind.Begin(2));

	Vector3 at;
	REQUIRE(rewind.Sample(1.0, At(1), at));
	CHECK(at.X == Approx(1.0f));

	REQUIRE(rewind.Sample(1.5, At(1), at));
	CHECK(at.X == Approx(1.0f));
}

TEST_CASE("a repeated or out-of-order tick is refused", "[replication][rewind]") {
	Rewind rewind;
	REQUIRE(rewind.Begin(5));

	CHECK_FALSE(rewind.Begin(5));
	CHECK_FALSE(rewind.Begin(4));
	CHECK(rewind.Begin(6));
	CHECK(rewind.Newest() == 6);
}

TEST_CASE("recording before a tick is open goes nowhere", "[replication][rewind]") {
	Rewind rewind;
	CHECK_FALSE(rewind.Record(At(1), Vector3{}));

	Vector3 at;
	CHECK_FALSE(rewind.Sample(0.0, At(1), at));
	CHECK(rewind.Depth() == 0);
	CHECK(rewind.Oldest() == 0);
}

TEST_CASE("the history is bounded and the oldest falls off", "[replication][rewind]") {
	const Rewind rewind = Walked(100, 8);

	CHECK(rewind.Depth() == 8);
	CHECK(rewind.Newest() == 100);
	CHECK(rewind.Oldest() == 93);

	Vector3 at;
	CHECK(rewind.Sample(93.0, At(1), at));
	CHECK_FALSE(rewind.Sample(92.0, At(1), at));
}

TEST_CASE("a history of zero ticks still records one", "[replication][rewind]") {
	RewindSettings settings;
	settings.HistoryTicks = 0;

	Rewind rewind(settings);
	REQUIRE(rewind.Begin(1));
	REQUIRE(rewind.Record(At(1), Vector3{4.0f, 0.0f, 0.0f}));

	Vector3 at;
	REQUIRE(rewind.Sample(1.0, At(1), at));
	CHECK(at.X == Approx(4.0f));
}

TEST_CASE("clearing forgets everything and keeps working", "[replication][rewind]") {
	Rewind rewind = Walked(5);
	rewind.Clear();

	Vector3 at;
	CHECK_FALSE(rewind.Sample(3.0, At(1), at));
	CHECK(rewind.Depth() == 0);

	CHECK(rewind.Begin(1));
}

TEST_CASE("the tick a client saw is its input less the delay and half the trip", "[replication][rewind]") {
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, 60.0) == Approx(98.0));

	CHECK(Rewind::TickSeenBy(100, 2.0, 100.0, 60.0) == Approx(95.0));

	CHECK(Rewind::TickSeenBy(100, 4.0, 0.0, 60.0) == Approx(96.0));
}

TEST_CASE("a nonsense latency or rate falls back to the uncompensated tick", "[replication][rewind]") {
	const double nan = std::numeric_limits<double>::quiet_NaN();

	CHECK(Rewind::TickSeenBy(100, nan, 0.0, 60.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, nan, 60.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, 0.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, -60.0) == Approx(100.0));

	CHECK(Rewind::TickSeenBy(100, 0.0, -500.0, 60.0) == Approx(100.0));

	CHECK(Rewind::TickSeenBy(1, 100.0, 0.0, 60.0) == Approx(0.0));
}

TEST_CASE("the whole loop answers with what the client saw", "[replication][rewind]") {
	constexpr double TICK_RATE = 60.0;
	constexpr double DELAY_TICKS = 2.0;
	constexpr double LATENCY = 100.0;
	constexpr float SPEED = 10.0f;

	Rewind rewind;
	for (uint64_t tick = 1; tick <= 100; tick++) {
		REQUIRE(rewind.Begin(tick));
		REQUIRE(rewind.Record(At(7), Vector3{static_cast<float>(tick) * SPEED, 0.0f, 0.0f}));
	}

	const double seen = Rewind::TickSeenBy(100, DELAY_TICKS, LATENCY, TICK_RATE);
	CHECK(seen == Approx(95.0));

	Vector3 was;
	REQUIRE(rewind.Sample(seen, At(7), was));

	CHECK(was.X == Approx(950.0f));

	Vector3 now;
	REQUIRE(rewind.Sample(100.0, At(7), now));
	CHECK(now.X == Approx(1000.0f));
	CHECK(now.X - was.X == Approx(50.0f));
}

TEST_CASE("a client on a worse link is rewound further", "[replication][rewind]") {
	const double good = Rewind::TickSeenBy(100, 2.0, 20.0, 60.0);
	const double poor = Rewind::TickSeenBy(100, 2.0, 200.0, 60.0);

	CHECK(good > poor);
	CHECK(good == Approx(97.4));
	CHECK(poor == Approx(92.0));
}
