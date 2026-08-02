#pragma once

// Recording a universe, and playing it back.
//
// **A recording is one snapshot plus every envelope applied since.** That is
// the whole format, and it is complete because a world is deterministic given
// its state and its inbox: nothing else reaches a headless world, and bus
// traffic is already ordered before it is applied. So a recording is a faithful
// log rather than a reconstruction.
//
// It also costs almost nothing to build, because both halves already existed
// for another reason — the snapshot for restarting a crashed world, the ordered
// traffic for making the barrier deterministic. Replay is what those two turn
// into when you keep them.
//
// **What replay is for.** Across hundreds of concurrently simulating worlds, a
// one-in-ten-thousand bug is the difference between fixable and not. Capture it
// once and it reproduces on demand.
//
// **Same binary, same machine.** Determinism here is not cross-machine and must
// not be claimed as such: floating point differs between compilers and chips.
// A recording made by one build replays under that build.
//
// Component and world **names** are recorded, never ids, so a rename or a
// reordered registration does not invalidate yesterday's recording.
//
// @tier L4 · shared

#include <engine/core/Bytes.hpp>
#include <engine/world/Bus.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace engine::world {

	// One barrier's worth of recorded input.
	//
	// @since v0.2
	struct RecordedBarrier {
		// The wall seconds the driver was handed. Replaying with a different
		// value would run a different number of ticks.
		float FrameSeconds = 0.0f;

		// Every envelope applied at that barrier, in the order applied.
		std::vector<Envelope> Traffic;
	};

	// Captures a universe and everything that happens to it.
	//
	// @since v0.2
	class Recorder {
	  public:
		// The recording format this build writes and accepts.
		static constexpr uint32_t VERSION = 1;

		// Captures the universe's current state as the starting point.
		//
		// Everything recorded afterwards is relative to this, so a recording
		// begun mid-run replays from mid-run rather than from nothing.
		//
		// @param universe The universe to record.
		// @return `false` when the universe cannot be snapshotted.
		bool Begin(const Universe &universe);

		// Records the barrier that has just been applied.
		//
		// Called immediately after `Universe::Tick`, because that is when the
		// applied traffic is still available and before the next tick replaces
		// it.
		//
		// @param universe     The universe that just ticked.
		// @param frameSeconds The value that tick was given.
		void Capture(const Universe &universe, float frameSeconds);

		// Whether a recording has been started.
		//
		// @return `true` between Begin and Clear.
		bool Recording_() const {
			return Started;
		}

		// The number of barriers captured so far.
		//
		// @return The barrier count.
		size_t Barriers() const {
			return Timeline.size();
		}

		// Writes the recording.
		//
		// @param writer The writer to append to.
		// @return `false` when nothing has been recorded.
		bool Write(core::ByteWriter &writer) const;

		// Discards everything.
		void Clear();

	  private:
		bool Started = false;
		std::vector<std::byte> Initial;
		std::vector<RecordedBarrier> Timeline;
	};

	// Plays a recording back into a universe.
	//
	// @since v0.2
	class Replayer {
	  public:
		// Reads a recording.
		//
		// @param reader The reader to consume.
		// @return `false` on a corrupt, truncated or wrong-version recording.
		bool Load(core::ByteReader &reader);

		// Puts a universe back to the recording's starting state.
		//
		// Everything the universe held is replaced, for the same reason a
		// store's Load replaces rather than merges.
		//
		// **A snapshot carries state, never code.** Systems are C++ callables,
		// so a restored world has storage and no behaviour until somebody
		// registers them again — exactly as a respawned host does by running
		// the same program. `configure` is called once per restored world for
		// that, and taking it as a parameter is what stops it being forgotten:
		// the failure otherwise is a world that ticks and does nothing, which
		// looks like a simulation bug rather than a missing registration.
		//
		// @param universe  The universe to restore into.
		// @param configure Called as `configure(Universe &, WorldId)` for each
		//                  restored world, to register its systems.
		// @return `false` when the recorded snapshot could not be restored.
		bool
		Restore(Universe &universe, const std::function<void(Universe &, WorldId)> &configure = {}) const;

		// Runs the next recorded barrier.
		//
		// @param universe The universe to advance.
		// @return `false` when the recording is finished.
		bool Step(Universe &universe);

		// Runs every remaining barrier.
		//
		// @param universe The universe to advance.
		// @return The number of barriers run.
		size_t Run(Universe &universe);

		// The number of barriers not yet played.
		//
		// @return The remaining barrier count.
		size_t Remaining() const {
			return Timeline.size() - Position;
		}

		// The number of barriers in the recording.
		//
		// @return The total barrier count.
		size_t Barriers() const {
			return Timeline.size();
		}

		// The frame time the last `Step` used.
		//
		// A recording decides its own frame times, so a process recording *what
		// it replays* — which is how the replay path is checked against the
		// original — has to capture with the recorded delta rather than one it
		// measured. Zero before the first step.
		//
		// @return The last replayed barrier's frame seconds.
		float LastFrameSeconds() const {
			return LastFrame;
		}

		// Rewinds to the start without reloading.
		void Rewind() {
			Position = 0;
			LastFrame = 0.0f;
		}

	  private:
		std::vector<std::byte> Initial;
		std::vector<RecordedBarrier> Timeline;
		size_t Position = 0;
		float LastFrame = 0.0f;
	};
}
