#pragma once

// The send rate as a function of what the path is doing.
//
// `LinkSettings::BytesPerTick` is a fixed cap, and a fixed cap is not congestion
// control: it does not back off when the path is congested and it does not open
// up when it is not, so on a real internet path it is either wasting the link or
// contributing to a collapse it cannot detect. This is the part that reacts.
//
// **The algorithm is Copa** - Arun and Balakrishnan, *Copa: Practical
// Delay-Based Congestion Control for the Internet*, NSDI 2018 - and the reason
// is that for a game the latency argument is the whole point rather than a side
// note. Copa aims at a *standing queue of `1/delta` packets* at the bottleneck
// and steers the rate toward `1/(delta * queueing delay)`, so the equilibrium is
// a nearly empty buffer rather than a full one. That number is spelled
// `TargetQueuePackets` here, because `1/delta` is literally the packets of queue
// the controller settles at and a name that says so is worth more than a Greek
// letter.
//
// **What was rejected, and why, so it is not re-argued.** A loss-based AIMD
// window in the NewReno lineage is simple, fair to TCP and thoroughly
// understood, and it *finds the bottleneck by filling its buffer* - that is not
// an implementation detail, it is the mechanism. On a home router with a
// hundred milliseconds of buffer that is a hundred milliseconds added to every
// input the player sends, arriving exactly when the game is busiest. Vegas is
// the other delay-based option and Copa is strictly the better of the two: it
// has the same equilibrium argument and it has an answer for the case Vegas is
// famous for losing, which is sharing a bottleneck with a buffer-filling flow.
// BBR trades better than either but wants per-packet delivery-rate sampling and
// a pacing engine, neither of which this transport has and both of which are
// more machinery than the problem is worth here.
//
// **What it does against a TCP download on the same bottleneck.** A pure
// delay-based controller is starved: it backs off the moment the queue grows,
// the loss-based flow does not back off until the queue overflows, and the
// delay-based flow's share converges toward nothing. Copa's answer is the
// *competitive mode* and it is implemented here. The controller watches whether
// the queue ever drains; if it has not drained for `CompetitionRoundTrips`, the
// only explanation is a flow that keeps it full, and `TargetQueuePackets` then
// moves AIMD-style - one packet added per round trip, halved on a loss - which
// is the same law the neighbour is playing and takes a comparable share. When
// the neighbour leaves the queue drains again, the mode drops back, and the
// latency comes back with it. **Latency is given up only for as long as
// somebody else is taking it anyway.**
//
// **Nothing here is random and nothing here reads a clock.** Copa needs no
// jitter, so there is no seeded source to justify. Time arrives as an argument
// exactly as it does everywhere else in this module, which is what keeps the
// controller's whole state a pure function of the sequence of calls it was
// given - the same property the idle timeout has, and the reason a suite states
// a congestion event rather than waiting for one.
//
// @tier L11 · shared

#include <engine/net/Packet.hpp>

#include <cstdint>

namespace engine::net {

	// How hard the controller pushes, and how much queue it will tolerate.
	//
	// The defaults come from the Copa paper and from RFC 6928 rather than from a
	// measurement taken here, and saying so is better than implying otherwise -
	// the same standing `LinkSettings` and `ReliabilitySettings` have.
	//
	// @since v0.15
	struct CongestionSettings {
		// Bytes a connection may have in flight before it has measured anything.
		//
		// **A cold start that blasts is a bad neighbour and one that crawls
		// makes a joining player wait**, so this is RFC 6928's initial window
		// exactly - ten full datagrams. A number with fifteen years of
		// deployment behind it rather than one chosen here, and small enough
		// that it cannot hurt a path nothing has measured: at the assumed 100 ms
		// it is under a megabit a second, and it is still enough that the first
		// snapshot chunks of a join move on the first tick instead of the fifth.
		//
		// It is where the search *starts* and not where it stays: see
		// `CongestionControl::InSlowStart`.
		double InitialWindowBytes = 10.0 * 1200.0;

		// What slow start multiplies the window by each round trip.
		//
		// **Doubling, because the alternative to an exponential search is a
		// linear one and a joining player pays for it.** Copa's velocity
		// parameter gets there too and takes three round trips to start, which
		// is three round trips of a join crawling on a path that had nothing
		// wrong with it. This is TCP's answer to the same question and it is the
		// right one for the same reason.
		double SlowStartGrowth = 2.0;

		// The window it will not go below, however bad the path looks.
		//
		// **A controller that can reach zero and stay there is worse than no
		// controller**, because the link then has no way to discover that the
		// path recovered - it needs to send something to get an acknowledgement
		// and it needs an acknowledgement to be allowed to send. Two datagrams
		// is TCP's own floor and it is here for TCP's reason.
		double MinimumWindowBytes = 2.0 * 1200.0;

		// Packets of standing queue the controller settles at.
		//
		// Copa's `1/delta`, and the whole latency argument in one number: two
		// packets at a 1 Mbit/s bottleneck is about 19 ms of buffering, against
		// the hundreds a loss-based controller parks there. Raising it trades
		// latency for throughput and the competitive mode already raises it when
		// something else is doing so anyway.
		double TargetQueuePackets = 2.0;

		// The ceiling the competitive mode may raise `TargetQueuePackets` to.
		//
		// Without a ceiling the AIMD law has nothing to stop it, and a
		// controller that has decided it is competing would keep climbing on a
		// path that is simply slow to drain.
		double MaximumQueuePackets = 64.0;

		// Queueing delay below which the path counts as uncongested.
		//
		// A round trip is never measured to the microsecond, so a threshold of
		// zero would read ordinary measurement noise as a queue and back off on
		// a path with nothing on it.
		double MinimumQueueSeconds = 0.001;

		// Multiples of the round-trip variance that also count as noise.
		//
		// **The jitter-aware half of the threshold above.** A path whose round
		// trip swings by fifteen milliseconds from one sample to the next is not
		// a path with fifteen milliseconds of queue on it, and a controller that
		// cannot tell the two apart backs off for ever on a wireless link.
		double VarianceFactor = 2.0;

		// How far back the base round trip - the path with no queue on it - is
		// remembered.
		//
		// Long enough that a queue which stays full for several seconds does not
		// become the new baseline, short enough that a route change is forgotten
		// rather than making every later sample look congested.
		double BaseWindowSeconds = 10.0;

		// What one loss event multiplies the window by.
		//
		// 0.7 rather than a half, which is CUBIC's figure and the reason is the
		// same: halving is a very deep cut to recover from at a game's tick rate,
		// and the delay signal has usually already started the back-off before
		// the first loss arrives.
		double LossBackoff = 0.7;

		// Round trips the window must move the same way before it speeds up.
		//
		// Copa's velocity parameter. Three is the paper's figure and it is what
		// makes the controller converge rather than oscillate: a direction that
		// keeps reversing never earns the doubling, so the window settles instead
		// of hunting.
		uint32_t VelocityRoundTrips = 3;

		// The most the velocity parameter may double to.
		//
		// It is what makes the cold start exponential, and an unbounded
		// exponential is how a controller overshoots a fat path by an order of
		// magnitude and then has to find its way back.
		double MaximumVelocity = 64.0;

		// How far the queue must fall for the competitive mode to end.
		//
		// A fraction of what it was when the mode began, so the exit is stated
		// against the thing that caused it. Half is a clear departure rather
		// than a fluctuation.
		double CompetitionRecoveryFraction = 0.5;

		// Round trips of backing off with no answer before the mode switches.
		//
		// **The test is whether the queue responds to this flow, and that is a
		// restatement of Copa's rather than a departure from it.** The paper
		// asks whether the queue is ever nearly empty, which works because its
		// per-acknowledgement window oscillates hard enough to empty it; a
		// window steered once a tick settles at its target instead and never
		// empties anything, so that form of the test calls every ordinary path
		// contested. The question underneath is the same one and it survives the
		// discretisation: **if this end reduces its window round trip after
		// round trip and the queueing delay does not follow it down, the queue
		// is not this flow's.**
		//
		// Five round trips, which is Copa's figure. Shorter and an ordinary
		// overshoot reads as competition; longer and a real TCP neighbour
		// starves this link for a second first.
		uint32_t CompetitionRoundTrips = 5;

		// The round trip assumed before one has been measured.
		//
		// Everything the controller does is per round trip, so before one is
		// measured there has to be a number to be per. It is an assumption and
		// it is replaced by the first acknowledgement that closes a trip.
		double AssumedRoundTripSeconds = 0.1;

		// Whether these can be used. Requires positive windows with the minimum
		// under the initial, a target queue of at least one packet no larger
		// than the ceiling, a back-off and an empty-queue fraction strictly
		// inside `(0, 1)`, and non-zero periods. Anything else falls back to the
		// defaults, as `LinkSettings` does.
		bool IsValid() const;
	};

	// The send rate, steered by the round trip and by loss.
	//
	// One per `Link`, owned by it. It reads no clock and holds no transport: the
	// link feeds it what it already knows - a round-trip estimate whenever one
	// lands, a count of sends the far side's acknowledgement showed missing, and
	// one `Advance` per tick - and reads back a byte allowance.
	//
	// **`Advance` is the only place the control law runs, and that is rule 5.**
	// Observations may arrive several times in a tick because several packets
	// may arrive in a tick; they accumulate into windowed minima, which are
	// order-independent. The decision that turns them into a rate happens once,
	// at the point in the tick the link already advances its timeouts.
	//
	// @since v0.15
	class CongestionControl {
	  public:
		// @param settings How hard to push. An invalid set falls back to the
		//        defaults, as `LinkSettings` does.
		explicit CongestionControl(const CongestionSettings &settings = {});

		// The settings in use, after the validity fallback.
		const CongestionSettings &Settings() const {
			return Paced;
		}

		// Feeds one round-trip estimate.
		//
		// **This is the existing measurement and not a second one.**
		// `ReliableSender` samples the gap between a reliable packet going out
		// and the acknowledgement that retires it, under Karn's rule, and
		// `Link::RecordRoundTrip` is where that already arrives. Nothing new
		// crosses the wire to feed this.
		//
		// @param smoothedSeconds The smoothed estimate. Ignored when not
		//        positive, which is what "nothing measured yet" reads as.
		// @param varianceSeconds The estimate's variance, RFC 6298's `RTTVAR`.
		//        Negative is read as unknown and the noise threshold then falls
		//        back to `MinimumQueueSeconds` alone.
		// @param nowSeconds The current time.
		void OnRoundTrip(double smoothedSeconds, double varianceSeconds, double nowSeconds);

		// Feeds sends the far side's acknowledgement showed missing.
		//
		// **At most one reduction per round trip, whatever the count.** A single
		// congestion event loses a burst of packets, not one, and a controller
		// that cut its rate once per lost packet would cut it by an order of
		// magnitude for one event and then spend seconds climbing back.
		//
		// @param packets How many went missing. Zero does nothing.
		// @param nowSeconds The current time.
		void OnLoss(uint32_t packets, double nowSeconds);

		// Runs the control law for one tick.
		//
		// @param elapsedSeconds Wall time since the previous call, which is what
		//        turns a rate into an allowance. Clamped, because a stalled tick
		//        must not buy the connection a second's worth of sending inside
		//        one tick.
		// @param ceilingBytes The most this tick may allow, whatever the path
		//        would take - `LinkSettings::BytesPerTick`. The controller will
		//        not steer above it, and that is **anti-windup rather than
		//        cosmetics**: a rate allowed to climb far past a cap it can never
		//        spend would take seconds to come back down after real
		//        congestion, and every one of those seconds is a link ignoring
		//        the path.
		// @param answered Whether everything outstanding when the current
		//        observation period opened has now been acknowledged.
		//        **This is what clocks the controller, and the round trip is only
		//        the fallback.** Every periodic decision here - the doubling, the
		//        velocity, the mode switch - is waiting for the effect of the
		//        last one to come back, and an acknowledgement *is* that effect
		//        arriving. A controller clocked by an assumed round trip instead
		//        behaves the same on a loopback as on a satellite, which is
		//        wrong on both.
		// @tick
		void Advance(double elapsedSeconds, uint32_t ceilingBytes, bool answered);

		// Payload bytes the path looks able to take in the coming tick.
		//
		// The window spread over the round trip and then over one tick. **The
		// exception is the opening tick, which may spend the whole initial
		// window** - that is what an initial window is for, and it is the one
		// burst on a connection that has no feedback to pace against.
		uint32_t AllowanceBytes() const {
			return Allowance;
		}

		// Payload bytes this end may have unacknowledged on the path.
		//
		// **The window is what is steered and the rate is what falls out of
		// it**, which is Copa's shape and not a presentational choice. A window
		// carries its own negative feedback: when the bottleneck queues, the
		// round trip grows and the same window sends more slowly without the
		// controller having decided anything. An open-loop rate has none of
		// that, and it overshoots by however long the feedback takes to arrive.
		double WindowBytes() const {
			return Window;
		}

		// The rate that window comes to, in payload bytes per second.
		//
		// The window over the round trip, or over the assumed one before a trip
		// has been measured.
		double RateBytesPerSecond() const;

		// The standing queue the path looks to be holding, in seconds.
		//
		// The difference between the recent round trip and the least this
		// connection has seen. Zero on a path with nothing queued on it.
		double QueueSeconds() const {
			return Queue;
		}

		// The round trip with no queue on it, in seconds, or zero before the
		// first sample.
		double BaseRoundTripSeconds() const {
			return Base.Value();
		}

		// Whether the connection is still searching for the path's capacity.
		//
		// **A new connection has no idea what the path will carry and cannot
		// find out by reasoning**, so it doubles its window every round trip from
		// RFC 6928's initial window until something pushes back - a queue that
		// stops draining, a lost packet, or the cap. That is TCP's slow start
		// and it is here for TCP's reason: the search has to be exponential or
		// the first seconds of every connection are spent below the path.
		//
		// Copa takes over the moment it ends, and it does not start again. A
		// controller that re-entered slow start after every congestion event
		// would be a controller that answers congestion by doubling.
		bool InSlowStart() const {
			return SlowStart;
		}

		// Whether the queue has stopped draining and the mode has switched.
		//
		// True means something else is holding the bottleneck full and this
		// controller has stopped being polite about it. See the file comment.
		bool Competing() const {
			return Competitive;
		}

		// Packets of standing queue currently aimed at.
		//
		// `CongestionSettings::TargetQueuePackets` until the competitive mode
		// raises it.
		double TargetQueuePackets() const {
			return TargetQueue;
		}

		// Times the rate has been cut by a loss event, over this controller's
		// life.
		uint64_t Reductions() const {
			return Cuts;
		}

	  private:
		// A minimum over roughly the last window, in two buckets.
		//
		// **Two buckets rather than a history of samples**, because the only
		// question asked of it is "what is the least this has been lately" and a
		// ring of samples would be per-connection memory and a scan to answer
		// exactly that. The cost is that the window it actually covers is
		// between one and two of the stated length, which is the same
		// approximation every windowed-minimum filter in a congestion controller
		// makes and is on the safe side: an over-long window keeps the base
		// round trip low, which reads as *more* queue rather than less.
		struct WindowedMinimum {
			double Current = 0.0;
			double Previous = 0.0;
			double StartedAt = 0.0;
			bool Filled = false;
			bool Rolled = false;

			void Observe(double value, double nowSeconds, double windowSeconds);
			double Value() const;
		};

		CongestionSettings Paced;

		double Window = 0.0;
		double Queue = 0.0;
		double RoundTrip = 0.0;
		double Variance = 0.0;
		double TargetQueue = 0.0;

		WindowedMinimum Base;
		WindowedMinimum Standing;

		// Copa's velocity parameter and the direction it is tracking. Positive
		// is speeding up, negative is slowing down, zero is "no decision yet".
		double Velocity = 1.0;
		int Direction = 0;
		uint32_t SameDirectionRoundTrips = 0;

		// The round-trip-length observation period the velocity and the mode
		// switch are both defined over. Accumulated in seconds rather than
		// counted in ticks, because the tick rate is not this module's to know.
		double SinceObservation = 0.0;
		bool SlowStart = true;
		uint32_t UndrainedRoundTrips = 0;
		bool Competitive = false;

		// The queue when the competitive mode began, which the recovery is
		// measured against.
		double CompetitionQueueSeconds = 0.0;

		// The queue at the start of the current observation period, which is
		// what "the queue followed the window down" is judged against.
		double QueueWhenPeriodOpened = 0.0;

		double LastCutAt = 0.0;
		bool HasCut = false;
		uint64_t Cuts = 0;

		uint32_t Allowance = 0;
	};
}
