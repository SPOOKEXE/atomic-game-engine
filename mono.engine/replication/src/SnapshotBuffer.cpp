#include <engine/ecs/Store.hpp>
#include <engine/replication/SnapshotBuffer.hpp>

#include <algorithm>

namespace engine::replication {

	namespace {
		// How long the measured rate averages over. Long enough that a pause
		// moves it by a few percent rather than halving it, short enough that
		// it still describes this connection.
		constexpr double RATE_WINDOW_SECONDS = 8.0;

		// How much has to have happened before a ratio means anything. A
		// quarter of a second is seven ticks at 30 Hz and fifteen at 60, which
		// is enough to tell those two apart by a mile.
		constexpr double RATE_MINIMUM_SECONDS = 0.25;
		constexpr double RATE_MINIMUM_TICKS = 4.0;
	}

	SnapshotBuffer::SnapshotBuffer(const InterpolationSettings &settings) : Settings_(settings) {
		// A history of nothing would leave `Sample` with no pose to hold and a
		// world that never drew, and a history of one can never bracket. Two is
		// the floor at which this class does its job at all.
		Settings_.HistoryTicks = std::max<size_t>(Settings_.HistoryTicks, 2);
	}

	const SnapshotBuffer::Pose &SnapshotBuffer::Oldest(const Track &track, size_t index) {
		const size_t capacity = track.Ring.size();
		return track.Ring[(track.Head + capacity - track.Count + index) % capacity];
	}

	bool SnapshotBuffer::Holds(uint64_t tick) const {
		return Started && tick <= Newest_;
	}

	void SnapshotBuffer::Prune() {
		// Anything older than the history kept can no longer bracket a render
		// position the clock is allowed to reach, so it is a track for an entity
		// that was destroyed, forgotten, or left this client's interest. Without
		// this the map grows for the life of the connection.
		const uint64_t horizon = Settings_.HistoryTicks;
		if (Newest_ <= horizon) {
			return;
		}

		const uint64_t cutoff = Newest_ - horizon;
		for (auto entry = Tracks.begin(); entry != Tracks.end();) {
			const Track &track = entry->second;
			if (track.Count > 0 && Oldest(track, track.Count - 1).Tick < cutoff) {
				entry = Tracks.erase(entry);
			} else {
				++entry;
			}
		}
	}

	void SnapshotBuffer::Record(uint64_t tick, ecs::Entity entity, const core::CFrame &frame) {
		if (tick == 0) {
			// Zero is what `Replica::Applied` reads before the joining snapshot
			// has landed, so it names no state at all.
			return;
		}

		// **The two exclusions, and they are the same rule stated twice.** An
		// entity in the predicted range was minted by this client and the server
		// has never heard of it; the nominated one is the local player, which is
		// run ahead precisely so the input does not feel delayed. Delaying either
		// is the lag prediction exists to remove, put straight back.
		if (entity == Predicted_ || ecs::Store::IsPredicted(entity)) {
			return;
		}

		if (!Started || tick > Newest_) {
			if (!Started) {
				// The clock starts a full delay behind, rather than at the tick
				// and easing back: easing from zero means the first second of a
				// connection is the world running slow, which is the first second
				// anybody looks at.
				RenderTicks = static_cast<double>(tick) - Settings_.DelayTicks;
				Started = true;
			}

			// **The rate window closes on a tick boundary, and both halves of it
			// move here.** Seconds accumulate every frame and ticks only arrive
			// four times as rarely, so a ratio taken part way through a tick is
			// systematically low — enough to read a steady 60 Hz stream as 56 and
			// draw the world in slow motion for it.
			if (Newest_ != 0) {
				// Counted as the ticks that passed rather than as arrivals: a
				// lost tick still happened on the server, and a rate that counted
				// only what landed would read a lossy link as a slow one.
				ObservedTicks += static_cast<double>(tick - Newest_);
				ObservedSeconds += Elapsed - ElapsedAtTick;
			}
			ElapsedAtTick = Elapsed;

			if (ObservedSeconds > RATE_WINDOW_SECONDS) {
				// Halving both keeps the ratio and drops the oldest half of the
				// window's influence, which is a sliding average without a ring of
				// samples to keep.
				ObservedSeconds *= 0.5;
				ObservedTicks *= 0.5;
			}

			Newest_ = tick;
			Stats_.Ticks++;
			Prune();
		}

		Track &track = Tracks[entity];
		if (track.Ring.empty()) {
			track.Ring.resize(Settings_.HistoryTicks);
		}

		// A tick already held is a duplicate poll rather than new state. Writing
		// it again would push the pose that brackets the render position out of
		// the ring, which is a stall caused by nothing having happened.
		if (track.Count > 0 && Oldest(track, track.Count - 1).Tick >= tick) {
			return;
		}

		track.Ring[track.Head] = Pose{tick, frame};
		track.Head = (track.Head + 1) % track.Ring.size();
		track.Count = std::min(track.Count + 1, track.Ring.size());
	}

	void SnapshotBuffer::Advance(double frameSeconds) {
		if (!Started) {
			return;
		}

		// A clock that went backwards is the caller's problem and not this
		// class's to amplify: a negative step would drag the render position back
		// through poses it has already drawn.
		frameSeconds = std::max(frameSeconds, 0.0);

		// Only the running total moves here. What the rate is measured over is
		// closed off in `Record`, on a tick boundary.
		Elapsed += frameSeconds;

		const double newest = static_cast<double>(Newest_);
		double behind = newest - RenderTicks;

		// **One-sided, because the other side cannot happen.** The clamp at the
		// bottom of this function keeps the render position at or before the
		// newest sample, so `behind` is never negative and a clock running *too
		// far ahead* is not a state this can reach. Past this much lag the
		// connection paused rather than jittered, and easing back would be
		// seconds of a world that is visibly late.
		if (behind > Settings_.DelayTicks + Settings_.ResyncTicks) {
			RenderTicks = newest - Settings_.DelayTicks;
			behind = Settings_.DelayTicks;
			Stats_.Resyncs++;
		}

		// **The deadband is one tick wide either side, and it is not slack.**
		// Ticks arrive one at a time, so `behind` is a sawtooth by construction:
		// it jumps by one the instant a tick lands and falls back by one over the
		// period before the next. A correction that fired inside that swing would
		// be chasing the staircase rather than the drift, and the world would
		// speed up and slow down once per tick — which is the judder this class
		// exists to remove, rebuilt out of the cure.
		double rate = 1.0;
		if (behind > Settings_.DelayTicks + 1.0) {
			// The link is delivering faster than the clock is consuming, so the
			// world is drawn later than it needs to be. Run a little fast.
			rate = 1.0 + Settings_.CorrectionFraction;
		} else if (behind < Settings_.DelayTicks - 1.0) {
			// The budget is being eaten. Run a little slow, which spends less of
			// it per frame and buys the late packet time to arrive.
			rate = 1.0 - Settings_.CorrectionFraction;
		}

		RenderTicks += frameSeconds * MeasuredTickRate() * rate;

		// **No extrapolation.** Past the newest sample there is nothing to
		// interpolate toward; guessing produces a pose the server never sent and
		// then a snap when the next tick disagrees with the guess. The clock
		// stops, the world holds, and the counter says so — a stated behaviour
		// rather than an invented one.
		if (RenderTicks > newest) {
			RenderTicks = newest;
			Stats_.Stalls++;
		}
	}

	std::optional<core::CFrame> SnapshotBuffer::Sample(ecs::Entity entity) {
		if (entity == Predicted_ || ecs::Store::IsPredicted(entity)) {
			return std::nullopt;
		}

		const auto found = Tracks.find(entity);
		if (found == Tracks.end() || found->second.Count == 0) {
			return std::nullopt;
		}

		const Track &track = found->second;

		// Before the oldest pose held, or after the newest: there is one end to
		// hold onto and nothing to interpolate with. The first happens on the
		// frames right after an entity comes into view, the second is the stall
		// `Advance` counts.
		const Pose &oldest = Oldest(track, 0);
		if (RenderTicks <= static_cast<double>(oldest.Tick)) {
			Stats_.Held++;
			return oldest.Frame;
		}

		const Pose &newest = Oldest(track, track.Count - 1);
		if (RenderTicks >= static_cast<double>(newest.Tick)) {
			Stats_.Held++;
			return newest.Frame;
		}

		// The two that bracket it. Walked from the newest end because the render
		// position is normally within a couple of ticks of it, and the history is
		// short enough that a binary search would cost more than it saved.
		for (size_t index = track.Count - 1; index > 0; index--) {
			const Pose &after = Oldest(track, index);
			const Pose &before = Oldest(track, index - 1);
			if (RenderTicks < static_cast<double>(before.Tick)) {
				continue;
			}

			// The span is whatever arrived, not one tick: a lost or late tick
			// leaves a gap and the two either side of it are still the right
			// pair to interpolate between.
			const double span = static_cast<double>(after.Tick) - static_cast<double>(before.Tick);
			const double alpha = (RenderTicks - static_cast<double>(before.Tick)) / span;

			Stats_.Interpolated++;

			// NLerp rather than Lerp, for the reason `CFrame` gives: consecutive
			// received ticks are a few degrees apart at most, where the two agree
			// to well under a pixel and one of them costs an `acos`.
			return before.Frame.NLerp(after.Frame, static_cast<float>(alpha));
		}

		// Unreachable while the two guards above hold, and cheaper to answer than
		// to assert: the oldest pose is the honest fallback for a render position
		// that is somehow before everything held.
		Stats_.Held++;
		return oldest.Frame;
	}

	void SnapshotBuffer::Predict(ecs::Entity entity) {
		Predicted_ = entity;
		Tracks.erase(entity);
	}

	void SnapshotBuffer::Forget(ecs::Entity entity) {
		Tracks.erase(entity);
	}

	void SnapshotBuffer::Clear() {
		Tracks.clear();
		RenderTicks = 0.0;
		Newest_ = 0;
		Started = false;
		ObservedTicks = 0.0;
		ObservedSeconds = 0.0;
		Elapsed = 0.0;
		ElapsedAtTick = 0.0;
	}

	double SnapshotBuffer::MeasuredTickRate() const {
		if (ObservedSeconds < RATE_MINIMUM_SECONDS || ObservedTicks < RATE_MINIMUM_TICKS) {
			return Settings_.TickRate;
		}
		return ObservedTicks / ObservedSeconds;
	}

	double SnapshotBuffer::Behind() const {
		if (!Started) {
			return 0.0;
		}
		return static_cast<double>(Newest_) - RenderTicks;
	}
}
