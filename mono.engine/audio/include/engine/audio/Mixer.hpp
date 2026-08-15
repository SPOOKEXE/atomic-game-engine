#pragma once

// The pipeline: commands in, one block of samples out.
//
// **The mixer owns the graph**, and that is what makes the threading tractable.
// A tick never touches a node; it posts a command. The mixer drains the queue,
// applies each command at its exact sample, and mixes - all on one thread. So
// there is no lock anywhere in here, and the reason is not that locks were
// avoided but that there is nothing to lock: only one thread ever reads or
// writes the graph.
//
// **A block is split at every command deadline**, which is the whole of
// sample-accurate scheduling. Rendering 512 frames with a `Play` due at sample
// 200 means two sub-blocks: 0..200 without it, 200..512 with it. The alternative
// - applying everything at the top of the block - is what
// `DATATYPES_LIBRARIES.md` §11.2 calls out as audible jitter, and it is the one
// place where "close enough to the frame" is wrong.
//
// **Nothing is allocated during a render.** Every scratch buffer is sized when
// the graph changes and reused after that. A device callback that allocated
// would eventually take a lock inside the allocator, on a thread whose deadline
// is measured in milliseconds.
//
// @tier L12 · client

#include <engine/audio/Commands.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Sample.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine::audio {

	// What one render did, for a meter and for a test.
	//
	// @since v0.9
	struct MixReport {
		// Frames produced.
		size_t Frames = 0;

		// Commands applied.
		size_t Applied = 0;

		// How many pieces the block was cut into. One means nothing was
		// scheduled inside it, which is the common case.
		size_t Segments = 0;

		// The loudest sample in the output, before clipping.
		//
		// **Before**, so a meter shows what the graph produced rather than what
		// survived - a mix that is clipping reads as exactly 1.0 for ever if
		// measured after, which is the number that hides the problem.
		float Peak = 0.0f;

		// Whether anything had to be clipped.
		bool Clipped = false;

		// Players that reached the end and stopped this block.
		size_t Finished = 0;
	};

	// Runs the graph.
	//
	// Named `AudioMixer` rather than `Mixer` so `Device::Mixer()` can be an
	// accessor - a member function and a type of one name compiles and reads
	// badly. `AudioGraph`/`Graph()` is the same pairing.
	//
	// **One owner, one thread.** The device thread calls `Render`; the tick
	// posts to `Commands()`. Everything else on this class is for the owner or
	// for a test.
	//
	// @since v0.9
	class AudioMixer {
	  public:
		// @param format What to produce. An invalid format falls back to the
		//        engine's default rather than aborting, for `Chunker`'s reason:
		//        a bad configuration should not be a crash inside a callback.
		// @param blockFrames The largest block `Render` will be asked for.
		explicit AudioMixer(AudioFormat format = {}, size_t blockFrames = DEFAULT_BLOCK_FRAMES);

		// The graph. For the owning thread, and for building a routing before
		// the device starts.
		AudioGraph &Graph() {
			return Nodes;
		}

		// The graph.
		const AudioGraph &Graph() const {
			return Nodes;
		}

		// Where a tick posts.
		CommandQueue &Commands() {
			return Queue;
		}

		// The format being produced.
		const AudioFormat &Format() const {
			return Shape;
		}

		// How many frames have been rendered since this mixer started.
		//
		// **The sample clock**, and the thing a command's deadline is measured
		// against. A caller scheduling something "in 20 milliseconds" adds
		// `0.020 * SampleRate` to this.
		uint64_t Clock() const {
			return Rendered;
		}

		// Renders one block.
		//
		// Drains the queue, splits the block at every deadline inside it,
		// applies commands at their exact sample, and mixes.
		//
		// @param[out] out Filled with exactly its own length in frames. Its
		//        format must match this mixer's; a mismatch produces silence
		//        rather than a resample, because a resample on this thread is
		//        the wrong answer to a caller's configuration mistake.
		// @return What happened.
		MixReport Render(SampleBuffer &out);

		// Applies every waiting command immediately, ignoring deadlines.
		//
		// For building a routing before the clock is running, and for a test
		// that wants a graph in a known state. **Not for the device thread** -
		// using it there is exactly the tick-boundary quantisation this module
		// exists to avoid.
		//
		// @return How many were applied.
		size_t ApplyPending();

	  private:
		// One command, resolved against this block.
		struct Due {
			Command What;
			size_t Offset = 0;
		};

		void Apply(const Command &command);

		// Mixes `frames` starting at `offset` in the output.
		void MixSegment(SampleBuffer &out, size_t offset, size_t frames);

		// Fills one node's scratch buffer for a segment.
		void RenderNode(size_t index, size_t frames);

		void EnsureScratch();

		AudioFormat Shape;
		size_t BlockFrames;

		AudioGraph Nodes;
		CommandQueue Queue;

		uint64_t Rendered = 0;

		// One scratch buffer per node, indexed alongside the graph's order.
		// Sized when the graph changes and reused after that, so a render
		// allocates nothing.
		std::vector<SampleBuffer> Scratch;
		std::vector<NodeId> ScratchFor;

		// Which scratch slot each node's id occupies.
		//
		// **Because the alternative was a linear scan of `ScratchFor`, per
		// input, per node, per segment.** Summing what is wired into a node
		// means finding each input's scratch, and searching for it made the mix
		// quadratic in the node count - an output with sixty-four inputs over a
		// hundred-and-thirty-node graph is eight thousand comparisons to move
		// sixty-four buffers, and a block split by commands paid all of it again
		// per piece.
		//
		// Rebuilt only alongside `ScratchFor`, which is to say only when the
		// node set changes - far less often than a block is rendered, and never
		// on the device thread's critical path for an unchanged graph.
		std::unordered_map<uint32_t, size_t> SlotOfNode;

		// Reused across renders for the same reason.
		std::vector<Command> Taken;
		std::vector<Due> Schedule;

		size_t FinishedThisBlock = 0;
	};
}
