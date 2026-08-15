#include <engine/core/Metrics.hpp>
#include <engine/net/Congestion.hpp>
#include <engine/net/Packet.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.net.congestion")

using Catch::Approx;
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.core.metrics")

using engine::net::CongestionControl;
using engine::net::CongestionSettings;
using engine::net::Packet;

namespace {
	constexpr double TICK = 1.0 / 60.0;

	// The cap a link would hand down, in payload bytes for one tick. The
	// `LinkSettings` default, so a case that reaches it has reached the number a
	// connection actually has.
	constexpr uint32_t CAP = 64 * 1024;

	// A bottleneck with a rate, a buffer and a propagation delay.
	//
	// **The only way to prove a congestion controller converges is to give it
	// something to converge on**, and a fixed reply of "the round trip is 40 ms"
	// proves nothing at all - the whole question is what happens when the round
	// trip is a function of what the controller just did. So this is the smallest
	// honest path model: bytes offered above the drain rate queue up, the queue
	// is what the round trip is made of, and a queue past the buffer is loss.
	//
	// Deterministic and stated in the test, exactly as `LossyTransport` is:
	// no clock, nothing random, and the same numbers every run.
	struct Path {
		double CapacityBytesPerSecond = 250'000.0; // 2 Mbit/s.
		double BufferSeconds = 0.3;				   // A home router's, roughly.
		double PropagationSeconds = 0.04;		   // 40 ms each way and back.

		double QueuedBytes = 0.0;
		uint32_t DroppedThisStep = 0;

		// Offers one tick's worth of bytes and answers the round trip a packet
		// sent now would see.
		double Step(double offeredBytes, double elapsedSeconds) {
			QueuedBytes += offeredBytes;

			const double drained = CapacityBytesPerSecond * elapsedSeconds;
			QueuedBytes = std::max(0.0, QueuedBytes - drained);

			const double buffer = CapacityBytesPerSecond * BufferSeconds;
			DroppedThisStep = 0;
			if (QueuedBytes > buffer) {
				DroppedThisStep = static_cast<uint32_t>(
					(QueuedBytes - buffer) / static_cast<double>(Packet::MAXIMUM_PAYLOAD_BYTES) + 1.0
				);
				QueuedBytes = buffer;
			}

			return PropagationSeconds + QueuedBytes / CapacityBytesPerSecond;
		}

		double QueueSeconds() const {
			return QueuedBytes / CapacityBytesPerSecond;
		}
	};

	// Runs a controller against a path for a stated number of ticks.
	//
	// The controller is fed exactly what a `Link` feeds it: a round trip, a
	// count of sends the far side did not get, and one `Advance` per tick.
	struct Run {
		double PeakQueueSeconds = 0.0;
		double FinalRateBytesPerSecond = 0.0;
		uint64_t Reductions = 0;
		std::vector<double> Rates;
	};

	Run Drive(CongestionControl &control, Path &path, int ticks, double now = 0.0, uint32_t cap = CAP) {
		Run run;
		double answeredAt = now;

		for (int tick = 0; tick < ticks; tick++) {
			now += TICK;

			const double offered =
				std::min(static_cast<double>(control.AllowanceBytes()), static_cast<double>(cap));
			const double trip = path.Step(offered, TICK);

			control.OnRoundTrip(trip, 0.0, now);
			if (path.DroppedThisStep > 0) {
				control.OnLoss(path.DroppedThisStep, now);
			}

			// **What a `Link` reports, and reporting it every tick instead would
			// make every case here a lie.** The period closes when what was
			// outstanding at its start has been acknowledged, which is one round
			// trip on this path - so the controller sees roughly twenty ticks
			// between decisions here and one tick on a loopback.
			const bool answered = now - answeredAt >= trip;
			if (answered) {
				answeredAt = now;
			}

			control.Advance(TICK, cap, answered);

			run.PeakQueueSeconds = std::max(run.PeakQueueSeconds, path.QueueSeconds());
			run.Rates.push_back(control.RateBytesPerSecond());
		}

		run.FinalRateBytesPerSecond = control.RateBytesPerSecond();
		run.Reductions = control.Reductions();
		return run;
	}
}

// --- what a connection does before it knows anything -------------------------

TEST_CASE("a cold start opens at the initial window and not at the cap", "[net][congestion]") {
	CongestionControl control;

	// **The whole cold-start decision in two assertions.** The opening tick is
	// RFC 6928's initial window and nothing more - ten datagrams, once, which is
	// what every TCP connection on the internet opens with and enough that the
	// first snapshot chunks of a join move immediately.
	CHECK(control.AllowanceBytes() == 10 * 1200);
	CHECK(control.WindowBytes() == Approx(10 * 1200));
	CHECK(control.AllowanceBytes() < CAP);
	CHECK(control.InSlowStart());

	// And the tick after it is paced, because by then there is a round trip to
	// pace against - assumed until measured. Ten datagrams *every* tick until
	// the first acknowledgement would be sixty times the window it opened with.
	control.Advance(TICK, CAP, false);
	CHECK(control.AllowanceBytes() < 10 * 1200);
}

TEST_CASE("slow start doubles each answered period and stops at the cap", "[net][congestion]") {
	CongestionControl control;

	const double opened = control.RateBytesPerSecond();

	// One period, answered. The rate doubles rather than creeping: a joining
	// player pays for a linear search.
	control.Advance(TICK, CAP, true);
	CHECK(control.RateBytesPerSecond() == Approx(opened * 2.0));

	control.Advance(TICK, CAP, true);
	CHECK(control.RateBytesPerSecond() == Approx(opened * 4.0));

	for (int tick = 0; tick < 60; tick++) {
		control.Advance(TICK, CAP, true);
	}

	// It stops at the cap and it leaves slow start there - staying in it would
	// mean answering the first sign of a queue by halving from a rate the link
	// was never allowed to spend.
	CHECK(control.AllowanceBytes() <= CAP);
	CHECK_FALSE(control.InSlowStart());
}

TEST_CASE("slow start does not wait for a round trip to be measured", "[net][congestion]") {
	// A link whose reliable stream is quiet offers no sample. The ramp falls
	// back on the assumed round trip rather than sitting at the initial window
	// for ever on a path with nothing wrong with it.
	CongestionControl control;

	const double opened = control.RateBytesPerSecond();
	for (int tick = 0; tick < 7; tick++) {
		control.Advance(TICK, CAP, false);
	}

	CHECK(control.RateBytesPerSecond() > opened);
}

// --- the path pushing back ---------------------------------------------------

TEST_CASE("the rate backs off when the path starts queueing", "[net][congestion]") {
	CongestionControl control;
	Path path;

	const Run run = Drive(control, path, 600);

	// **It found the path.** Within a factor of two of the bottleneck in both
	// directions, which is the band a delay-based controller settles in - it
	// deliberately runs a little under capacity, which is what keeps the queue
	// empty.
	CHECK(run.FinalRateBytesPerSecond > path.CapacityBytesPerSecond * 0.5);
	CHECK(run.FinalRateBytesPerSecond < path.CapacityBytesPerSecond * 1.5);
}

TEST_CASE("the standing queue stays small and that is the whole point", "[net][congestion]") {
	CongestionControl control;
	Path path;

	Drive(control, path, 600);

	// **This is the bufferbloat assertion and it is the reason Copa was chosen
	// over a NewReno-style window.** A loss-based controller finds the
	// bottleneck by filling its buffer, so it would settle here with the full
	// 300 ms of queue on the path and add every millisecond of it to every
	// input the player sends. Copa settles at a couple of packets.
	CHECK(path.QueueSeconds() < 0.05);

	// And it did not merely take a while to get there.
	CHECK(control.QueueSeconds() < 0.05);
}

TEST_CASE("a deep buffer is answered without a single loss", "[net][congestion]") {
	CongestionControl control;
	Path path;
	path.BufferSeconds = 2.0;

	const Run run = Drive(control, path, 600);

	// The delay signal arrives long before the buffer overflows, which is what
	// "delay-based" means in practice: the controller has already found the
	// path by the time a loss-based one would have had its first signal.
	CHECK(run.Reductions == 0);
	CHECK(run.FinalRateBytesPerSecond > path.CapacityBytesPerSecond * 0.5);
}

TEST_CASE("it converges rather than oscillating", "[net][congestion]") {
	CongestionControl control;
	Path path;

	const Run run = Drive(control, path, 900);

	// Over the last two seconds the rate must sit inside a band rather than
	// hunting across one. **The velocity parameter reset on a direction change
	// is what buys this** - a controller that kept its momentum through a
	// reversal overshoots by as much as it had accelerated, and then overshoots
	// the other way.
	double lowest = run.Rates[run.Rates.size() - 120];
	double highest = lowest;
	for (size_t index = run.Rates.size() - 120; index < run.Rates.size(); index++) {
		lowest = std::min(lowest, run.Rates[index]);
		highest = std::max(highest, run.Rates[index]);
	}

	CHECK(highest / lowest < 1.5);
}

// --- loss --------------------------------------------------------------------

TEST_CASE("loss cuts the rate and one event is one cut", "[net][congestion]") {
	CongestionControl control;

	// Out of slow start and up at a rate worth cutting.
	for (int tick = 0; tick < 30; tick++) {
		control.Advance(TICK, CAP, true);
	}
	control.OnRoundTrip(0.05, 0.0, 1.0);
	control.Advance(TICK, CAP, true);

	const double before = control.RateBytesPerSecond();

	// A congestion event loses a burst, not one packet. Answering each of them
	// would cut by an order of magnitude for one event.
	control.OnLoss(1, 1.0);
	control.OnLoss(1, 1.001);
	control.OnLoss(8, 1.002);

	CHECK(control.Reductions() == 1);
	CHECK(control.RateBytesPerSecond() == Approx(before * control.Settings().LossBackoff));

	// A second event, one round trip later, is a second cut.
	control.OnLoss(1, 1.2);
	CHECK(control.Reductions() == 2);
}

TEST_CASE("loss ends the search rather than being ramped through", "[net][congestion]") {
	CongestionControl control;
	REQUIRE(control.InSlowStart());

	control.OnLoss(1, 0.5);

	// Slow start's premise is that nothing has pushed back yet, and a packet
	// that did not arrive is the plainest push-back there is.
	CHECK_FALSE(control.InSlowStart());
}

TEST_CASE("nothing can drive the window to zero", "[net][congestion]") {
	CongestionControl control;

	// A hundred separate congestion events, one every fifth of a second, with
	// nothing at all going right in between.
	for (int event = 0; event < 100; event++) {
		control.OnLoss(1, event * 0.2);
	}
	CHECK(control.Reductions() == 100);

	// **A controller that can reach zero and stay there is worse than no
	// controller**, because the link then has no way to discover that the path
	// recovered: it needs to send to get an acknowledgement and it needs an
	// acknowledgement to be allowed to send. Two datagrams is where it stops.
	CHECK(control.WindowBytes() == Approx(control.Settings().MinimumWindowBytes));

	control.OnRoundTrip(0.05, 0.0, 20.0);
	control.Advance(TICK, CAP, true);
	CHECK(control.AllowanceBytes() > 0);
}

TEST_CASE("a path that punishes everything is answered and not abandoned", "[net][congestion]") {
	CongestionControl control;

	// A bottleneck a two-hundredth of the cap with a shallow buffer, so both
	// signals arrive at once: the delay says back off and the overflow says it
	// again.
	Path path;
	path.CapacityBytesPerSecond = 20'000.0;
	path.BufferSeconds = 0.05;

	const Run run = Drive(control, path, 900);
	REQUIRE(run.Reductions > 0);

	// It came a long way down from the cap and it is still sending.
	CHECK(control.RateBytesPerSecond() < CAP * 60.0 / 4.0);
	CHECK(control.WindowBytes() >= control.Settings().MinimumWindowBytes);
	CHECK(control.AllowanceBytes() > 0);
}

TEST_CASE("it opens up again once the path clears", "[net][congestion]") {
	CongestionControl control;

	// A narrow path first.
	Path path;
	path.CapacityBytesPerSecond = 60'000.0;
	Drive(control, path, 900);

	const double narrow = control.RateBytesPerSecond();
	CHECK(narrow < path.CapacityBytesPerSecond * 1.5);

	// The same connection, on a path that is now ten times wider. Nothing tells
	// the controller - it has to find out.
	path.CapacityBytesPerSecond = 600'000.0;
	Drive(control, path, 1800, 15.0);

	// **Opening up is half the definition and it is the half a fixed cap can
	// never do.** A controller that only ever came down would leave a link that
	// had one bad minute permanently narrow. Copa is additive on the way up, so
	// what is asserted is that it got there rather than how fast.
	CHECK(control.RateBytesPerSecond() > narrow * 4.0);
}

// --- what it is not fooled by ------------------------------------------------

namespace {
	// Settles a controller on a 50 ms path, raises the round trip to 65 ms and
	// holds it there, answering with the least it allowed after the rise.
	//
	// The least rather than the last, because a round trip held at a fixed
	// number for four seconds is a path whose delay does not answer the window -
	// which the controller eventually and correctly reads as somebody else
	// holding the bottleneck. What this asks about is the response to the rise.
	uint32_t LeastAllowanceAfterRise(double varianceSeconds) {
		CongestionControl control;

		double now = 0.0;
		for (int tick = 0; tick < 240; tick++) {
			now += TICK;
			control.OnRoundTrip(0.05, varianceSeconds, now);
			control.Advance(TICK, CAP, true);
		}

		uint32_t least = control.AllowanceBytes();
		for (int tick = 0; tick < 120; tick++) {
			now += TICK;
			control.OnRoundTrip(0.065, varianceSeconds, now);
			control.Advance(TICK, CAP, true);
			least = std::min(least, control.AllowanceBytes());
		}

		return least;
	}
}

TEST_CASE("a rise inside the jitter is not read as a queue", "[net][congestion]") {
	// **A wireless link's round trip moves by tens of milliseconds with nothing
	// queued anywhere.** Told how much the path moves, the controller reads a
	// fifteen-millisecond rise as noise and stays up against the cap. It does
	// not land exactly on it: the cap is a per-tick number and a longer round
	// trip needs a wider window to spend it, so there are a few ticks of
	// catching up. That is arithmetic and not a back-off.
	CHECK(LeastAllowanceAfterRise(0.0075) > CAP * 3 / 4);
}

TEST_CASE("the same rise with no jitter to explain it is a queue", "[net][congestion]") {
	// The other half, and the reason `ReliableSender` grew a variance rather
	// than only a mean: told the path is steady, the controller has to read the
	// identical numbers as fifteen milliseconds of bottleneck, and it settles an
	// order of magnitude under the cap instead.
	CHECK(LeastAllowanceAfterRise(0.0) < CAP / 4);
}

// --- sharing the bottleneck with something that will not yield ---------------

TEST_CASE("a queue that never drains switches the mode", "[net][congestion]") {
	// **What a TCP download on the same bottleneck looks like from here.** It
	// fills the buffer and keeps it full, so the queueing delay never returns
	// to the floor however far this controller backs off. A pure delay-based
	// controller answers that by backing off to nothing.
	CongestionControl control;

	double now = 0.0;
	for (int tick = 0; tick < 60; tick++) {
		now += TICK;
		control.OnRoundTrip(0.04, 0.0, now);
		control.Advance(TICK, CAP, true);
	}
	REQUIRE_FALSE(control.Competing());
	const double polite = control.TargetQueuePackets();

	// The neighbour arrives and parks 200 ms of queue on the path - and holds
	// it there however far this end backs off, which is the whole of what makes
	// it a buffer-filler.
	for (int tick = 0; tick < 300; tick++) {
		now += TICK;
		control.OnRoundTrip(0.24, 0.0, now);
		control.Advance(TICK, CAP, true);
	}

	CHECK(control.Competing());
	CHECK(control.TargetQueuePackets() > polite);
}

TEST_CASE("the mode drops back when the neighbour leaves", "[net][congestion]") {
	CongestionControl control;

	double now = 0.0;
	for (int tick = 0; tick < 60; tick++) {
		now += TICK;
		control.OnRoundTrip(0.04, 0.0, now);
		control.Advance(TICK, CAP, true);
	}
	for (int tick = 0; tick < 300; tick++) {
		now += TICK;
		control.OnRoundTrip(0.24, 0.0, now);
		control.Advance(TICK, CAP, true);
	}
	REQUIRE(control.Competing());

	// The download finishes and the queue drains.
	for (int tick = 0; tick < 120; tick++) {
		now += TICK;
		control.OnRoundTrip(0.04, 0.0, now);
		control.Advance(TICK, CAP, true);
	}

	// **Latency is given up only for as long as somebody else is taking it
	// anyway.** A controller that kept the wider target would hold a queue
	// nothing was justifying any more.
	CHECK_FALSE(control.Competing());
	CHECK(control.TargetQueuePackets() == Approx(control.Settings().TargetQueuePackets));
}

// --- the settings ------------------------------------------------------------

TEST_CASE("congestion settings that cannot be used fall back", "[net][congestion]") {
	CongestionSettings broken;
	CHECK(broken.IsValid());

	broken.LossBackoff = 1.0;
	CHECK_FALSE(broken.IsValid());

	broken = CongestionSettings{};
	broken.TargetQueuePackets = 0.5;
	CHECK_FALSE(broken.IsValid());

	broken = CongestionSettings{};
	broken.MinimumWindowBytes = broken.InitialWindowBytes * 2.0;
	CHECK_FALSE(broken.IsValid());

	// The fallback is the same one `LinkSettings` uses, so a controller built
	// from nonsense is a working controller rather than a broken link.
	CongestionSettings nonsense;
	nonsense.MinimumQueueSeconds = -1.0;
	const CongestionControl control(nonsense);
	CHECK(control.Settings().MinimumQueueSeconds == Approx(CongestionSettings{}.MinimumQueueSeconds));
}

TEST_CASE("a stalled tick does not buy a burst", "[net][congestion]") {
	// A tick that took a second must not hand the connection a second's worth
	// of sending inside it, which is exactly the burst the controller exists to
	// avoid.
	CongestionControl control;
	control.Advance(5.0, CAP, true);

	CHECK(control.AllowanceBytes() <= CAP);
}
