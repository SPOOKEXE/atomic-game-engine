#pragma once

// A framed byte queue between two endpoints.
//
// This is the seam that keeps thread-per-world and process-per-world the same
// design. Everything crossing a world boundary is already bytes - that is rule
// 3, and `world`'s buses were built to it - so the only difference between two
// worlds in one process and two worlds in two processes is what carries the
// bytes. A caller holding a `Channel` cannot tell which it has, and that is the
// whole point rather than a nicety.
//
// **Frames, not a stream.** What goes in as one buffer comes out as one buffer.
// A stream would make every reader implement its own framing, and they would
// each get the partial-read case wrong differently.
//
// **Never blocks.** A world tick occupies a job worker, so a send that waited
// for room would stall every other world in the host. A full channel refuses
// and says so; the caller decides whether to drop, retry or complain. That is
// the same reason bus calls return a `Ticket` instead of an answer.
//
// **Bounded.** A producer that outruns its consumer would otherwise grow the
// queue until the host dies, which is a crash a long way from its cause. The
// cap is on bytes rather than frames because frames vary by three orders of
// magnitude and a frame count says nothing about memory.
//
// Only the queue is here. Owning an operating system endpoint and handing it to
// a child is `ProcessChannel.hpp`, which almost nothing needs.
//
// @tier L2 · shared

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::parallel {

	// Why a send or receive did not happen.
	//
	// @since v0.2
	enum class ChannelStatus : uint8_t {
		// The frame was sent, or one was received.
		Ok,

		// Nothing was waiting. Not an error: a channel polled every tick is
		// empty most ticks.
		Empty,

		// The queue is at its byte cap. The caller decides what to do, because
		// only the caller knows whether this frame matters.
		Full,

		// The other endpoint is gone, or this one was closed.
		Closed,

		// The frame is larger than the channel's maximum. Refused whole rather
		// than truncated, because half a frame is worse than none.
		TooLarge,
	};

	// Returns a stable, human-readable name for a channel status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ChannelStatus status);

	// One end of a framed byte queue.
	//
	// Abstract so that the local and cross-process implementations are
	// interchangeable at the call site. There is exactly one implementation
	// today; the interface exists so that adding the second one is a new file
	// rather than a change to every caller.
	//
	// @since v0.2
	class Channel {
	  public:
		// The largest frame a channel accepts by default.
		//
		// Sixteen megabytes: far above any envelope and far below anything that
		// would embarrass a host. A snapshot crossing a channel is chunked by
		// its sender rather than sent whole.
		static constexpr size_t DEFAULT_MAXIMUM_FRAME = 16u * 1024u * 1024u;

		// How many bytes a channel queues before refusing.
		static constexpr size_t DEFAULT_CAPACITY = 64u * 1024u * 1024u;

		virtual ~Channel() = default;

		// Queues one frame for the other endpoint.
		//
		// Never blocks. A frame is either queued whole or not at all.
		//
		// @param frame The bytes to send. An empty frame is legal and arrives
		//              as an empty frame.
		// @return `Ok`, `Full`, `TooLarge`, or `Closed`.
		// @threadsafe
		virtual ChannelStatus Send(std::span<const std::byte> frame) = 0;

		// Takes the next frame, if there is one.
		//
		// Never blocks, so a driver polls it at the barrier and moves on. The
		// vector is resized to the frame; its capacity is kept, so a caller
		// reusing one across ticks stops allocating.
		//
		// @param frame Filled with the frame's bytes. Untouched unless `Ok`.
		// @return `Ok`, `Empty`, or `Closed` when closed and drained.
		// @threadsafe
		virtual ChannelStatus Receive(std::vector<std::byte> &frame) = 0;

		// The number of frames waiting to be received.
		//
		// @return The queued frame count.
		// @threadsafe
		virtual size_t Pending() const = 0;

		// The number of bytes waiting to be received.
		//
		// What the capacity is measured against, and the number worth watching:
		// a figure that climbs is a consumer falling behind.
		//
		// @return The queued byte count.
		// @threadsafe
		virtual size_t PendingBytes() const = 0;

		// Whether this endpoint can still be used.
		//
		// @return `false` once either end has closed.
		// @threadsafe
		virtual bool Open() const = 0;

		// Closes this endpoint.
		//
		// The other end may still drain what was already queued - a host that
		// exits cleanly should not strip the driver of the last thing it said.
		// Once drained, the other end reports `Closed`.
		//
		// @threadsafe
		virtual void Close() = 0;
	};

	// How a channel is sized.
	//
	// @since v0.2
	struct ChannelSettings {
		// The largest single frame.
		size_t MaximumFrame = Channel::DEFAULT_MAXIMUM_FRAME;

		// The most bytes queued in one direction before sends are refused.
		size_t Capacity = Channel::DEFAULT_CAPACITY;
	};

	// Creates a connected pair of endpoints inside this process.
	//
	// What one end sends, the other receives; the two directions are
	// independent, so a full inbound queue does not stop the outbound one.
	//
	// The cross-process implementation is the same interface over a socket, and
	// nothing that holds a `Channel` needs to change between them.
	//
	// @param settings How to size both directions.
	// @return Two endpoints, already connected.
	// @since v0.2
	std::pair<std::unique_ptr<Channel>, std::unique_ptr<Channel>>
	MakeLocalChannel(const ChannelSettings &settings = {});
}
