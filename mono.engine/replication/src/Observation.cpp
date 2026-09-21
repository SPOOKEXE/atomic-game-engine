#include <engine/replication/Observation.hpp>

namespace engine::replication {

	const char *Describe(ReplicationHook hook) {
		switch (hook) {
		case ReplicationHook::AuthorityPublished:
			return "replication.authority.published";
		case ReplicationHook::AuthorityReceived:
			return "replication.authority.received";
		case ReplicationHook::AuthorityApplied:
			return "replication.authority.applied";
		case ReplicationHook::AuthorityRejected:
			return "replication.authority.rejected";
		case ReplicationHook::AuthorityRepaired:
			return "replication.authority.repaired";
		case ReplicationHook::AuthorityDropped:
			return "replication.authority.dropped";
		case ReplicationHook::ReplicaApplied:
			return "replication.replica.applied";
		case ReplicationHook::ReplicaRejected:
			return "replication.replica.rejected";
		}
		return "replication.unknown";
	}

	ReplicationObservations::ReplicationObservations() {
		for (size_t index = 0; index < Slots.size(); index++) {
			Slots[index].Sequence.store(index, std::memory_order_relaxed);
		}
	}

	bool ReplicationObservations::Record(ReplicationObservationRecord record) {
		size_t position = Enqueue.load(std::memory_order_relaxed);
		for (;;) {
			Slot &slot = Slots[position % Slots.size()];
			const size_t sequence = slot.Sequence.load(std::memory_order_acquire);
			const ptrdiff_t difference = static_cast<ptrdiff_t>(sequence) - static_cast<ptrdiff_t>(position);
			if (difference == 0) {
				if (Enqueue.compare_exchange_weak(
						position, position + 1, std::memory_order_relaxed, std::memory_order_relaxed
					)) {
					slot.Record.emplace(std::move(record));
					slot.Sequence.store(position + 1, std::memory_order_release);
					return true;
				}
				continue;
			}
			if (difference < 0) {
				DroppedRecords.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			position = Enqueue.load(std::memory_order_relaxed);
		}
	}

	std::optional<ReplicationObservationRecord> ReplicationObservations::Poll() {
		Slot &slot = Slots[Dequeue % Slots.size()];
		if (slot.Sequence.load(std::memory_order_acquire) != Dequeue + 1) {
			return std::nullopt;
		}

		std::optional<ReplicationObservationRecord> record = std::move(slot.Record);
		slot.Record.reset();
		slot.Sequence.store(Dequeue + Slots.size(), std::memory_order_release);
		Dequeue++;
		return record;
	}

	void ReplicationObservations::Clear() {
		while (Poll().has_value()) {}
		DroppedRecords.store(0, std::memory_order_relaxed);
	}

	uint64_t ReplicationObservations::Dropped() const {
		return DroppedRecords.load(std::memory_order_relaxed);
	}
}
