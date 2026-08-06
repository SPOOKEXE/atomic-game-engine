// Where things were, so a server can judge a shot against what a client saw.
//
// **The cases that matter are the ones where being approximately right is
// wrong.** A rewind that answers with a neighbouring tick's placement, or with
// a whole tick where the client saw a fraction, produces a hit test that is
// subtly generous or subtly mean — and neither shows up as a failure, only as
// players saying the game feels off. So the interpolation, the refusals and the
// ordering are each pinned rather than sampled.

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

	// Records `ticks` ticks with one entity walking along X at one metre a
	// tick, starting at tick one.
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
	// **The case a whole-tick rewind gets wrong.** A renderer sits between two
	// ticks and blends — `SnapshotBuffer::RenderTick` is a double for exactly
	// this reason — so answering with tick three when the client saw three and
	// a half is half a tick of error, and it is largest for the fastest things,
	// which is where a hit test is already hardest.
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

	// Past the newest: a client claiming to have seen the future.
	CHECK_FALSE(rewind.Sample(6.0, At(1), at));

	// Before the oldest, on a short history: a client claiming to have seen
	// something further back than the server kept. Answered with a refusal
	// rather than with the oldest thing held, which would be a placement the
	// client never saw.
	const Rewind shallow = Walked(20, 4);
	CHECK(shallow.Depth() == 4);
	CHECK(shallow.Oldest() == 17);
	CHECK_FALSE(shallow.Sample(10.0, At(1), at));
	CHECK(shallow.Sample(18.0, At(1), at));
}

TEST_CASE("a fraction past the newest tick answers with the newest", "[replication][rewind]") {
	// The ordinary case for an input that arrived promptly, and refusing a
	// query that is one part in sixty out would be worse than answering it.
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

	// Entity two exists at tick two and not at tick one — it had not spawned.
	// Answering the tick-one query with the tick-two placement would be
	// inventing a position the client never saw.
	CHECK_FALSE(rewind.Sample(1.0, At(2), at));
	CHECK(rewind.Sample(2.0, At(2), at));

	// And one it never had at all.
	CHECK_FALSE(rewind.Sample(2.0, At(3), at));
}

TEST_CASE("an entity that stops being recorded keeps its last known frame", "[replication][rewind]") {
	Rewind rewind;
	REQUIRE(rewind.Begin(1));
	REQUIRE(rewind.Record(At(1), Vector3{1.0f, 0.0f, 0.0f}));
	REQUIRE(rewind.Begin(2));

	// Destroyed between the two ticks. The tick it *was* in still answers,
	// which is the whole point: a shot fired at something that has since died
	// is judged against the world the shooter saw.
	Vector3 at;
	REQUIRE(rewind.Sample(1.0, At(1), at));
	CHECK(at.X == Approx(1.0f));

	// And a fractional sample into the tick it is missing from does not
	// interpolate towards nothing.
	REQUIRE(rewind.Sample(1.5, At(1), at));
	CHECK(at.X == Approx(1.0f));
}

TEST_CASE("a repeated or out-of-order tick is refused", "[replication][rewind]") {
	Rewind rewind;
	REQUIRE(rewind.Begin(5));

	// **Refused rather than written into the ring anyway.** A frame is only
	// identified by the tick stamped on it, so one landing in the slot the next
	// tick wanted makes every later query find the wrong frame with nothing
	// saying so.
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
	// The ceiling is on memory *and* on how far back a claim may reach: a
	// client claiming two seconds ago is either on a dreadful connection or
	// lying, and both are answered the same way.
	const Rewind rewind = Walked(100, 8);

	CHECK(rewind.Depth() == 8);
	CHECK(rewind.Newest() == 100);
	CHECK(rewind.Oldest() == 93);

	Vector3 at;
	CHECK(rewind.Sample(93.0, At(1), at));
	CHECK_FALSE(rewind.Sample(92.0, At(1), at));
}

TEST_CASE("a history of zero ticks still records one", "[replication][rewind]") {
	// A setting of zero is a caller mistake, and the smallest thing that is
	// still a history beats a crash.
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

	// A tick that would have been refused as out of order before the clear is
	// accepted after it, which is what makes this usable across a reload.
	CHECK(rewind.Begin(1));
}

TEST_CASE("the tick a client saw is its input less the delay and half the trip", "[replication][rewind]") {
	// **Half the round trip, not all of it.** The gap between what the client
	// saw and what the server holds is the time the *snapshot* took to get out;
	// the input's journey back is already accounted for by the tick it names.
	// Doubling it is the classic way to make compensation too generous.
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, 60.0) == Approx(98.0));

	// 100 ms round trip at 60 Hz is 50 ms one way, which is three ticks.
	CHECK(Rewind::TickSeenBy(100, 2.0, 100.0, 60.0) == Approx(95.0));

	// A perfect link still owes the interpolation delay, because the client is
	// rendering behind whatever it has received.
	CHECK(Rewind::TickSeenBy(100, 4.0, 0.0, 60.0) == Approx(96.0));
}

TEST_CASE("a nonsense latency or rate falls back to the uncompensated tick", "[replication][rewind]") {
	const double nan = std::numeric_limits<double>::quiet_NaN();

	CHECK(Rewind::TickSeenBy(100, nan, 0.0, 60.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, nan, 60.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, 0.0) == Approx(100.0));
	CHECK(Rewind::TickSeenBy(100, 2.0, 0.0, -60.0) == Approx(100.0));

	// A negative latency is clamped rather than rewarded with a rewind into
	// the future.
	CHECK(Rewind::TickSeenBy(100, 0.0, -500.0, 60.0) == Approx(100.0));

	// And nothing rewinds past zero.
	CHECK(Rewind::TickSeenBy(1, 100.0, 0.0, 60.0) == Approx(0.0));
}

TEST_CASE("the whole loop answers with what the client saw", "[replication][rewind]") {
	// **The case that proves the feature rather than the class.** Everything
	// above checks one piece; this runs the situation lag compensation exists
	// for and asserts the number a hit test would actually use.
	//
	// A target moving at ten metres a tick — absurd, and chosen so an error of
	// one tick is ten metres rather than something a tolerance could hide. The
	// client renders two ticks behind on a hundred-millisecond link at sixty
	// hertz, so it is looking five ticks into the past when it acts.
	constexpr double TICK_RATE = 60.0;
	constexpr double DELAY_TICKS = 2.0;
	constexpr double LATENCY = 100.0;
	constexpr float SPEED = 10.0f;

	Rewind rewind;
	for (uint64_t tick = 1; tick <= 100; tick++) {
		REQUIRE(rewind.Begin(tick));
		REQUIRE(rewind.Record(At(7), Vector3{static_cast<float>(tick) * SPEED, 0.0f, 0.0f}));
	}

	// The client produced this input for tick 100 — the newest tick it had —
	// and it arrived while the server is on 100 too.
	const double seen = Rewind::TickSeenBy(100, DELAY_TICKS, LATENCY, TICK_RATE);
	CHECK(seen == Approx(95.0));

	Vector3 was;
	REQUIRE(rewind.Sample(seen, At(7), was));

	// Where the client was aiming.
	CHECK(was.X == Approx(950.0f));

	// **And where the server thinks it is now, which is fifty metres away.**
	// That gap is the whole bug: judged against the current state, a shot that
	// was dead centre on the player's screen misses by five ticks of travel,
	// and no amount of tightening the network closes it — the gap is the speed
	// of light plus the interpolation the smoothness depends on.
	Vector3 now;
	REQUIRE(rewind.Sample(100.0, At(7), now));
	CHECK(now.X == Approx(1000.0f));
	CHECK(now.X - was.X == Approx(50.0f));
}

TEST_CASE("a client on a worse link is rewound further", "[replication][rewind]") {
	// The compensation scales with the link rather than being a constant, which
	// is the difference between compensating and guessing.
	const double good = Rewind::TickSeenBy(100, 2.0, 20.0, 60.0);
	const double poor = Rewind::TickSeenBy(100, 2.0, 200.0, 60.0);

	CHECK(good > poor);
	CHECK(good == Approx(97.4));
	CHECK(poor == Approx(92.0));
}
