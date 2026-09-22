#pragma once

// Durable source-side decisions for a sealed portal handoff.
//
// A record is written between simulation ticks. Replaying an existing record
// only acknowledges the matching receipt, so a crash after fsync cannot create
// a second destination body.

#include <engine/script/PortalTransfer.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace server {

	// The terminal action authorized by a sealed source decision.
	enum class PortalJournalOutcome : uint8_t { Commit = 1, Completed = 2 };

	struct PortalJournalRecord {
		engine::script::PortalTransferDecision Decision;
		PortalJournalOutcome Outcome = PortalJournalOutcome::Commit;
	};

	// Reads every complete, checksummed record. A torn final append is ignored;
	// corruption before the tail refuses recovery rather than skipping history.
	bool LoadPortalJournal(const std::filesystem::path &path, std::vector<PortalJournalRecord> &records);
	// Appends and flushes all records before returning success.
	bool AppendPortalJournal(const std::filesystem::path &path, std::span<const PortalJournalRecord> records);
	// Identifies one decision independently of local entity handles.
	bool PortalJournalContains(
		std::span<const PortalJournalRecord> records, const engine::script::PortalTransferDecision &decision
	);
	// A completed record is historical; only Commit records authorize a source acknowledgement.
	bool PortalJournalPending(
		std::span<const PortalJournalRecord> records, const engine::script::PortalTransferDecision &decision
	);
}
