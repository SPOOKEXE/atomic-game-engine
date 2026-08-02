#pragma once

// One view of one world, published for a compositor to draw.
//
// Three consumers on the roadmap are the same object underneath: a surface
// camera for mirrors, split-screen across two worlds, and any view whose
// producer is not the compositor. All of them are a *view* — produced
// somewhere, buffered, and composited by the client at its own frame rate.
//
// **Three slots and one atomic publish index, not a lock.** A lock is correct
// and couples the two rates: a compositor that stalled a producer would be a
// client stalling a simulation, and the frame budget cannot absorb that. With a
// triple buffer a slow consumer **drops frames instead of throttling the
// world**, which is the behaviour you want — and it makes the remote case
// behave exactly like the local one. The atomics are lock-free and sit inside
// the region, so identical code works across a process boundary with no named
// mutex.
//
// **No backpressure and no queue.** The consumer always takes the newest frame.
// Dropped frames are counted rather than hidden, because a starved view should
// be visible on F5 rather than mysterious.
//
// **The payload is opaque here.** This layer moves bytes, exactly as
// `Envelope` does; L7's `scene::DrawInstance` gives them meaning and the tier
// check keeps L4 from learning what a draw instance is. Carrying a *draw list*
// rather than pixels is what lets a `server`-tier host publish at all — a host
// that published pixels would need a GPU and would stop being server tier.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::world {

	// What a view frame says about itself.
	//
	// Presentation only: nothing here is read by simulation, so none of it has
	// to be deterministic. Letting a view back into the tick would make the
	// simulation depend on the frame rate of whoever was watching.
	//
	// @since v0.2
	struct ViewHeader {
		// The world this is a view of. A name, because it crosses.
		core::Name World;

		// The tick that produced it. A compositor uses this and `Alpha` to
		// interpolate, and to notice a producer that has stopped.
		uint64_t SourceTick = 0;

		// The interpolation position the producer used, 0..1.
		float Alpha = 0.0f;

		// Where the view was taken from.
		core::CFrame Camera;

		// Monotonic per channel. A consumer that sees the same serial twice has
		// been handed a repeat and can skip the work.
		uint32_t Serial = 0;

		// How many payload bytes accompany this header.
		uint32_t PayloadBytes = 0;
	};

	// A triple-buffered slot for one view.
	//
	// One producer and one consumer. Two producers would need a lock, and there
	// is never more than one: a view is published by the world it views.
	//
	// @since v0.2
	class ViewChannel {
	  public:
		// The number of slots. Three is the smallest number that lets a
		// producer write while a consumer reads and still have somewhere to put
		// the next frame.
		static constexpr uint32_t SLOTS = 3;

		// Creates a channel sized for one payload.
		//
		// Slots are allocated once, at the maximum, so publishing never
		// allocates — which is what keeps a producer off the allocator inside
		// its own render phase.
		//
		// @param maximumPayload The largest payload a frame may carry.
		explicit ViewChannel(size_t maximumPayload);

		// A channel is a rendezvous between two threads and is never copied.
		ViewChannel(const ViewChannel &) = delete;

		// A channel is a rendezvous between two threads and is never assigned.
		ViewChannel &operator=(const ViewChannel &) = delete;

		// Publishes one frame. Never blocks and never fails for want of room.
		//
		// The `Serial` field is filled in here rather than by the caller, so it
		// is monotonic whatever the producer does.
		//
		// @param header  What the frame is. `Serial` is overwritten.
		// @param payload The bytes. Refused when larger than the maximum.
		// @return `false` only when the payload is too large.
		bool Publish(const ViewHeader &header, std::span<const std::byte> payload);

		// Takes the newest frame, if one has arrived since the last call.
		//
		// @param header  Filled with the frame's header.
		// @param payload Filled with the frame's bytes, keeping its capacity.
		// @return `false` when nothing new has been published.
		bool Acquire(ViewHeader &header, std::vector<std::byte> &payload);

		// Whether a frame is waiting.
		//
		// @return `true` when Acquire would return one.
		bool Ready() const {
			return Published_.load(std::memory_order_acquire) != NONE;
		}

		// How many frames have been published.
		//
		// @return The publish count.
		uint64_t Frames() const {
			return Count.load(std::memory_order_relaxed);
		}

		// How many frames were published and never taken.
		//
		// The number worth putting on F5: a figure that climbs is a compositor
		// that cannot keep up with a producer, which is a tuning problem rather
		// than a bug — but only if somebody can see it.
		//
		// @return The dropped count.
		uint64_t Dropped() const {
			return Drops.load(std::memory_order_relaxed);
		}

		// The largest payload this channel accepts.
		//
		// @return The maximum in bytes.
		size_t MaximumPayload() const {
			return Maximum;
		}

	  private:
		// The value meaning "no slot".
		static constexpr uint32_t NONE = 0xFFFFFFFFu;

		struct Slot {
			ViewHeader Header;
			std::vector<std::byte> Payload;
		};

		// The slot holding the newest published frame, or NONE.
		std::atomic<uint32_t> Published_{NONE};

		// The slot the consumer is copying out of, or NONE. Claimed *before*
		// the published slot is cleared, so there is no window in which a slot
		// belongs to neither side and the producer could overwrite it mid-read.
		std::atomic<uint32_t> Holding{NONE};

		std::atomic<uint64_t> Count{0};
		std::atomic<uint64_t> Drops{0};

		uint32_t NextSerial = 1;
		size_t Maximum = 0;
		Slot Slots[SLOTS];
	};
}
