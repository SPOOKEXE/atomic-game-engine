#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/net/Congestion.hpp>

#include <algorithm>

namespace engine::net {

	namespace {
		// A tick shorter than this is a repeated call rather than a tick, and a
		// tick longer than this is a stall. Neither may set the allowance: the
		// first would round the rate away and the second would let a connection
		// spend a quarter of a second's sending inside one tick, which is
		// exactly the burst the controller exists to avoid.
		constexpr double SHORTEST_TICK_SECONDS = 0.0005;
		constexpr double LONGEST_TICK_SECONDS = 0.25;

		// The bytes one packet contributes to the queue.
		//
		// **The payload size rather than the datagram size, because the budget
		// this feeds counts payload.** The path carries the header and the
		// framing on top, so the controller's idea of a packet is about two per
		// cent smaller than the real one - a systematic understatement, and it
		// errs toward less queueing rather than more.
		constexpr double PACKET_BYTES = static_cast<double>(Packet::MAXIMUM_PAYLOAD_BYTES);
	}

	bool CongestionSettings::IsValid() const {
		return InitialWindowBytes > 0.0 && MinimumWindowBytes > 0.0 &&
			   MinimumWindowBytes <= InitialWindowBytes && TargetQueuePackets >= 1.0 &&
			   MaximumQueuePackets >= TargetQueuePackets && MinimumQueueSeconds > 0.0 &&
			   VarianceFactor >= 0.0 && BaseWindowSeconds > 0.0 && LossBackoff > 0.0 && LossBackoff < 1.0 &&
			   VelocityRoundTrips > 0 && MaximumVelocity >= 1.0 && CompetitionRoundTrips > 0 &&
			   CompetitionRecoveryFraction > 0.0 && CompetitionRecoveryFraction < 1.0 &&
			   AssumedRoundTripSeconds > 0.0;
	}

	void CongestionControl::WindowedMinimum::Observe(double value, double nowSeconds, double windowSeconds) {
		if (!Filled) {
			Filled = true;
			Current = value;
			StartedAt = nowSeconds;
			return;
		}

		if (nowSeconds - StartedAt >= windowSeconds) {
			// The bucket that was being filled becomes the one being remembered,
			// which is what lets the minimum *rise* again. Without the roll a
			// single low sample would pin the base round trip for the life of
			// the connection, and a path that genuinely got slower would read as
			// a path with a permanent queue on it.
			Previous = Current;
			Rolled = true;
			Current = value;
			StartedAt = nowSeconds;
			return;
		}

		Current = std::min(Current, value);
	}

	double CongestionControl::WindowedMinimum::Value() const {
		if (!Filled) {
			return 0.0;
		}
		return Rolled ? std::min(Current, Previous) : Current;
	}

	CongestionControl::CongestionControl(const CongestionSettings &settings)
		: Paced(settings.IsValid() ? settings : CongestionSettings{}) {
		Window = Paced.InitialWindowBytes;
		TargetQueue = Paced.TargetQueuePackets;

		// **The opening tick may spend the whole initial window, and every tick
		// after it is paced.** That is what an initial *window* means: RFC 6928
		// allows ten segments back to back before anything has been
		// acknowledged, because there is no feedback yet to pace against. What
		// it does not allow is ten segments every tick until the feedback
		// arrives, which is what spreading the window over a guessed round trip
		// and then repeating it would come to.
		Allowance = static_cast<uint32_t>(Paced.InitialWindowBytes);
	}

	double CongestionControl::RateBytesPerSecond() const {
		return Window / (RoundTrip > 0.0 ? RoundTrip : Paced.AssumedRoundTripSeconds);
	}

	void CongestionControl::OnRoundTrip(double smoothedSeconds, double varianceSeconds, double nowSeconds) {
		if (!(smoothedSeconds > 0.0)) {
			// Zero means unknown rather than instant - `ReliableSender` says so
			// - and a controller that took it literally would compute an
			// infinite target rate on its first tick.
			return;
		}

		RoundTrip = smoothedSeconds;
		Variance = varianceSeconds >= 0.0 ? varianceSeconds : 0.0;

		Base.Observe(smoothedSeconds, nowSeconds, Paced.BaseWindowSeconds);

		// Copa's *standing* round trip is the least seen over half a trip, which
		// is short enough to be about the queue right now and long enough not to
		// be one sample's noise.
		Standing.Observe(smoothedSeconds, nowSeconds, smoothedSeconds * 0.5);
	}

	void CongestionControl::OnLoss(uint32_t packets, double nowSeconds) {
		if (packets == 0) {
			return;
		}

		const double trip = RoundTrip > 0.0 ? RoundTrip : Paced.AssumedRoundTripSeconds;
		if (HasCut && nowSeconds - LastCutAt < trip) {
			// Same congestion event. Counted nowhere and acted on nowhere: the
			// first packet of the burst already bought the reduction.
			return;
		}

		const double before = Window;

		HasCut = true;
		LastCutAt = nowSeconds;
		++Cuts;

		// **Loss ends the search, whatever else it does.** Slow start's whole
		// premise is that nothing has pushed back yet, and a packet that did not
		// arrive is the plainest push-back there is. Answering it by doubling
		// again on the next round trip is how a controller turns one congestion
		// event into a collapse.
		SlowStart = false;

		Window = std::max(Window * Paced.LossBackoff, Paced.MinimumWindowBytes);

		// **The velocity starts again from a congestion event, and without this
		// the cut does not stick.** A path that is losing packets with no
		// queueing delay to show for it - a policer, a shallow buffer, a
		// permanently full one whose round trip never varies - offers the delay
		// signal nothing to read, so the window law keeps saying "faster" and
		// accelerating while the loss keeps cutting. Measured on a 20 kB/s path
		// with a 50 ms buffer: at a velocity of 64 the increase is more than the
		// reduction and the window rides the cap for ever.
		Velocity = 1.0;
		Direction = -1;
		SameDirectionRoundTrips = 0;

		if (Competitive) {
			// The multiplicative half of the AIMD the competitive mode is
			// playing. Outside that mode the target queue is a constant and loss
			// is answered by the window cut alone - a delay-based controller that
			// also shrank its target on every loss would ratchet itself down on a
			// path that merely has a shallow buffer.
			TargetQueue = std::max(Paced.TargetQueuePackets, TargetQueue * 0.5);
		}

		core::Metrics::Count("net.congestion.loss", 1.0);

		// **The window either side of the cut, which the counter cannot carry.**
		// "The connection got slow" is answered by how far the window fell and
		// how often, and rate-limiting keeps a lossy path from being the log.
		ENGINE_DEBUG_EVERY(
			1.0, "loss on {} packet(s): window {} -> {} bytes, cut {}", packets, before, Window, Cuts
		);
	}

	void CongestionControl::Advance(double elapsedSeconds, uint32_t ceilingBytes, bool answered) {
		const double elapsed = std::clamp(elapsedSeconds, SHORTEST_TICK_SECONDS, LONGEST_TICK_SECONDS);

		// **Everything periodic here is per round trip, and the assumed one
		// stands in until a real one lands.** The doubling, the velocity
		// parameter and the mode switch are all waiting for the effect of the
		// last decision to come back, and that takes a round trip. A controller
		// that ran them per tick would behave differently at 30 Hz and at 144 Hz
		// on the same path.
		const double trip = RoundTrip > 0.0 ? RoundTrip : Paced.AssumedRoundTripSeconds;

		// The cap arrives per tick and the law runs on a window, so it is turned
		// into the window that would exactly spend it - anything else and the
		// two disagree about what a tick is.
		const double ceiling =
			std::max(static_cast<double>(ceilingBytes) * trip / elapsed, Paced.MinimumWindowBytes);

		// Ordinary measurement noise is not a queue, and neither is jitter. A
		// threshold of zero backs off for ever on a wireless link.
		const double noise = std::max(Paced.MinimumQueueSeconds, Paced.VarianceFactor * Variance);
		Queue = RoundTrip > 0.0 ? std::max(0.0, Standing.Value() - Base.Value()) : 0.0;

		const bool drained = Queue <= noise;

		if (SlowStart) {
			// **Nothing measured is not the same as nothing happening, and the
			// search does not wait for a round trip to be measured.** A link
			// whose reliable stream is quiet offers no sample, and a controller
			// that sat at the initial window until one arrived would leave a
			// perfectly good path unused for as long as the game had nothing
			// that had to arrive.
			SinceObservation += elapsed;
			if (answered || SinceObservation >= trip) {
				SinceObservation = 0.0;
				Window *= Paced.SlowStartGrowth;
			}

			if (!drained) {
				// The path pushed back. Give up the last doubling rather than
				// the whole search - the window that produced the queue is
				// roughly the path's, and Copa steers from here.
				SlowStart = false;
				Window /= Paced.SlowStartGrowth;
				core::Metrics::Count("net.congestion.slowstart.ended", 1.0);
				ENGINE_DEBUG("slow start ended on a queue of {}s; window {} bytes", Queue, Window);
			}

			Window = std::clamp(Window, Paced.MinimumWindowBytes, ceiling);
			if (Window >= ceiling) {
				// The cap is reached, so there is nothing left to search for.
				// Staying in slow start would mean answering the first sign of a
				// queue by halving from a window the link was never spending.
				//
				// This exit had neither a counter nor a line, and it is the one
				// that fires on a link whose configured budget is the limit
				// rather than the path.
				SlowStart = false;
				ENGINE_DEBUG("slow start reached the {} byte ceiling; the budget is the limit", ceiling);
			}

			Allowance = static_cast<uint32_t>(Window * elapsed / trip);
			return;
		}

		// Copa steers the *rate* toward `TargetQueuePackets / queueing delay`
		// and the rate a window comes to is the window over the round trip.
		// Below the noise threshold there is no queue to divide by and the
		// answer is "faster", which is what the formula says as the delay goes
		// to zero anyway.
		const bool below = drained || Window / trip < TargetQueue * PACKET_BYTES / Queue;
		const int direction = below ? 1 : -1;

		// Copa's law is `cwnd += velocity / (delta * cwnd)` packets per
		// acknowledgement, which over one round trip is a constant
		// `velocity / delta` packets however large the window is. `1/delta` is
		// `TargetQueuePackets`, and this module advances in seconds rather than
		// in round trips.
		const double step = Velocity * TargetQueue * PACKET_BYTES * elapsed / trip;

		Window = std::clamp(Window + direction * step, Paced.MinimumWindowBytes, ceiling);

		SinceObservation += elapsed;
		if (answered || SinceObservation >= trip) {
			SinceObservation = 0.0;

			if (direction == Direction) {
				++SameDirectionRoundTrips;
			} else {
				// **A reversal resets the velocity, and that is what makes this
				// converge rather than oscillate.** A controller that kept its
				// momentum through a direction change overshoots by exactly as
				// much as it had accelerated, and then overshoots the other way.
				Direction = direction;
				SameDirectionRoundTrips = 1;
				Velocity = 1.0;
			}

			if (SameDirectionRoundTrips > Paced.VelocityRoundTrips) {
				Velocity = std::min(Velocity * 2.0, Paced.MaximumVelocity);
			}

			// **The mode switch asks a different question from the window law
			// and therefore reads a different thing.** The window law wants to
			// know whether there is a queue worth dividing by. This wants to
			// know whether the queue is *this flow's* - and the answer is
			// whether it followed the window down. A period spent steering down
			// with the delay unmoved is a period somebody else was holding the
			// bottleneck.
			const bool answeredTheBackoff = drained || direction > 0 || Queue < QueueWhenPeriodOpened;
			QueueWhenPeriodOpened = Queue;

			if (answeredTheBackoff) {
				UndrainedRoundTrips = 0;
			} else {
				++UndrainedRoundTrips;
			}

			const bool wasCompetitive = Competitive;
			if (!Competitive && UndrainedRoundTrips >= Paced.CompetitionRoundTrips) {
				Competitive = true;
				CompetitionQueueSeconds = Queue;
			} else if (Competitive &&
					   Queue <=
						   std::max(noise, Paced.CompetitionRecoveryFraction * CompetitionQueueSeconds)) {
				// **The exit is stated against what let the mode in**, not
				// against an absolute delay. A path that was 200 ms deep and is
				// now 20 ms has plainly lost whatever was filling it; a path
				// that was always 20 ms deep never entered.
				Competitive = false;
			}

			if (Competitive) {
				// The additive half of the AIMD. One packet of tolerated queue
				// per round trip is the same increase law the flow holding the
				// bottleneck full is playing, which is the whole point: a
				// delay-based controller that will not play it takes no share at
				// all.
				TargetQueue = std::min(TargetQueue + 1.0, Paced.MaximumQueuePackets);
			} else if (wasCompetitive) {
				// The neighbour left. Give the latency back rather than keeping
				// a queue that was only ever justified by somebody else's.
				TargetQueue = Paced.TargetQueuePackets;
				core::Metrics::Count("net.congestion.competitive.ended", 1.0);
				ENGINE_DEBUG("competitive mode ended; target queue back to {} packets", TargetQueue);
			}

			if (Competitive && !wasCompetitive) {
				core::Metrics::Count("net.congestion.competitive.began", 1.0);
				// A materially different sending regime, and the reason a path
				// suddenly tolerates latency it did not a moment ago.
				ENGINE_DEBUG("competitive mode began after {} undrained round trips", UndrainedRoundTrips);
			}
		}

		Allowance = static_cast<uint32_t>(Window * elapsed / trip);
	}
}
