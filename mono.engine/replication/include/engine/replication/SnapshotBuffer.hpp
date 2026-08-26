#pragma once

// Received ticks, held back far enough that the gaps between them can be drawn.
//
// **A replicated world judders at the server's tick rate, and a
// `scene::PreviousTransform` does not fix it.** The demo world is smooth
// because it owns both ends of its own tick: the previous pose and the current
// one are two states of one local simulation, half a tick apart by
// construction. A replica owns neither. The two states worth interpolating
// between are two *received* ticks, and they arrive irregularly over a link
// that drops, duplicates and reorders - so writing a previous transform on
// arrival interpolates between whichever two packets happened to land, which is
// smooth exactly while the link is and judders the moment it is not.
//
// So: hold what arrived in a small tick-ordered buffer, render at a fixed delay
// behind the newest tick, and interpolate between the two samples that bracket
// the render position. **The delay is the jitter budget and it is the one
// number that matters** - see `InterpolationSettings::DelayTicks`, which states
// what each end of its range costs.
//
// **And when there is nothing left to interpolate toward, it stops and says how
// long for.** The clock never runs past the newest sample - that is D00010 and
// it has not changed - but a caller holding a body's velocity can spend the time
// the clock could not, which is `DeadReckonSeconds` and `D00015(c)`. Deciding
// *which* bodies is a question about components and therefore not this module's;
// deciding *how long* is a question about the link and therefore is.
//
// **This is presentation and nothing here may reach a simulation.** The
// interpolated pose is handed back by value to whoever is filling a draw list;
// nothing in this file writes a component, and letting a render-rate quantity
// back into a tick would make the simulation depend on the frame rate of
// whoever was watching. `v02v03v04.md` §2.11.
//
// **Time is passed in, never read.** `Advance` takes the frame's seconds and
// `Record` takes a tick, so a suite states a stall rather than waiting for one -
// the same rule `net/AGENTS.md` and `replication/AGENTS.md` both give, and a
// buffer with a delay in it is exactly the shape that invites a
// `steady_clock::now()`.
//
// **Keyed on ticks, not on arrival times.** A tick is the unit of agreement in
// this module; an arrival timestamp is a fact about one machine's clock and one
// network, and two of them do not agree.
//
// Not to be confused with `Replica`'s joining snapshot, which is a reassembly
// buffer for one world-sized message. This holds many small states over time.
//
// @tier L12 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine::replication {

	// How far behind the newest received tick a replica is drawn, and how it
	// gets back there when it drifts.
	//
	// @since v0.5
	struct InterpolationSettings {
		// What the authority's tick rate is assumed to be, in ticks per second,
		// until enough of the stream has arrived to measure it.
		//
		// **A starting estimate, not the figure that is used.** Nothing on the
		// wire carries the server's tick rate and the two programs do not share
		// a default - `server --listen` paces at 30 and `client` at 60 - so a
		// configured rate is wrong by a factor of two in the most ordinary setup
		// there is, and being wrong by that much is not a drift the correction
		// can absorb: the render clock would spend half of every tick period
		// frozen. `SnapshotBuffer::MeasuredTickRate` is what the clock actually
		// runs at, and it is the ticks and the seconds the caller passed in
		// divided by each other. This is only what it uses for the first
		// quarter-second, before there is anything to divide.
		double TickRate = 60.0;

		// The jitter budget: how far behind the newest received tick the world
		// is drawn.
		//
		// **Two ticks - 33 ms at 60 Hz - and the number is a compromise between
		// two visible failures, not a tuning knob.**
		//
		// What it actually buys is `DelayTicks - 1` tick periods of lateness,
		// because one of them is the gap between two on-time arrivals. So:
		//
		// - **At zero there is nothing to interpolate between.** The render
		//   position sits on the newest sample, every frame between two ticks
		//   draws the same pose, and the result is exactly the judder this class
		//   exists to remove.
		// - **At one the budget is nil.** The next tick has to arrive before the
		//   previous one has been drawn out, so the first packet that is even
		//   slightly late is a stall, and on a real link that is most of them.
		// - **At two there is one tick period - 17 ms - of slack.** One tick
		//   arriving a whole period late, or one lost and recovered by the next,
		//   is absorbed with nothing to spare. This is the same setting Source's
		//   `cl_interp_ratio 2` names, and for the same reasons.
		// - **Above about four it stops being free.** Everything that is not the
		//   local player is drawn where it was 67 ms ago, which is where a player
		//   starts leading their aim without knowing why.
		//
		// Raise it to three where a link's jitter is known to exceed a tick
		// period; that costs 17 ms more apparent lag on everything replicated and
		// buys a second period of slack. Lower it only with a measurement.
		double DelayTicks = 2.0;

		// How much further behind than `DelayTicks` the render clock may fall
		// before it jumps rather than eases back.
		//
		// A jump is a visible skip and easing is not, so easing is preferred -
		// but easing a lag of seconds takes seconds, during which the world is
		// visibly late the whole time. Eight ticks past the delay is well outside
		// any jitter this is meant to absorb and squarely in "the connection
		// paused and came back" territory, which is a skip whichever way it is
		// handled.
		double ResyncTicks = 8.0;

		// How much faster or slower than the authority the render clock runs
		// while it is closing a gap, as a fraction of the tick rate.
		//
		// Five percent, because it has to be invisible: this is the whole world
		// briefly moving at the wrong speed. It is also what absorbs the
		// permanent, tiny disagreement between two machines' idea of a second -
		// a crystal is out by parts per million and this covers that by four
		// orders of magnitude.
		//
		// It only applies outside a one-tick deadband either side of the delay.
		// Inside it the clock runs at exactly the tick rate: `behind` swings by a
		// whole tick every tick simply because ticks arrive one at a time, and
		// correcting against that swing would make the world speed up and slow
		// down once per tick.
		double CorrectionFraction = 0.05;

		// How far past the newest received tick a body may be dead-reckoned
		// before the guess is given up and the world simply holds, in seconds.
		//
		// **A quarter of a second, and it is derived rather than chosen.** A
		// decoded position is out by half a step of the position grid; a decoded
		// velocity by half a step of the velocity grid. Integrating the second
		// accumulates linearly, so a dead-reckoned pose is out by the first plus
		// the second times the elapsed seconds - and the two are equal at
		// exactly the ratio of the grids' extents, 64 m over 256 m/s. Past that
		// the guess is worse-conditioned than the last thing the authority
		// actually said, which is the point at which holding is the better
		// answer whatever else is true.
		//
		// **Written as a number here because this module may not see the
		// grid.** `replication` links no simulation module and names no
		// component, so `scene::WIRE_DEAD_RECKON_SECONDS` is where it is
		// derived and `client.replicated` is the suite that fails if the two
		// ever disagree. Rule 6: the build cannot check it, so a test does.
		//
		// Zero switches dead reckoning off, and a host with a link that never
		// stalls loses nothing by it.
		double ExtrapolateSeconds = 0.25;

		// How fast a dead-reckoned offset is given back once ticks arrive
		// again, as a fraction of real time.
		//
		// **Given back rather than dropped, and this is the correction
		// decision.** The offset is `velocity * seconds`, so easing `seconds`
		// to zero eases the offset to zero - one number instead of a per-entity
		// blend, and continuous by construction for every body at once.
		//
		// **Half, because that is the fastest a body can be corrected without
		// appearing to move backwards.** The guess is unwound at this fraction
		// of real time while the authority's own pose keeps moving forward, so
		// the drawn body travels at `1 - UnwindFraction` of its speed for as
		// long as the correction lasts. At one it stands still; above one it
		// reverses, which is the one artefact more visible than the snap this
		// exists to avoid. At half, a correction of the full
		// `ExtrapolateSeconds` takes twice that - half a second of a body at
		// half speed, against a snap of however far it had been carried.
		double UnwindFraction = 0.5;

		// How many ticks of history are kept per entity.
		//
		// The buffer only ever reads the two samples bracketing the render
		// position, so this is a bound on how far behind the render clock may
		// fall and still find them - comfortably more than `ResyncTicks`, so a
		// clock about to be resynced has not already lost the samples it needs.
		size_t HistoryTicks = 16;
	};

	// Received poses, held in tick order, sampled at a fixed delay behind the
	// newest.
	//
	// One instance per replicated world. Feed it with `Record` whenever a tick
	// has been applied, move it on with `Advance` once per frame, and ask it for
	// a pose with `Sample`.
	//
	// @since v0.5
	class SnapshotBuffer {
	  public:
		// Creates a buffer.
		//
		// @param settings The delay, the rate it is measured against, and how
		//        much history to keep.
		explicit SnapshotBuffer(const InterpolationSettings &settings = {});

		// Whether a tick has already been recorded.
		//
		// The cheap question a caller asks before walking a world to record it,
		// since a client polls many times per received tick and the walk is the
		// expensive half.
		//
		// @param tick The tick to ask about.
		// @return `true` when nothing new would be recorded for it.
		bool Holds(uint64_t tick) const;

		// Records where one entity was at one tick.
		//
		// **A pose per entity per tick, taken from the world after the tick was
		// applied.** That is deliberately what the store holds rather than what
		// the tick's messages carried.
		//
		// **And the caller feeds it `Replica::Applied`, which is why a tick that
		// arrived in pieces and lost one is never recorded at all.** `Applied`
		// names the last tick held in full - every part of it and every row it
		// named - so the store this reads is never a mixture of the rows one
		// datagram carried and the previous values of the rows another was going
		// to. A pose taken from that mixture would be interpolated through and
		// then contradicted a tick later when the missing values arrive, which is
		// a visible snap on entities that never moved. D00013.
		//
		// A tick skipped this way leaves a wider gap between two samples, which
		// is a longer segment to interpolate across and not a hole: the clock is
		// on the tick axis and the arithmetic does not care that the two are two
		// ticks apart rather than one.
		//
		// An entity in the predicted index range is ignored, and so is the
		// entity named by `Predict`. Both are this client's own run-ahead and
		// delaying either is input lag the player can feel.
		//
		// @param tick   The tick the pose is the state of.
		// @param entity Whose pose it is.
		// @param frame  Where it was.
		void Record(uint64_t tick, ecs::Entity entity, const core::CFrame &frame);

		// Moves the render position on by one frame.
		//
		// Called once per frame, with the frame's own elapsed seconds. The clock
		// runs at the authority's tick rate and is corrected back toward
		// `DelayTicks` behind the newest received tick - slowly, by
		// `CorrectionFraction`, and only once it is more than a tick out; or in
		// one jump past `ResyncTicks`.
		//
		// **It never runs past the newest sample.** There is nothing to
		// interpolate toward there, and inventing a pose out of two samples this
		// class does not have would be a guess with nothing behind it. The clock
		// stops instead, the world holds its last received pose, and
		// `Statistics::Stalls` says it happened.
		//
		// What it could not spend is kept rather than discarded, for a caller
		// that does have something behind a guess: `DeadReckonSeconds`.
		//
		// @param frameSeconds Wall seconds since the previous frame. A negative
		//        value is treated as zero.
		void Advance(double frameSeconds);

		// How far past the newest received tick the world wanted to be, in
		// seconds, and therefore how far a body may be dead-reckoned.
		//
		// **Zero on a healthy link, which is most of the time.** The clock runs
		// `DelayTicks` *behind* the newest sample, so there is normally
		// something to interpolate toward and nothing to guess about. This is
		// what the clock could not spend because it had reached the newest
		// sample - the same frames `Statistics::Stalls` counts - capped at
		// `ExtrapolateSeconds` and eased back to zero at `UnwindFraction` once
		// ticks arrive again.
		//
		// **This class does not apply it, and that is the division rather than
		// an omission.** Which entities may be dead-reckoned is a question about
		// components - a velocity to integrate, and no `scene::NetworkOwner`
		// saying somebody else already simulates it - and this module names no
		// component and links no simulation module, exactly as
		// `DistancePriority` and `Rewind::Record` do not. The arithmetic is
		// here; the lookup is the host's. `client::CollectReplicated` is the
		// host that does it, and `replication/AGENTS.md` carries the rule.
		//
		// @return Seconds to advance a body by, or zero for none.
		double DeadReckonSeconds() const {
			return Reckoned;
		}

		// Where an entity should be drawn now.
		//
		// Not `const`, because `Statistics::Interpolated` against
		// `Statistics::Held` is the ratio that says whether this buffer is
		// smoothing anything at all - and a counter reached through a `mutable`
		// is a const method that is not one.
		//
		// @param entity The entity to place.
		// @return The interpolated pose, or nothing when this buffer has no
		//         business placing it - an entity it has never recorded, one it
		//         has not heard about for longer than the history it keeps, or
		//         one that is predicted. **The caller draws its own live value
		//         in that case**, which for a predicted entity is the whole
		//         point and for an unknown one is the only thing there is.
		std::optional<core::CFrame> Sample(ecs::Entity entity);

		// Names the one entity that is predicted and must never be delayed.
		//
		// Prediction and interpolation are the two halves of the same problem
		// and they pull opposite ways: the local player is run ahead so that
		// input feels immediate, and delaying it by the jitter budget puts the
		// lag straight back. Everything else is interpolated authoritative
		// state, which is `replication/AGENTS.md`'s "prediction is the local
		// player and nothing else" read from the other side.
		//
		// Whatever it already held for this entity is dropped, so nominating one
		// mid-connection takes effect on the next frame rather than after the
		// history drains.
		//
		// @param entity The predicted entity, or `ecs::NULL_ENTITY` to nominate
		//        none.
		void Predict(ecs::Entity entity);

		// The entity `Predict` named.
		//
		// @return The predicted entity, or `ecs::NULL_ENTITY`.
		ecs::Entity Predicted() const {
			return Predicted_;
		}

		// Drops everything held for an entity.
		//
		// For a destroy or a forget: the next time that index is issued it is a
		// different thing, and interpolating from where the previous one stood
		// is a pose neither of them was ever at.
		//
		// @param entity The entity to drop.
		void Forget(ecs::Entity entity);

		// Drops everything, including the render clock.
		//
		// For a rejoin, where the ticks held describe a world that no longer
		// exists.
		void Clear();

		// Where along the tick axis the world is currently being drawn.
		//
		// Fractional: 104.25 means a quarter of the way from tick 104 to tick
		// 105. Zero before anything has been recorded.
		//
		// @return The render position, in ticks.
		double RenderTick() const {
			return RenderTicks;
		}

		// The newest tick recorded.
		//
		// @return The tick, or zero before anything has been recorded.
		uint64_t Newest() const {
			return Newest_;
		}

		// The rate the render clock is actually running at, in ticks per second.
		//
		// **Measured, not configured.** The caller hands this class ticks
		// through `Record` and seconds through `Advance`, so the rate is one
		// divided by the other - a derived quantity rather than a second fact
		// that can disagree with the first, and still nothing that reads a
		// clock. Until a quarter of a second has passed it is
		// `InterpolationSettings::TickRate`, because a ratio of almost nothing
		// to almost nothing is noise.
		//
		// It is an average over roughly the last eight seconds, which is long
		// enough that a pause does not move it much and short enough that it is
		// still about this connection.
		//
		// @return The rate.
		double MeasuredTickRate() const;

		// How far behind the newest received tick the world is being drawn.
		//
		// The number to watch. It sits at `DelayTicks` on a healthy link, climbs
		// when the link delivers faster than the clock can absorb, and falls
		// toward zero as a late packet eats the budget.
		//
		// @return The distance, in ticks. Zero before anything has been
		//         recorded.
		double Behind() const;

		// What this buffer has done.
		//
		// @since v0.5
		struct Statistics {
			// Distinct ticks recorded.
			uint64_t Ticks = 0;

			// Frames the render clock could not advance because it had reached
			// the newest sample.
			//
			// **The number that says the delay is too small for this link.** A
			// handful over a session is one bad moment; a figure that tracks the
			// frame count is a world that is frozen more often than it is
			// moving, and the answer is a larger `DelayTicks` rather than
			// anything in this class.
			uint64_t Stalls = 0;

			// Times the render clock jumped rather than eased.
			//
			// Past `ResyncTicks` from where it should be. Zero on a link that is
			// merely jittery; not zero means the connection paused.
			uint64_t Resyncs = 0;

			// Poses answered by interpolating between two ticks.
			//
			// Against `Held` below, this is the ratio that says whether the
			// world is actually being smoothed. All `Held` and no `Interpolated`
			// is a buffer doing nothing.
			uint64_t Interpolated = 0;

			// Poses answered by holding a single tick - before the oldest
			// sample, after the newest, or with only one to hand.
			uint64_t Held = 0;

			// Frames on which `DeadReckonSeconds` was not zero.
			//
			// Against `Stalls`, this says how much of a stall was covered by a
			// guess rather than by a freeze. It counts frames rather than
			// entities because the clock is one clock: which of the bodies drawn
			// on those frames were eligible is the host's answer and not this
			// class's to report.
			uint64_t Extrapolated = 0;
		};

		// What this buffer has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

		// The delay and the rates it is measured against.
		//
		// @return The settings this buffer was built with.
		const InterpolationSettings &Settings() const {
			return Settings_;
		}

	  private:
		// One entity's pose at one tick.
		struct Pose {
			uint64_t Tick = 0;
			core::CFrame Frame;
		};

		// One entity's history, as a ring so that recording a tick is a write
		// rather than a shuffle of everything older.
		struct Track {
			std::vector<Pose> Ring;
			size_t Head = 0;  ///< Where the next pose goes.
			size_t Count = 0; ///< How many of the ring are filled.
		};

		// The `index`th oldest pose in a track.
		static const Pose &Oldest(const Track &track, size_t index);

		// Drops tracks nothing has recorded into for longer than the history
		// kept. Called once per new tick rather than per frame, because it is
		// the only thing here that walks every entity.
		void Prune();

		InterpolationSettings Settings_;

		// Point lookups only, and never iterated in an order anything observes:
		// `Prune` erases a set that does not depend on the order it is found in,
		// and every other access is by key. `replication/AGENTS.md` bars an
		// unordered container from deciding an *ordering*, which this is not.
		std::unordered_map<ecs::Entity, Track> Tracks;

		ecs::Entity Predicted_;

		// The render position, in ticks. Fractional, and deliberately a double:
		// a float loses tick resolution somewhere around a day of uptime at
		// 60 Hz, which is exactly the run nobody tests.
		double RenderTicks = 0.0;

		uint64_t Newest_ = 0;
		bool Started = false;

		// Seconds of dead reckoning currently in force. See
		// `DeadReckonSeconds`.
		double Reckoned = 0.0;

		// Ticks and seconds over the same whole tick intervals, which are the two
		// halves of `MeasuredTickRate`. Both are halved once the window is full,
		// which keeps the ratio and makes it an eight-second average rather than
		// a whole-connection one.
		double ObservedTicks = 0.0;
		double ObservedSeconds = 0.0;

		// The running total of what `Advance` was given, and what it read at the
		// last tick boundary. Their difference is what a tick's arrival adds to
		// `ObservedSeconds`, which is what keeps the two in phase.
		double Elapsed = 0.0;
		double ElapsedAtTick = 0.0;

		Statistics Stats_;
	};

	// Registers this module's own resource types under explicit names.
	//
	// **Rule 4, and `engine::render::DrawList` is the entry that learned it.** A
	// `SnapshotBuffer` is set as a world resource, a resource is keyed by a
	// component id, and `Store::SetResource` mints one under the compiler's
	// spelling of the type unless a name was registered first. `Store::Save`
	// then refuses the world - correctly, because it has no way to write a
	// resource nobody described - and a replica world could not be snapshotted
	// at all. The studio saves the universe when Play is pressed, so that was a
	// Play that failed for a reason no message named.
	//
	// **Registered here rather than by the client**, because the module that
	// owns a type is the one that names it: a second host that holds a replica
	// would otherwise have to know to do this, and would find out the same way.
	//
	// The serialisation writes nothing and reads back cleared, exactly as
	// `engine::render::DrawList`'s does and for a stronger reason: this holds a
	// connection's ticks in flight, and a file carrying them would describe a
	// session that ended.
	//
	// Idempotent. Call it before anything touches a `SnapshotBuffer`:
	// `Components::Of<T>` caches its answer per type per process, so an explicit
	// registration arriving second aborts rather than leaving two names for one
	// thing.
	//
	// @since v0.11
	void RegisterReplicationComponents();
}
