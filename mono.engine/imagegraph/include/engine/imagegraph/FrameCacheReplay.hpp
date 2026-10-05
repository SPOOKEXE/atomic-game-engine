#pragma once
#include <engine/imagegraph/DataReplay.hpp>

namespace engine::imagegraph {
	inline constexpr uint64_t SOURCE_FRAME_CACHE_EDIT_WORK_BYTES = 64ull * 1024 * 1024;
	// Private replay tags identify the source node type without adding authored schema fields.
	std::string_view SourceFrameCacheRowType(const DataReplayEntry &entry);
	// grug keep owned row identity when Serialize changes; only constructor loading uses the flag.
	std::string_view SourceFrameCacheIdentity(const Node &node);
	// Empty when serialization is disabled or no typed saved payload is authored.
	std::string_view SourceFrameCacheSavedText(const Node &node);
	const Value *SourceFrameCacheLastOutput(const DataReplayEntry &entry);
	// Force-clear captured slots, retaining latest output and exact saved-source identity.
	// An empty constructor row prevents the authored receipt from loading again.
	uint64_t ClearedSourceFrameCacheReplayBytes(const Node &node, const DataReplayState &source);
	Status ClearSourceFrameCacheReplay(
		const Node &node,
		const DataReplayState &source,
		DataReplayState &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Source button action applies Cache/Cache Array clear guards, then enables retained members.
	// Nonempty enabled groups require authoritative loading/appending observations. An empty journal
	// has no frozen members; initialize authored groups before supplying an existing partial journal.
	Status ClearSourceFrameCacheButtonReplay(
		const Node &node,
		const DataReplayState &source,
		DataReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Branch-selected Enable clears every owner row before a caller's subsequent auto capture;
	// Disable keeps captured frames and changes activity only at the authoritative playing endpoint.
	// The caller owns scheduler ordering. Both operations retain latest getters and publish atomically.
	Status ApplySourceFrameCacheGroupReplay(
		const Node &node,
		CacheGroupReplayAction action,
		const DataReplayState &source,
		DataReplayState &output,
		bool playing,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Input and connection edits wake the member's current owner, then enclosing source groups.
	// Overlapping membership lists do not select additional owners. All selected owner actions
	// share one work cap and publish together, preserving latest outputs and unrelated histories.
	[[nodiscard]] Status EnableSourceFrameCacheEditedGroups(
		const Document &document,
		std::span<const std::string_view> editedNodes,
		const DataReplayState &source,
		DataReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// grug stage authored membership, Serialize flags and input Enable together for one or two journals.
	// native restoration retires omitted owner pointers without waking members. source load refresh stays
	// separate. caller subtracts document/host residency; all stages share the supplied bounded
	// comparison-work allowance.
	[[nodiscard]] Status SynchronizeSourceFrameCacheEdits(
		const Document &document,
		std::span<const std::string_view> editedNodes,
		std::span<const std::string_view> membershipOwners,
		std::span<const std::string_view> serializeOwners,
		std::span<DataReplayState *const> journals,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes,
		uint64_t maximumWork = SOURCE_FRAME_CACHE_EDIT_WORK_BYTES
	);
	inline constexpr std::string_view SOURCE_FRAME_CACHE_NATIVE_TEXT = "composer_frame_cache_text";
	inline constexpr std::string_view SOURCE_FRAME_CACHE_NATIVE_DATA = "composer_frame_cache_data";
	// Native constructor receipts carry named node types, sparse frame indices and
	// RGBA8 bytes in bounded text chunks. No foreign decoder is needed at runtime.
	Status EncodeSourceFrameCacheReceipt(
		const DataReplayEntry &row,
		ArrayValue &chunks,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Check the complete native packet without allocating decoded pixels or tables.
	Status MeasureSourceFrameCacheReceipt(const Node &node, uint64_t &decodedBytes, Diagnostic &diagnostic);
	// Decode only a complete receipt bound to the current exact source cache text.
	// Failure preserves the caller's previous row.
	Status DecodeSourceFrameCacheReceipt(
		const Node &node,
		DataReplayEntry &row,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	enum class FrameCacheOutputPolicy : uint8_t { RetainedObservation, PreObservation, Constructor };
	// Merge captured frame histories transactionally. PreObservation keeps the target's earlier
	// output, while Constructor resets only latest output for an explicit native played-prefix seek.
	Status OverlaySourceFrameCacheRows(
		const Document &document,
		const DataReplayState &retained,
		DataReplayState &target,
		FrameCacheOutputPolicy policy,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
