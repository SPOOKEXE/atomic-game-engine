#include <engine/world/ViewChannel.hpp>

#include <algorithm>

namespace engine::world {

	ViewChannel::ViewChannel(size_t maximumPayload) : Maximum(maximumPayload) {
		for (Slot &slot : Slots) {
			// Reserved once, at the maximum, so publishing never allocates.
			// A producer that hit the allocator inside its own render phase
			// would be paying for it every frame.
			slot.Payload.reserve(maximumPayload);
		}
	}

	bool ViewChannel::Publish(const ViewHeader &header, std::span<const std::byte> payload) {
		if (payload.size() > Maximum) {
			return false;
		}

		// The slot that is neither published nor being read. With three slots
		// and at most one in each of those two states, there is always exactly
		// one left.
		const uint32_t published = Published_.load(std::memory_order_acquire);
		const uint32_t holding = Holding.load(std::memory_order_acquire);

		uint32_t write = 0;
		for (uint32_t index = 0; index < SLOTS; index++) {
			if (index != published && index != holding) {
				write = index;
				break;
			}
		}

		Slot &slot = Slots[write];
		slot.Header = header;
		slot.Header.Serial = NextSerial++;
		slot.Header.PayloadBytes = static_cast<uint32_t>(payload.size());
		slot.Payload.assign(payload.begin(), payload.end());

		// The release pairs with the consumer's acquire: everything written
		// above is visible to whoever takes this slot.
		const uint32_t previous = Published_.exchange(write, std::memory_order_release);

		Count.fetch_add(1, std::memory_order_relaxed);
		if (previous != NONE) {
			// A frame was sitting published and nobody took it. Dropping is the
			// design — a slow compositor must not throttle a simulation — but
			// it is counted, because a figure that climbs is worth seeing.
			Drops.fetch_add(1, std::memory_order_relaxed);
		}

		return true;
	}

	bool ViewChannel::Acquire(ViewHeader &header, std::vector<std::byte> &payload) {
		for (;;) {
			const uint32_t ready = Published_.load(std::memory_order_acquire);
			if (ready == NONE) {
				return false;
			}

			// Claimed *before* the published slot is cleared. The other order
			// would leave a window in which the slot belonged to neither side,
			// and the producer would be free to overwrite it mid-read.
			Holding.store(ready, std::memory_order_release);

			uint32_t expected = ready;
			if (Published_.compare_exchange_strong(
					expected, NONE, std::memory_order_acq_rel, std::memory_order_acquire
				)) {
				const Slot &slot = Slots[ready];
				header = slot.Header;
				payload.assign(slot.Payload.begin(), slot.Payload.end());

				Holding.store(NONE, std::memory_order_release);
				return true;
			}

			// The producer published again between the load and the exchange,
			// so the slot we claimed is stale. Release it and take the newer
			// one — which is the right answer anyway, since the contract is
			// "the newest frame" rather than "some frame".
			Holding.store(NONE, std::memory_order_release);
		}
	}
}
