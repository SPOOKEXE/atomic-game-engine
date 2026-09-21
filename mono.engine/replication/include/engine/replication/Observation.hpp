#pragma once

// Read-only replication exchange observations.
//
// Records carry only copied exchange metadata. Wire payloads and store rows
// stay behind their existing authority and permission boundaries.
//
// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Replica.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace engine::replication {

	// Declared replication exchange observation points. The string names are the
	// discovery and MCP contract. Do not serialize this enum's numeric value.
	enum class ReplicationHook : uint8_t {
		AuthorityPublished,
		AuthorityReceived,
		AuthorityApplied,
		AuthorityRejected,
		AuthorityRepaired,
		AuthorityDropped,
		ReplicaApplied,
		ReplicaRejected,
	};

	// Returns the stable discovery name for one hook.
	const char *Describe(ReplicationHook hook);

	// Copied identity for one exchange. A field is meaningful only when its
	// matching availability flag is true. Names are interned values whose text
	// remains stable for the process lifetime.
	struct ExchangeIdentity {
		const core::Name World{};
		const core::Name Authority{};
		const ClientId Client{};
		const uint64_t Baseline = 0;
		const uint64_t Tick = 0;
		const uint64_t Round = 0;
		const bool BaselineAvailable = false;
		const bool TickAvailable = false;
	};

	// One authority-to-client exchange completed at the publish boundary.
	struct AuthorityPublishedObservation {
		const ExchangeIdentity Identity;
		const uint32_t MessageCount = 0;
		const uint64_t ByteCount = 0;
		const bool Snapshot = false;
	};

	// One valid inbound authority message changed replication state.
	struct AuthorityAppliedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const ApplyStatus Status = ApplyStatus::Ok;
		const uint64_t ByteCount = 0;
		// Values named by the submitted delta and values refused by the ownership
		// or component gate. A successful status may still have refused values.
		const uint32_t ValueCount = 0;
		const uint32_t RefusedCount = 0;
	};

	// One valid inbound message was accepted for authority processing. A client
	// delta remains queued until the host calls ApplySubmitted.
	struct AuthorityReceivedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const uint64_t ByteCount = 0;
	};

	// One inbound authority message was refused. Its payload remains private.
	struct AuthorityRejectedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const ApplyStatus Status = ApplyStatus::Malformed;
		const uint64_t ByteCount = 0;
		const bool MessageAvailable = false;
	};

	// One valid audit answer scheduled rows for the existing recovery walk.
	struct AuthorityRepairedObservation {
		const ExchangeIdentity Identity;
		const uint32_t GroupCount = 0;
		const uint32_t EntityCount = 0;
	};

	// The transport declined an already-built authority message.
	struct AuthorityDroppedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const uint64_t ByteCount = 0;
		const bool MessageAvailable = false;
	};

	// One server message was applied to the replica world at its receive boundary.
	struct ReplicaAppliedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const ApplyStatus Status = ApplyStatus::Ok;
		const uint64_t ByteCount = 0;
	};

	// One server message was rejected before it could change the replica world.
	struct ReplicaRejectedObservation {
		const ExchangeIdentity Identity;
		const MessageKind Message = MessageKind::Applied;
		const ApplyStatus Status = ApplyStatus::Malformed;
		const uint64_t ByteCount = 0;
		const bool MessageAvailable = false;
	};

	using ReplicationObservationContext = std::variant<
		AuthorityPublishedObservation,
		AuthorityReceivedObservation,
		AuthorityAppliedObservation,
		AuthorityRejectedObservation,
		AuthorityRepairedObservation,
		AuthorityDroppedObservation,
		ReplicaAppliedObservation,
		ReplicaRejectedObservation>;

	// A completed, value-only observation. Consumers may inspect the variant but
	// have no path back to an authority, replica, store, or wire payload.
	struct ReplicationObservationRecord {
		const ReplicationHook Hook = ReplicationHook::AuthorityPublished;
		const ReplicationObservationContext Context;
	};

	// Bounded multi-producer, single-consumer exchange record queue. Publishing
	// never waits for a consumer; a full queue drops the newest record and counts
	// it. Its fixed slots avoid per-record allocation in replication workers.
	class ReplicationObservations {
	  public:
		static constexpr size_t MAXIMUM_RECORDS = 256;

		ReplicationObservations();

		// Attempts to append a completed record without blocking.
		bool Record(ReplicationObservationRecord record);

		// Removes the next completed record, if any. One consumer calls Poll.
		std::optional<ReplicationObservationRecord> Poll();

		// Drops completed records and resets the dropped count before a world or
		// session is rebound. Call only after its exchange producers have stopped.
		void Clear();

		// Number of records rejected because the fixed queue was full.
		uint64_t Dropped() const;

	  private:
		struct Slot {
			std::atomic<size_t> Sequence{0};
			std::optional<ReplicationObservationRecord> Record;
		};

		std::array<Slot, MAXIMUM_RECORDS> Slots;
		std::atomic<size_t> Enqueue{0};
		size_t Dequeue = 0;
		std::atomic<uint64_t> DroppedRecords{0};
	};
}
