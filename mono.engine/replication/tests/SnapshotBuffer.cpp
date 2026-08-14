// Drawing a world nobody here simulates, between the ticks that arrived.
//
// **The assertions are on positions, not on the buffer existing.** A world
// received at 60 Hz and drawn at 240 has to move on every one of those frames,
// by a quarter of a tick each time, and the numbers below say so - a suite that
// only checked a counter would pass with the interpolation removed.
//
// Two halves. The first drives the buffer directly, because the delay, the
// stall and the exclusion are all statements about *when* a tick is recorded
// and nothing about how it got here. The second runs the same measurement over
// `Wire.hpp`'s real loopback, real framing and real encryption with
// `net::LossyTransport` losing a nominated share of it - because jitter is the
// entire justification for this feature and it is now something a suite can
// state rather than argue about.

#include "Wire.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

TEST_SUITE_ID("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.core.types.cframe")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.net.lossytransport")
TEST_DEPENDS("engine.replication.endtoend")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::LossSettings;
using engine::replication::InterpolationSettings;
using engine::replication::SnapshotBuffer;
using namespace replication_wire;

namespace {
	// The authority's rate everything here is stated against, and how many
	// frames are drawn per tick of it. Four is enough that "moved every frame"
	// and "moved once per tick" are different numbers by a wide margin.
	constexpr double TICK_RATE = 60.0;
	constexpr int FRAMES_PER_TICK = 4;
	constexpr double FRAME_SECONDS = 1.0 / (TICK_RATE * FRAMES_PER_TICK);

	// A pose on the X axis and nothing else, so that a position read back is
	// directly comparable with the tick it belongs to.
	CFrame At(double x) {
		return CFrame(Vector3{static_cast<float>(x), 0.0f, 0.0f});
	}

	// An entity that moves one unit per tick, so its X *is* the tick it was
	// recorded at and an interpolated X is the render position.
	constexpr Entity MOVER{1};

	InterpolationSettings Steady() {
		InterpolationSettings settings;
		settings.TickRate = TICK_RATE;
		return settings;
	}
}

TEST_CASE("a world received at a steady rate is drawn between ticks", "[replication][interpolation]") {
	// **The case the whole entry is about.** At 240 fps over a 60 Hz stream
	// there are four frames per received tick, and three of them have no tick of
	// their own. Drawing the newest received pose on all four is the judder;
	// drawing a quarter of the way along each time is what this buys.
	SnapshotBuffer buffer(Steady());

	// Warm up past the first few ticks, where there is genuinely only one pose
	// to hold and holding it is correct.
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	double previous = buffer.Sample(MOVER).value().Position.X;
	int moved = 0;

	for (uint64_t tick = 9; tick <= 24; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));

		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);

			const std::optional<CFrame> pose = buffer.Sample(MOVER);
			REQUIRE(pose.has_value());
			const double x = pose->Position.X;

			// The entity moves one unit per tick, so its position *is* the
			// render position. This is the assertion that fails the moment the
			// delay is removed or the interpolation is replaced by the newest
			// pose.
			REQUIRE(x == Approx(buffer.RenderTick()).margin(1e-4));

			// A quarter of a tick per frame, every frame. Not "it changed" -
			// how much it changed by.
			REQUIRE(x - previous == Approx(1.0 / FRAMES_PER_TICK).margin(0.01));

			previous = x;
			moved++;
		}
	}

	REQUIRE(moved == 16 * FRAMES_PER_TICK);

	// It is drawn behind the newest tick, and by the stated amount. A world in
	// step with the newest tick is a world that cannot interpolate.
	REQUIRE(buffer.Behind() >= buffer.Settings().DelayTicks - 1.0);
	REQUIRE(buffer.Behind() <= buffer.Settings().DelayTicks);

	// And it interpolated rather than held, overwhelmingly.
	REQUIRE(buffer.Stats().Interpolated > buffer.Stats().Held);
	REQUIRE(buffer.Stats().Stalls == 0);
	REQUIRE(buffer.Stats().Resyncs == 0);
}

TEST_CASE(
	"the newest received tick is never drawn, only interpolated toward", "[replication][interpolation]"
) {
	// The delay stated as a position rather than as a counter: with two ticks of
	// budget the world on screen is two ticks old, so a tick that has just
	// arrived has not been drawn yet and a tick two back has been drawn in full.
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 20; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	const double drawn = buffer.Sample(MOVER).value().Position.X;
	REQUIRE(drawn < 20.0);
	REQUIRE(drawn > 17.0);
}

TEST_CASE("nothing is extrapolated when the buffer runs dry", "[replication][interpolation]") {
	// **The stated answer to "what happens when it runs out", and it is to
	// stop.** Guessing forward produces a pose the server never sent and then a
	// snap when the next tick disagrees, which is worse than a freeze because it
	// is a freeze plus a lie.
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	// Nothing more arrives. The clock eats the delay and then stops.
	for (int frame = 0; frame < 40; frame++) {
		buffer.Advance(FRAME_SECONDS);
		REQUIRE(buffer.Sample(MOVER).value().Position.X <= 8.0 + 1e-4);
	}

	REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(8.0).margin(1e-4));
	REQUIRE(buffer.RenderTick() == Approx(8.0).margin(1e-9));
	REQUIRE(buffer.Stats().Stalls > 0);

	// And it comes back where it stopped rather than where it would have been.
	buffer.Record(9, MOVER, At(9.0));
	buffer.Advance(FRAME_SECONDS);
	const double resumed = buffer.Sample(MOVER).value().Position.X;
	REQUIRE(resumed > 8.0);
	REQUIRE(resumed < 8.5);
}

TEST_CASE("a dry buffer measures the guess it does not itself make", "[replication][interpolation]") {
	// **The `D00015(c)` half, and the case above is the half it does not
	// touch.** The clock still stops, `Sample` still holds the last pose the
	// authority described, and this class still extrapolates nothing - what it
	// gained is a number saying how much time it could not spend, for a caller
	// that has a velocity to spend it with.
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	// A healthy link has nothing to guess about: the clock runs a delay behind
	// the newest sample, so there is always something to interpolate toward.
	REQUIRE(buffer.DeadReckonSeconds() == 0.0);
	REQUIRE(buffer.Stats().Extrapolated == 0);

	// Nothing more arrives for a second.
	double previousReckon = 0.0;
	for (int frame = 0; frame < 240; frame++) {
		buffer.Advance(FRAME_SECONDS);

		// Never backwards, and never past the horizon.
		REQUIRE(buffer.DeadReckonSeconds() >= previousReckon);
		REQUIRE(buffer.DeadReckonSeconds() <= buffer.Settings().ExtrapolateSeconds);
		previousReckon = buffer.DeadReckonSeconds();

		// And D00010's answer, unchanged: this class never hands back a pose
		// past the last one the server actually described.
		REQUIRE(buffer.Sample(MOVER).value().Position.X <= 8.0 + 1e-4);
	}

	// **Saturated, and the horizon is what stopped it rather than the run
	// ending.** A second of silence is four times the quarter-second the guess
	// is worth.
	REQUIRE(buffer.DeadReckonSeconds() == Approx(buffer.Settings().ExtrapolateSeconds));
	REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(8.0).margin(1e-4));
	REQUIRE(buffer.Stats().Extrapolated > 0);
	REQUIRE(buffer.Stats().Stalls >= buffer.Stats().Extrapolated);
}

TEST_CASE("the guess is given back rather than dropped", "[replication][interpolation]") {
	// **The correction decision, stated as a rate.** The offset a caller adds is
	// a function of this number, so easing it to zero *is* the blend - and it is
	// eased at half real time, which is slow enough that a corrected body still
	// moves forward while it is being taken back.
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	for (int frame = 0; frame < 240; frame++) {
		buffer.Advance(FRAME_SECONDS);
	}
	const double saturated = buffer.DeadReckonSeconds();
	REQUIRE(saturated > 0.0);

	// The stream returns. One frame does not undo a quarter of a second.
	buffer.Record(9, MOVER, At(9.0));
	buffer.Advance(FRAME_SECONDS);
	REQUIRE(buffer.DeadReckonSeconds() > saturated * 0.9);

	// Given back at `UnwindFraction` of real time, so a horizon's worth takes
	// twice the horizon to unwind.
	int frames = 1;
	double previous = buffer.DeadReckonSeconds();
	while (buffer.DeadReckonSeconds() > 0.0 && frames < 2000) {
		buffer.Record(9 + static_cast<uint64_t>(frames) / FRAMES_PER_TICK, MOVER, At(9.0));
		buffer.Advance(FRAME_SECONDS);
		REQUIRE(buffer.DeadReckonSeconds() <= previous);
		REQUIRE(buffer.DeadReckonSeconds() >= 0.0);
		previous = buffer.DeadReckonSeconds();
		frames++;
	}

	const double unwoundSeconds = static_cast<double>(frames) * FRAME_SECONDS;
	REQUIRE(
		unwoundSeconds == Approx(saturated / buffer.Settings().UnwindFraction).margin(4.0 * FRAME_SECONDS)
	);
}

TEST_CASE("dead reckoning switched off is D00010 unchanged", "[replication][interpolation]") {
	// **The control, and the reason the horizon is a setting rather than a
	// constant.** A host that wants the v0.5 behaviour asks for zero and gets a
	// buffer that measures nothing, offers nothing, and stalls exactly as it
	// always did.
	InterpolationSettings settings = Steady();
	settings.ExtrapolateSeconds = 0.0;
	SnapshotBuffer buffer(settings);

	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	for (int frame = 0; frame < 240; frame++) {
		buffer.Advance(FRAME_SECONDS);
		REQUIRE(buffer.DeadReckonSeconds() == 0.0);
	}

	REQUIRE(buffer.Stats().Stalls > 0);
	REQUIRE(buffer.Stats().Extrapolated == 0);
	REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(8.0).margin(1e-4));
}

TEST_CASE("clearing drops the guess with everything else", "[replication][interpolation]") {
	// A rejoin: the ticks held describe a world that no longer exists, and so
	// does anything derived from how long ago the last of them arrived.
	SnapshotBuffer buffer(Steady());
	buffer.Record(1, MOVER, At(1.0));
	for (int frame = 0; frame < 120; frame++) {
		buffer.Advance(FRAME_SECONDS);
	}
	REQUIRE(buffer.DeadReckonSeconds() > 0.0);

	buffer.Clear();
	REQUIRE(buffer.DeadReckonSeconds() == 0.0);
}

TEST_CASE("a gap larger than the delay holds and then closes it smoothly", "[replication][interpolation]") {
	// A stall is not a failure to have a policy. The world holds the last pose
	// it was actually told about, and when the stream returns it walks the gap
	// at a bounded rate rather than teleporting across it.
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	// Ticks 9 to 13 never arrive - five tick periods, well past a two-tick
	// budget and well inside the resync threshold.
	for (int frame = 0; frame < 5 * FRAMES_PER_TICK; frame++) {
		buffer.Advance(FRAME_SECONDS);
	}
	REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(8.0).margin(1e-4));
	REQUIRE(buffer.Stats().Resyncs == 0);

	// Tick 14 lands. The two poses bracketing the render position are now 8 and
	// 14, six ticks apart, and the walk between them is continuous.
	buffer.Record(14, MOVER, At(14.0));

	double previous = 8.0;
	double largestStep = 0.0;
	for (int frame = 0; frame < 8 * FRAMES_PER_TICK; frame++) {
		buffer.Advance(FRAME_SECONDS);
		const double x = buffer.Sample(MOVER).value().Position.X;
		REQUIRE(x >= previous - 1e-4);
		largestStep = std::max(largestStep, x - previous);
		previous = x;
	}

	// **No teleport.** The clock closes the gap at most `CorrectionFraction`
	// faster than the authority runs, so a frame can never cover more than a
	// little over its own quarter tick.
	REQUIRE(largestStep < 0.5);
	REQUIRE(buffer.Stats().Resyncs == 0);
}

TEST_CASE(
	"a pause past the resync threshold jumps once rather than crawling", "[replication][interpolation]"
) {
	SnapshotBuffer buffer(Steady());
	buffer.Record(1, MOVER, At(1.0));
	buffer.Advance(FRAME_SECONDS);

	// Six hundred ticks later. Easing that back at five percent would take three
	// minutes of a world ten seconds behind.
	buffer.Record(600, MOVER, At(600.0));
	buffer.Advance(FRAME_SECONDS);

	REQUIRE(buffer.Stats().Resyncs == 1);
	REQUIRE(buffer.Behind() < buffer.Settings().DelayTicks + 1.0);
}

TEST_CASE("the predicted entity is not delayed", "[replication][interpolation][prediction]") {
	// **Prediction and interpolation are the two halves and they pull opposite
	// ways.** The local player is run ahead so input feels immediate; handing it
	// back a pose two ticks old puts exactly that lag back, and the player feels
	// it as their own character lagging their own keys.
	SnapshotBuffer buffer(Steady());

	constexpr Entity PLAYER{2};
	buffer.Predict(PLAYER);
	REQUIRE(buffer.Predicted() == PLAYER);

	for (uint64_t tick = 1; tick <= 16; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		buffer.Record(tick, PLAYER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}

	// Everything else is behind, which is the point of the buffer: a full delay
	// at the instant a tick lands, and a tick less than that just before the
	// next one does.
	const std::optional<CFrame> other = buffer.Sample(MOVER);
	REQUIRE(other.has_value());
	REQUIRE(other->Position.X <= 15.0 + 1e-4);
	REQUIRE(other->Position.X >= 14.0 - 1e-4);

	// The player is not. Nothing is offered, so the caller draws the live,
	// predicted row it already has - undelayed by construction rather than by a
	// second copy of the pose that would have to be kept in step.
	REQUIRE_FALSE(buffer.Sample(PLAYER).has_value());
}

TEST_CASE(
	"nominating a predicted entity drops what was already held for it",
	"[replication][interpolation][prediction]"
) {
	// A client that starts predicting an entity part way through - the moment it
	// is told which row is its own - must not go on drawing the delayed pose
	// while the ring drains.
	SnapshotBuffer buffer(Steady());

	constexpr Entity PLAYER{2};
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, PLAYER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}
	REQUIRE(buffer.Sample(PLAYER).has_value());

	buffer.Predict(PLAYER);
	REQUIRE_FALSE(buffer.Sample(PLAYER).has_value());
}

TEST_CASE("an entity this client minted is never buffered", "[replication][interpolation][prediction]") {
	// The other half of the same rule, and this one is structural rather than
	// nominated: `Store::CreatePredicted` mints above 2^31, which the authority
	// never allocates from - so a row in that range is this client's own
	// run-ahead and there is no received tick for it to be delayed against.
	Store store("interpolation_predicted");
	const Entity local = store.CreatePredicted();
	REQUIRE(Store::IsPredicted(local));

	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, local, At(static_cast<double>(tick)));
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		buffer.Advance(1.0 / TICK_RATE);
	}

	REQUIRE_FALSE(buffer.Sample(local).has_value());
	REQUIRE(buffer.Sample(MOVER).has_value());
}

TEST_CASE("a tick that repeats a value invents no motion", "[replication][interpolation]") {
	// **The tick a value did not arrive on, which since D00013 is the tick the
	// per-client budget held it over on rather than a tick a datagram was lost
	// from.** A tick short of one of its parts is never acknowledged and
	// therefore never reaches this buffer at all - `Record` is fed
	// `Replica::Applied`. A tick the *budget* trimmed is complete, is
	// acknowledged, and does leave the rows it held over at their previous
	// value, which is what a walk of the store then finds.
	//
	// Either way the entity reads as having stood still for a tick and moved
	// twice as far on the next, and that is smoothed across two segments instead
	// of one - never a pose between two values the server did not send in that
	// order.
	SnapshotBuffer buffer(Steady());

	// Ticks 1 to 4 arrive. Tick 5 carried nothing for this entity, so the store
	// still holds tick 4's value. Tick 6 arrives.
	const double values[] = {1.0, 2.0, 3.0, 4.0, 4.0, 6.0, 7.0, 8.0, 9.0, 10.0};

	double previous = 0.0;
	bool sawTheStall = false;
	bool sawTheCatchUp = false;

	// Every pose the render clock passes through has to be one the server could
	// have produced, in the order it produced them.
	const auto check = [&] {
		const double x = buffer.Sample(MOVER).value().Position.X;
		REQUIRE(x >= previous - 1e-4);
		REQUIRE(x <= 10.0 + 1e-4);

		const double render = buffer.RenderTick();
		if (render > 4.1 && render < 4.9) {
			// Between tick 4 and tick 5, both of which say 4.0. The
			// entity stands still, because that is what this client knows.
			REQUIRE(x == Approx(4.0).margin(1e-4));
			sawTheStall = true;
		}
		if (render > 5.4 && render < 5.6) {
			// Between the repeated 4.0 and tick 6's 6.0, so halfway is 5.0. The
			// motion is spread across the whole span rather than snapped at its
			// end.
			REQUIRE(x == Approx(5.0).margin(0.15));
			sawTheCatchUp = true;
		}
		previous = x;
	};

	for (uint64_t tick = 1; tick <= 10; tick++) {
		buffer.Record(tick, MOVER, At(values[tick - 1]));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
			check();
		}
	}

	// Drain what is left, so the render clock reaches the newest pose rather
	// than stopping a delay short of it.
	for (int frame = 0; frame < 4 * FRAMES_PER_TICK; frame++) {
		buffer.Advance(FRAME_SECONDS);
		check();
	}

	REQUIRE(sawTheStall);
	REQUIRE(sawTheCatchUp);
	REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(10.0).margin(1e-4));
}

TEST_CASE(
	"polling many times for one tick does not fill the buffer with it", "[replication][interpolation]"
) {
	// **A client polls its connection every frame and the server ticks at
	// 60 Hz**, so most polls find the tick the last one found. Twenty repeats of
	// one tick is more than the history is deep, so a buffer that took them all
	// would hold nothing but the newest tick - no pair to bracket the render
	// position, every frame held at the newest pose, and the judder back with
	// the buffer still in place and every counter looking healthy.
	SnapshotBuffer buffer(Steady());
	REQUIRE_FALSE(buffer.Holds(1));

	double previous = 0.0;
	for (uint64_t tick = 1; tick <= 24; tick++) {
		for (int poll = 0; poll < 20; poll++) {
			buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		}
		REQUIRE(buffer.Holds(tick));

		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
			const double x = buffer.Sample(MOVER).value().Position.X;
			if (tick > 8) {
				REQUIRE(x - previous == Approx(1.0 / FRAMES_PER_TICK).margin(0.01));
			}
			previous = x;
		}
	}

	REQUIRE(buffer.Stats().Ticks == 24);

	// And a repeat carrying a different value is still a repeat. The tick is the
	// identity; a second answer for one tick is a poll, not new state.
	buffer.Record(24, MOVER, At(999.0));
	REQUIRE(buffer.Sample(MOVER).value().Position.X < 100.0);
}

TEST_CASE("a rejoin forgets a world that no longer exists", "[replication][interpolation]") {
	SnapshotBuffer buffer(Steady());
	for (uint64_t tick = 1; tick <= 8; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		buffer.Advance(1.0 / TICK_RATE);
	}
	REQUIRE(buffer.Sample(MOVER).has_value());

	buffer.Clear();
	REQUIRE_FALSE(buffer.Sample(MOVER).has_value());
	REQUIRE(buffer.Newest() == 0);
	REQUIRE(buffer.Behind() == 0.0);

	// **And the render position itself, which is public and is read by a
	// panel.** Found by mutation: leaving `RenderTicks` alone here changed
	// nothing any other case could see, because the next `Record` reseeds it
	// and `Advance` does nothing until then. But between a rejoin and the first
	// tick of the new world, `RenderTick()` is readable - and a tick number
	// belonging to a world that no longer exists is exactly the reading that
	// sends somebody looking for a bug in the wrong connection.
	REQUIRE(buffer.RenderTick() == 0.0);

	// And a clock that starts again rather than one that thinks it is behind by
	// the whole of the previous connection.
	buffer.Record(400, MOVER, At(400.0));
	buffer.Advance(FRAME_SECONDS);
	REQUIRE(buffer.Behind() < buffer.Settings().DelayTicks + 1.0);
	REQUIRE(buffer.Stats().Resyncs == 0);
}

TEST_CASE("an entity that stops being replicated is dropped", "[replication][interpolation]") {
	// Otherwise the map grows for the life of the connection, one track per
	// entity that was ever in view. `HistoryTicks` past its last pose is the
	// point at which it could no longer bracket anything anyway.
	SnapshotBuffer buffer(Steady());
	constexpr Entity GONE{7};

	buffer.Record(1, GONE, At(1.0));
	for (uint64_t tick = 1; tick <= 40; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
	}

	REQUIRE_FALSE(buffer.Sample(GONE).has_value());
	REQUIRE(buffer.Sample(MOVER).has_value());

	// And the explicit form, for a destroy the caller knows about at once.
	buffer.Forget(MOVER);
	REQUIRE_FALSE(buffer.Sample(MOVER).has_value());
}

TEST_CASE(
	"under constructed loss the replica is drawn smoothly and the raw stream is not",
	"[replication][interpolation][loss]"
) {
	// **The comparison is the test.** Both columns are measured from the same
	// run over the same lossy link: one reads the client's store the way
	// `Replicated.cpp` used to, the other reads the buffer. A world drawn at
	// four frames per tick moves on one frame in four when it is read raw, and
	// on every frame when it is interpolated - and no counter is being asserted
	// on, only how often the drawn position actually changed.
	//
	// **Nominated rather than drawn, which is what `LossyTransport`'s own header
	// asks for and this case used to ignore.** It lost a tenth of the link
	// against `Random::Float(arrival, seed)`, so the pattern was whatever that
	// generator happened to produce - and when `core::Random` was replaced at
	// v0.15 every draw moved, a burst of three consecutive losses appeared where
	// there had been none, and `bufferedFrozen == 0` went red for a reason that
	// had nothing to do with interpolation. A seed is not a statement about the
	// link; this list is.
	//
	// **One in ten and never two in a row, because that is the pattern
	// `DelayTicks = 2` is documented to absorb** - "one lost and recovered by
	// the next, absorbed with nothing to spare". So the assertions below test
	// the guarantee the constant claims rather than a run that happened to stay
	// inside it.
	LossSettings toClient;
	for (uint64_t arrival = 7; arrival < 400; arrival += 10) {
		toClient.Drop.push_back(arrival);
	}

	Wire wire({}, {}, toClient);

	const Entity mover = wire.Server.Create();
	wire.Server.Set<Spot>(mover, Spot{0.0f, 0.0f});
	REQUIRE(wire.Join(512));

	SnapshotBuffer buffer(Steady());

	int rawFrozen = 0;
	int bufferedFrozen = 0;
	int frames = 0;
	double rawPrevious = -1.0;
	double bufferedPrevious = -1.0;
	double largestBufferedStep = 0.0;
	double largestRawStep = 0.0;

	for (int step = 0; step < 240; step++) {
		// One unit per tick, so a drawn X is directly comparable with a tick.
		wire.Server.GetMutable<Spot>(mover)->X = static_cast<float>(wire.Tick_ + 1);
		wire.Tick();

		const uint64_t applied = wire.Replica_.Applied();
		if (!buffer.Holds(applied)) {
			wire.Client.Each<const Spot>([&](Entity entity, const Spot &spot) {
				buffer.Record(applied, entity, At(spot.X));
			});
		}

		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);

			const Spot *raw = wire.Client.Get<Spot>(mover);
			REQUIRE(raw != nullptr);
			const std::optional<CFrame> pose = buffer.Sample(mover);
			REQUIRE(pose.has_value());

			// The first frames of each column have nothing to compare against.
			if (step >= 16) {
				frames++;
				if (raw->X == Approx(rawPrevious).margin(1e-4)) {
					rawFrozen++;
				} else {
					largestRawStep = std::max(largestRawStep, std::abs(raw->X - rawPrevious));
				}
				if (pose->Position.X == Approx(bufferedPrevious).margin(1e-4)) {
					bufferedFrozen++;
				} else {
					largestBufferedStep =
						std::max(largestBufferedStep, std::abs(pose->Position.X - bufferedPrevious));
				}
			}

			rawPrevious = raw->X;
			bufferedPrevious = pose->Position.X;
		}
	}

	// The link really did lose things, or this case proves nothing. 24 of 241
	// arrivals, which is the tenth the list above nominates.
	REQUIRE(wire.ClientEnd->Stats().Dropped > 0);
	REQUIRE(frames > 800);

	// The raw stream is frozen for three frames in four at best, and more than
	// that wherever a datagram went missing.
	REQUIRE(rawFrozen > frames / 2);

	// **The buffered one never froze at all, and zero is the measurement rather
	// than an aspiration.** The same run at `DelayTicks = 0` freezes 23 frames,
	// so this is the assertion that fails if the delay is taken away.
	REQUIRE(bufferedFrozen == 0);

	// **And this is what pins the default at two rather than one.** A run this
	// regular is absorbed by a one-tick delay too - 0 frozen frames - so the
	// frozen count alone no longer tells the two apart; the stall counter does,
	// because it is the more sensitive of the two and the clock reaching the
	// newest sample does not always cost a frozen frame. Measured on this
	// pattern: 0 stalls at `DelayTicks = 2`, 4 at 1, 54 at 0.
	//
	// Exact rather than a bound, because nothing in this case is drawn any more
	// - the loss list, the tick schedule and the frame schedule are all stated,
	// so a stall that appears is a change in the buffer and not in a generator.
	REQUIRE(buffer.Stats().Stalls == 0);

	// And it never snaps. The raw stream jumps a whole tick's worth at a time
	// and more across a loss; the buffered one covers a quarter of a tick per
	// frame, plus the five percent it is allowed to catch up by.
	REQUIRE(largestRawStep >= 1.0);
	REQUIRE(largestBufferedStep < 0.5);

	// A jump would be a legitimate answer to a link that fell over, and this
	// link did not: it lost a tenth of what crossed it and stayed inside the
	// jitter budget the whole way.
	REQUIRE(buffer.Stats().Resyncs == 0);
	REQUIRE(buffer.Stats().Interpolated > buffer.Stats().Held);
}

TEST_CASE("the tick rate is measured rather than believed", "[replication][interpolation]") {
	// **Nothing on the wire carries the authority's tick rate, and the two
	// programs do not share a default.** `server --listen` paces at 30 and
	// `client` at 60, so the most ordinary pair of command lines there is hands
	// this class a figure that is wrong by a factor of two - and that is not a
	// drift the correction absorbs. Believing it would run the render clock at
	// twice the rate ticks arrive, so the world would cover a tick in half a
	// tick period and then sit frozen for the other half: judder, with a
	// snapshot buffer in the way and every counter looking healthy.
	InterpolationSettings settings;
	settings.TickRate = 60.0;
	SnapshotBuffer buffer(settings);

	// Ticks at 30 Hz, frames at 240.
	constexpr double SERVER_TICK_SECONDS = 1.0 / 30.0;
	constexpr int FRAMES = 8;

	for (uint64_t tick = 1; tick <= 40; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES; frame++) {
			buffer.Advance(SERVER_TICK_SECONDS / FRAMES);
		}
	}

	REQUIRE(buffer.MeasuredTickRate() == Approx(30.0).margin(0.2));

	// And what the measurement is for: an eighth of a tick per frame, every
	// frame, with nothing frozen and nothing stalled.
	const uint64_t stallsBefore = buffer.Stats().Stalls;
	double previous = buffer.Sample(MOVER).value().Position.X;

	for (uint64_t tick = 41; tick <= 80; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES; frame++) {
			buffer.Advance(SERVER_TICK_SECONDS / FRAMES);
			const double x = buffer.Sample(MOVER).value().Position.X;
			REQUIRE(x - previous == Approx(1.0 / FRAMES).margin(0.01));
			previous = x;
		}
	}

	REQUIRE(buffer.Stats().Stalls == stallsBefore);
	REQUIRE(buffer.Stats().Resyncs == 0);
}

TEST_CASE(
	"a predicted entity is not stored either, not merely not answered",
	"[replication][interpolation][prediction]"
) {
	// **Found by mutating the guard away, which nothing failed.** `Sample`
	// refuses a predicted entity and `Record` also refuses it, and until this
	// case existed only the first of those two was checked - so deleting the
	// second left every test green while the buffer quietly kept a ring of
	// sixteen poses per predicted entity, per connection, that nothing could
	// ever read. A client predicting projectiles pays that in memory for state
	// it has already decided is not authoritative.
	//
	// The refusal is observable without a private accessor because `Record`
	// declines *before* the tick accounting: a tick in which the only thing
	// offered was predicted is a tick this buffer never saw.
	SnapshotBuffer buffer(Steady());

	constexpr Entity PLAYER{2};
	buffer.Predict(PLAYER);

	for (uint64_t tick = 1; tick <= 12; tick++) {
		buffer.Record(tick, PLAYER, At(static_cast<double>(tick)));
		buffer.Advance(1.0 / TICK_RATE);
	}

	// Nothing was recorded, so no tick was ever seen and the render clock never
	// started. Starting it off a predicted row would be seeding an
	// interpolation clock from the one entity that is never interpolated.
	REQUIRE(buffer.Stats().Ticks == 0);
	REQUIRE(buffer.Newest() == 0);
	REQUIRE(buffer.RenderTick() == 0.0);
	REQUIRE_FALSE(buffer.Holds(12));

	// And the same for the structural half of the rule, which is the one a
	// caller cannot get wrong by forgetting to nominate.
	Store store("interpolation_predicted_store");
	const Entity local = store.CreatePredicted();

	SnapshotBuffer second(Steady());
	for (uint64_t tick = 1; tick <= 12; tick++) {
		second.Record(tick, local, At(static_cast<double>(tick)));
		second.Advance(1.0 / TICK_RATE);
	}
	REQUIRE(second.Stats().Ticks == 0);
	REQUIRE(second.Newest() == 0);
}

TEST_CASE(
	"an entity recreated where another stood is never interpolated from it", "[replication][interpolation]"
) {
	// **The bug this class of feature always has.** A track is keyed by entity,
	// an entity is destroyed and a new one appears, and the new one is drawn
	// sliding in from wherever its predecessor happened to stop. Nothing on
	// screen was ever at any of those positions.
	//
	// Two halves, because there are two ways the case arrives. The first is the
	// one that actually happens: `ecs::Entity` carries a generation in its high
	// bits, so a recycled index is a *different handle* and the map cannot
	// collide. That is the property being tested, and it is worth a test rather
	// than a comment because it is the whole defence - key this map on an index
	// and the bug is back.
	SnapshotBuffer buffer(Steady());

	Store store("interpolation_recreate");
	const Entity first = store.Create();

	for (uint64_t tick = 1; tick <= 12; tick++) {
		buffer.Record(tick, first, At(static_cast<double>(tick) * 10.0));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}
	REQUIRE(buffer.Sample(first).value().Position.X > 50.0);

	store.Destroy(first);
	const Entity second = store.Create();

	// The index came back and the handle did not. If this ever fails, the
	// assertions below stop meaning anything and the `Forget` half is the only
	// defence left.
	REQUIRE(second != first);

	for (uint64_t tick = 13; tick <= 20; tick++) {
		buffer.Record(tick, second, At(-500.0));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
			const std::optional<CFrame> pose = buffer.Sample(second);
			REQUIRE(pose.has_value());

			// Never anywhere between the two. Not "close to -500" - nowhere in
			// the gap at all, because every point in that gap is a place
			// neither entity was ever at.
			REQUIRE(pose->Position.X == Approx(-500.0).margin(1e-4));
		}
	}

	// **The second half: the same case with the handle reused anyway.** A
	// generation is 32 bits and wraps, and an authority is free to hand out
	// whatever it likes - so the buffer also has to be droppable by hand, and a
	// caller that knows a row was destroyed says so. This is what
	// `SnapshotBuffer::Forget` is for and it is the only defence when the
	// handle does come back.
	SnapshotBuffer reused(Steady());
	constexpr Entity SAME{99};

	for (uint64_t tick = 1; tick <= 12; tick++) {
		reused.Record(tick, SAME, At(static_cast<double>(tick) * 10.0));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			reused.Advance(FRAME_SECONDS);
		}
	}
	REQUIRE(reused.Sample(SAME).value().Position.X > 50.0);

	reused.Forget(SAME);
	REQUIRE_FALSE(reused.Sample(SAME).has_value());

	for (uint64_t tick = 13; tick <= 20; tick++) {
		reused.Record(tick, SAME, At(-500.0));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			reused.Advance(FRAME_SECONDS);
			REQUIRE(reused.Sample(SAME).value().Position.X == Approx(-500.0).margin(1e-4));
		}
	}
}

TEST_CASE("a rejoin does not interpolate across the world it replaced", "[replication][interpolation]") {
	// A re-snapshot is a discontinuity by construction: the server decided this
	// client was too far behind to catch up with deltas, so the state it is
	// about is not the state the deltas were building on. Interpolating across
	// that draws a smooth walk between two worlds.
	SnapshotBuffer buffer(Steady());

	for (uint64_t tick = 1; tick <= 12; tick++) {
		buffer.Record(tick, MOVER, At(static_cast<double>(tick)));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
		}
	}
	REQUIRE(buffer.Sample(MOVER).value().Position.X > 5.0);

	// The snapshot lands. Everything held describes a world that is gone.
	buffer.Clear();

	for (uint64_t tick = 400; tick <= 412; tick++) {
		buffer.Record(tick, MOVER, At(-999.0));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
			REQUIRE(buffer.Sample(MOVER).value().Position.X == Approx(-999.0).margin(1e-4));
		}
	}

	// And no jump was counted, because there was nothing to jump from - a
	// cleared clock starts at the delay rather than believing it is four
	// hundred ticks behind.
	REQUIRE(buffer.Stats().Resyncs == 0);
}

TEST_CASE(
	"under seeded reordering and duplication the drawn world never goes backwards",
	"[replication][interpolation][loss]"
) {
	// **Reordering is the case a `PreviousTransform` written on arrival gets
	// wrong, and it is why this entry refused that fix.** A delayed datagram
	// delivered behind a newer one is a tick that arrives out of order; writing
	// "where it was" from whichever packet landed last would walk the world
	// backwards. Keyed on ticks rather than on arrivals, it cannot.
	//
	// Duplication is here in the same case because a resend looks identical to
	// one, and a receiver that acted on it twice would push the pose that
	// brackets the render position out of the ring.
	LossSettings toClient;
	toClient.Reorder = {12, 13, 24, 31, 47, 52, 66, 71, 88, 93};
	toClient.Duplicate = {15, 27, 39, 58, 74, 91};
	toClient.LossChance = 0.05f;
	toClient.Seed = 0x5eed'0011u;

	Wire wire({}, {}, toClient);

	const Entity mover = wire.Server.Create();
	wire.Server.Set<Spot>(mover, Spot{0.0f, 0.0f});
	REQUIRE(wire.Join(512));

	SnapshotBuffer buffer(Steady());

	double previous = -1e9;
	double largestStep = 0.0;
	int samples = 0;

	for (int step = 0; step < 200; step++) {
		// Monotonic on the server by construction, so anything non-monotonic on
		// screen was produced here rather than sent.
		wire.Server.GetMutable<Spot>(mover)->X = static_cast<float>(wire.Tick_ + 1);
		wire.Tick();

		const uint64_t applied = wire.Replica_.Applied();
		if (!buffer.Holds(applied)) {
			wire.Client.Each<const Spot>([&](Entity entity, const Spot &spot) {
				buffer.Record(applied, entity, At(spot.X));
			});
		}

		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			buffer.Advance(FRAME_SECONDS);
			const std::optional<CFrame> pose = buffer.Sample(mover);
			REQUIRE(pose.has_value());

			const double x = pose->Position.X;

			// **The assertion the case exists for.** Not "smooth" - never
			// backwards, on any frame, ever.
			REQUIRE(x >= previous - 1e-4);

			if (samples > 0) {
				largestStep = std::max(largestStep, x - previous);
			}
			previous = x;
			samples++;
		}
	}

	// The link really did reorder and duplicate, or this case proves nothing
	// and passes because the wire was clean.
	REQUIRE(wire.ClientEnd->Stats().Reordered > 0);
	REQUIRE(wire.ClientEnd->Stats().Duplicated > 0);
	REQUIRE(wire.ClientEnd->Stats().Dropped > 0);

	// And it never teleported: the clock covers a quarter tick per frame plus
	// the five percent it may catch up by, whatever order the ticks landed in.
	REQUIRE(largestStep < 0.5);
	REQUIRE(buffer.Stats().Interpolated > buffer.Stats().Held);
}
