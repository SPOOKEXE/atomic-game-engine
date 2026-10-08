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
// - applying everything at the top of the block - is audible jitter, and it is
// the one place where "close enough to the frame" is wrong.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::audio {

	// What one render did, for a meter and for a test.
	//
	// @since v0.9
	struct MixReport {
		// Frames produced.
		size_t Frames = 0;

		// Commands dispatched, including owner-side refusals.
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

		// The first sample produced by this block on the absolute mixer clock.
		uint64_t BeginSample = 0;

		// One past the final sample produced by this block.
		uint64_t EndSample = 0;

		// Process-local token tying this report to the mixer's retained trace.
		uint64_t ObservationSerial = 0;
	};

	// Result of dispatching a copied playback command on the render owner.
	enum class PlaybackStatus : uint8_t {
		Applied,
		InvalidCursor,
		MissingSource,
		OutOfRange,
		StaleGeneration,
		MissingTarget,
		Refused,
	};

	// One attempted command dispatch and its outcome in the latest block.
	//
	// This is an internal-clock record. `NodeId` is deliberately kept here and
	// translated to a stable source name by the observation boundary before the
	// record leaves the process.
	struct AppliedAudioCommand {
		// Command kind dispatched by the mixer.
		CommandKind Kind = CommandKind::None;
		// Process-local target node, translated to a durable name at capture.
		NodeId Target;
		// Process-local related node, translated to a durable name at capture.
		NodeId Related;
		// Actual incarnation after dispatch (or before successful removal).
		uint64_t PlaybackGeneration = 0;
		// Incarnation supplied by the producer, including stale refusals.
		uint64_t RequestedPlaybackGeneration = 0;
		// Sample clock position requested by the game tick.
		uint64_t RequestedSample = 0;
		// Sample clock position where the mixer dispatched the command.
		uint64_t AppliedSample = 0;
		// Frame offset within the observed audio block.
		size_t OffsetFrames = 0;
		// Owner-side outcome; dispatch timing alone does not mean success.
		PlaybackStatus Status = PlaybackStatus::Applied;
	};

	// A player that reached the end of its source during the latest block.
	struct FinishedAudioSource {
		// Process-local source node, translated to a durable name at capture.
		NodeId Source;
		// Incarnation that naturally completed.
		uint64_t PlaybackGeneration = 0;
		// Sample clock position where the source completed.
		uint64_t AtSample = 0;
		// Frame offset within the observed audio block.
		size_t OffsetFrames = 0;
	};

	// A copy of an existing command/finish record with the resulting player state.
	// NaturalCompletion selects Finished; otherwise Applied selects the command.
	struct PlaybackEvent {
		bool NaturalCompletion = false;
		AppliedAudioCommand Applied{};
		FinishedAudioSource Finished{};
		PlaybackStatus Status = PlaybackStatus::Applied;
		uint64_t PlaybackGeneration = 0;
		bool Present = false;
		bool Playing = false;
		double CursorFrames = 0.0;
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

		// Commands dispatched by the latest successful render, with outcomes.
		//
		// The span remains valid until the next call to `Render` or
		// `ApplyPending`. Storage is fixed so recording it never allocates on the
		// device thread.
		std::span<const AppliedAudioCommand> LastAppliedCommands() const {
			return std::span<const AppliedAudioCommand>(AppliedCommands.data(), AppliedCommandCount);
		}

		// Players that naturally finished in the most recent successful render.
		std::span<const FinishedAudioSource> LastFinishedSources() const {
			return std::span<const FinishedAudioSource>(FinishedSources.data(), FinishedSourceCount);
		}

		// Process-local token for the retained render trace.
		uint64_t ObservationSerial() const {
			return CurrentObservationSerial;
		}

		// Configure only before the device starts or while its owner is paused.
		void EnablePlaybackEvents(bool enabled = true) {
			PlaybackEventsEnabled = enabled;
		}

		// One render producer and one host consumer. Copies never expose graph storage.
		// Poll even when output is paused to receive acknowledged command bursts.
		// State acknowledgements do not retire SoundRefs retained by render scratch;
		// Keep host ownership until shutdown, or quiesce posts, pause the device,
		// owner ApplyPending then Render an empty matching-format buffer with no
		// new posts to clear Taken/Schedule. The old sound must also be unbound.
		// Pausing alone does not clear retained or pending SetSound copies.
		size_t PollPlaybackEvents(std::span<PlaybackEvent> into);

		// A changed count means the host's state is incomplete. Rebind a fresh
		// incarnation before trusting it; dropped acknowledgements are not success.
		uint64_t PlaybackEventsDropped() const {
			return MissedPlaybackEvents.load(std::memory_order_relaxed);
		}

		static constexpr size_t PLAYBACK_EVENT_CAPACITY = 2048;

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

		PlaybackStatus Apply(const Command &command);
		void ApplyRecorded(const Command &command, size_t offset);
		void FinishPlayer(NodeId id, Node &node, size_t offset);
		void PublishPlaybackEvent(const PlaybackEvent &event);

		// Mixes `frames` starting at `offset` in the output.
		void MixSegment(SampleBuffer &out, size_t offset, size_t frames);

		// Fills one node's scratch buffer for a segment.
		void RenderNode(size_t index, size_t frames, size_t blockOffset);

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

		// A render can drain at most the command ring's capacity. Fixed storage
		// preserves the callback's no-allocation rule while retaining exact
		// event timing for an offline observation made after the block.
		std::array<AppliedAudioCommand, CommandQueue::CAPACITY> AppliedCommands{};
		size_t AppliedCommandCount = 0;
		// A block may finish every player plus players restarted by its command ring.
		std::array<FinishedAudioSource, AudioGraph::MAXIMUM_NODES + CommandQueue::CAPACITY> FinishedSources{};
		size_t FinishedSourceCount = 0;
		uint64_t CurrentObservationSerial = 0;

		// Fixed SPSC copies share the same records as the owner-only trace.
		bool PlaybackEventsEnabled = false;
		std::array<PlaybackEvent, PLAYBACK_EVENT_CAPACITY> PlaybackEvents{};
		std::atomic<size_t> PlaybackWrite{0};
		std::atomic<size_t> PlaybackRead{0};
		std::atomic<uint64_t> MissedPlaybackEvents{0};
		static_assert(std::atomic<size_t>::is_always_lock_free);
		static_assert(std::atomic<uint64_t>::is_always_lock_free);
		static_assert((PLAYBACK_EVENT_CAPACITY & (PLAYBACK_EVENT_CAPACITY - 1)) == 0);

		size_t FinishedThisBlock = 0;
	};
}
