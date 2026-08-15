#include <engine/audio/Commands.hpp>

#include <utility>

namespace engine::audio {

	CommandQueue::CommandQueue() : Slots(CAPACITY) {
		// Above the output's reserved id. Both counters name nodes in one
		// graph, so starting at one would hand out the output's id first.
		NextNode.store(AudioGraph::FIRST_FREE_ID, std::memory_order_relaxed);
	}

	NodeId CommandQueue::Allocate() {
		// Relaxed is enough: this only has to be unique, and any ordering it
		// might imply is established by the release on `Write` when the command
		// naming it is posted.
		return NodeId{.Value = NextNode.fetch_add(1, std::memory_order_relaxed)};
	}

	bool CommandQueue::Post(const Command &command) {
		const size_t write = Write.load(std::memory_order_relaxed);
		const size_t read = Read.load(std::memory_order_acquire);

		// One slot is always left empty, so a full ring and an empty one are
		// distinguishable without a third variable.
		if (((write + 1) & MASK) == (read & MASK)) {
			Missed.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		// The slot is filled *before* the index is released. A consumer that
		// acquires the new index therefore sees a complete command - which is
		// the whole of why this is safe without a lock.
		Slots[write & MASK] = command;
		Write.store(write + 1, std::memory_order_release);
		return true;
	}

	size_t CommandQueue::Drain(std::vector<Command> &into) {
		const size_t write = Write.load(std::memory_order_acquire);
		size_t read = Read.load(std::memory_order_relaxed);

		size_t taken = 0;
		while (read != write) {
			Command &slot = Slots[read & MASK];
			into.push_back(std::move(slot));
			// The `SoundRef` in the slot is moved out rather than copied, so
			// the ring does not keep a sound alive after it has been handed
			// over - a queue holding the last reference to every sound ever
			// played is a leak that looks like a cache.
			slot = Command{};

			++read;
			++taken;
		}

		Read.store(read, std::memory_order_release);
		return taken;
	}

	size_t CommandQueue::Pending() const {
		const size_t write = Write.load(std::memory_order_acquire);
		const size_t read = Read.load(std::memory_order_acquire);
		return write - read;
	}
}
