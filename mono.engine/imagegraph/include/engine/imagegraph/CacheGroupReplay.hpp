#pragma once

// Caller-owned source cache-group activity and named producer outputs. This journal supplies policy;
// the evaluator must restore frozen outputs and the host must apply admitted frame-clear requests.

#include <engine/imagegraph/Document.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph {
	// SurfaceValue and ArrayValue retain image pixels and nested surface arrays without device handles.
	struct CacheGroupReplayOutput {
		std::string Port;
		std::optional<Value> Data;
		std::optional<SourceSocketDomain> Domain;
		std::optional<Diagnostic> Refusal;
		bool operator==(const CacheGroupReplayOutput &) const = default;
	};
	// Outputs start with the node's actual constructor values, then retain its latest published values.
	struct CacheGroupReplayNode {
		std::string NodeId, NodeType, OwnerId;
		bool RenderActive = true;
		std::vector<CacheGroupReplayOutput> Outputs;
		bool operator==(const CacheGroupReplayNode &) const = default;
	};
	// Loaded lists may overlap; OwnerId is the separate last-refreshed member pointer.
	struct CacheGroupReplayOwner {
		std::string NodeId;
		bool Serialize = true;
		std::vector<std::string> Members;
		bool operator==(const CacheGroupReplayOwner &) const = default;
	};
	struct CacheGroupReplayState {
		std::vector<CacheGroupReplayNode> Nodes;
		std::vector<CacheGroupReplayOwner> Owners;
		bool operator==(const CacheGroupReplayState &) const = default;
	};
	enum class CacheGroupReplayAction : uint8_t {
		RefreshOwner,
		TransferMember,
		RemoveMember,
		SetSerialize,
		Enable,
		Disable,
		DestroyOwner
	};
	struct CacheGroupReplayOperation {
		CacheGroupReplayAction Action = CacheGroupReplayAction::RefreshOwner;
		std::string_view OwnerId, MemberId;
		std::span<const std::string> Members;
		bool Serialize = true, Playing = false;
		SourceFrameCacheProjectObservation Project{};
	};
	struct CacheGroupReplayChange {
		Status Code = Status::Ok;
		// Admitted Enable, including owner destruction, force-clears that owner's captured frames.
		bool ClearCapturedFrames = false;
		Diagnostic Error;
	};
	uint64_t RetainedCacheGroupReplayBytes(const CacheGroupReplayState &state);
	[[nodiscard]] Status ValidateCacheGroupReplay(
		const CacheGroupReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	);
	// Upserts one complete named output snapshot. Existing activity and owner pointers survive.
	// Node-type changes require a new journal, so a frozen snapshot cannot acquire another kernel's type.
	[[nodiscard]] CacheGroupReplayChange RetainCacheGroupReplayNode(
		CacheGroupReplayState &state,
		std::string_view nodeId,
		std::string_view nodeType,
		std::span<const CacheGroupReplayOutput> outputs,
		uint64_t maximumBytes
	);
	// Mutations publish atomically and charge prior state plus candidate state before cloning outputs.
	[[nodiscard]] CacheGroupReplayChange ApplyCacheGroupReplay(
		CacheGroupReplayState &state, const CacheGroupReplayOperation &operation, uint64_t maximumBytes
	);
	// Load-time authored refresh, in document owner order. Existing producer outputs/activity survive.
	// New producers receive exact supported constructor outputs or explicit runtime-constructor refusals.
	// This is not a per-frame refresh or an interactive transfer. Reconcile replaced types before loading.
	[[nodiscard]] Status InitializeAuthoredCacheGroupReplay(
		const Document &document,
		const CacheGroupReplayState &source,
		CacheGroupReplayState &output,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	);
	// Retires missing or type-replaced producers and owner lists. Retiring an enabled owner requires
	// authoritative loading/appending observations before waking its surviving members.
	[[nodiscard]] Status ReconcileCacheGroupReplay(
		const Document &document,
		const CacheGroupReplayState &source,
		CacheGroupReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	);
	// The explicit source renderList bypasses ordinary activity gates. Untracked nodes are active.
	bool CacheGroupReplayShouldRun(
		const CacheGroupReplayState &state, std::string_view nodeId, bool explicitRenderList = false
	);
}
