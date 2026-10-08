#pragma once
#include <engine/imagegraph/SourceCommonSockets.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>

namespace engine::imagegraph {
	enum class SourceFrameActivity : uint8_t { Unknown, Static, FrameDriven };
	enum class SourceGroupPurity : uint8_t { Unknown, Nonpure, Pure };
	enum class SourceNodeInitialState : uint8_t { Loaded, Constructed };
	enum class SourcePurityRefreshEvent : uint8_t {
		LoadTopology,
		Membership,
		AnimationMode,
		PureFunction,
		InputOutput
	};
	struct SourcePurityRefresh {
		SourcePurityRefreshEvent Event = SourcePurityRefreshEvent::LoadTopology;
		// Empty refreshes all groups. The host issues actual source lifecycle events, not revisions.
		std::span<const std::string_view> Groups{};
	};
	struct GroupRenderPurity {
		std::string GroupId, ParentId, OwnerNodeId, InstanceBase;
		bool AuthoredPureFunction = true;
		SourceGroupPurity State = SourceGroupPurity::Nonpure;
		bool operator==(const GroupRenderPurity &) const = default;
	};
	// Readiness resets independently of the constructor or most recently published socket value.
	struct GroupRenderReadiness {
		std::string NodeId;
		bool Rendered = false;
		SourceFrameActivity FrameActivity = SourceFrameActivity::Unknown;
		// Membership snapshot invalidates cached purity after structural replacement.
		std::string GroupId, InstanceBase, SourceParentInputBase;
		bool operator==(const GroupRenderReadiness &) const = default;
	};
	// Caller-owned process history. Native documents serialize authored flags, never these payloads.
	struct SourceCommonOwnerSnapshot {
		std::string SourceOwnerId, SourceType;
		SourceCommonNativeOwnerKind NativeOwnerKind = SourceCommonNativeOwnerKind::Node;
		std::string NativeOwnerId;
		bool operator==(const SourceCommonOwnerSnapshot &) const = default;
	};
	struct SourceCommonWriterState {
		std::string OwnerId, Port;
		bool Modified = false, ForceDynamic = false, Edited = false;
		bool operator==(const SourceCommonWriterState &) const = default;
	};
	// A source getInputs capture, distinct from current authored controls. Missing data is explicit.
	struct SourceCommonInputMap {
		std::string OwnerId, OwnerType;
		std::optional<StructValue> Inputs;
		bool operator==(const SourceCommonInputMap &) const = default;
	};
	struct GroupRenderSession {
		CacheGroupReplayState Outputs;
		std::vector<GroupRenderReadiness> Nodes;
		StatefulOutputEvaluationResult Replay;
		std::vector<GroupRenderPurity> Purities;
		SourceCommonSocketSession Common;
		SourceAnimatorState CommonAnimators;
		std::vector<SourceCommonWriterState> SourceCommonWrites;
		std::vector<SourceCommonOwnerSnapshot> SourceCommonBindings;
		std::vector<SourceCommonInputMap> SourceCommonInputs;
		bool Ready(std::string_view nodeId) const;
	};
	enum class GroupRenderMode : uint8_t { AutomaticFull, AutomaticPartial, ForceGroup, RefreshPurity };
	struct GroupRenderOperation {
		GroupRenderMode Mode = GroupRenderMode::AutomaticFull;
		std::string_view GroupId{};
		// Partial seeds admitted by the host's dirty/dynamic policy. Unrendered nodes also seed work.
		std::span<const std::string_view> AffectedNodes{};
		// Constructed is a constructor observation, not a source-step receipt. Loaded ASE/audio/canvas
		// activity remains unknown where resource/step observations are unavailable.
		SourceNodeInitialState InitialState = SourceNodeInitialState::Loaded;
		std::optional<SourcePurityRefresh> PurityRefresh{};
	};
	uint64_t RetainedGroupRenderSessionBytes(const GroupRenderSession &session);
	// Full processing clears rendered bits. Partial processing retains readiness and processes only
	// unrendered nodes, explicit dirty/dynamic seeds and their ready downstream consumers.
	// ForceGroup performs the source wrapper's direct update: pure children run, nonpure children wait
	// for automatic scheduling. Refusal preserves the entire previous session.
	Status ProcessGroupRender(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const GroupRenderOperation &operation,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Refresh the source Collection's separately cached purity without executing children. Activity
	// changes and ordinary process pulses never imply this lifecycle event.
	Status RefreshSourceGroupPurity(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourcePurityRefresh &refresh,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Observe a held socket without processing its producer or changing readiness.
	Status ReadGroupRenderOutput(
		const Document &document,
		std::string_view outputId,
		const GroupRenderSession &session,
		CacheGroupReplayOutput &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
