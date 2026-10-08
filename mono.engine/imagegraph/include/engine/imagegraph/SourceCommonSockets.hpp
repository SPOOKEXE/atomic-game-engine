#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// Borrowed descriptors and captures are consumed synchronously; sessions own
	// their retained values.
	struct SourceCommonOwner {
		std::string_view OwnerId, OwnerType;
		bool Active = true, ShowUpdateTrigger = false, OutMeta = false;
	};
	struct SourceCommonMetadataCapture {
		std::string_view Name;
		Vector2 Position{};
	};
	struct SourceCommonStepCapture {
		std::string_view OwnerId, OwnerType;
		// Captured predicate of the source Update getter, not an inferred readiness
		// or frame pulse.
		std::optional<bool> UpdateRequested{}, DirectUpdateCompleted{};
		std::optional<SourceCommonMetadataCapture> Metadata{};
	};
	struct SourceCommonSocketState {
		std::string OwnerId, OwnerType;
		bool Updated = false;
		std::string Name;
		Vector2 Position{};
		bool operator==(const SourceCommonSocketState &) const = default;
	};
	struct SourceCommonSocketSession {
		std::vector<SourceCommonSocketState> Owners;
		bool operator==(const SourceCommonSocketSession &) const = default;
	};
	struct SourceCommonStepReceipt {
		std::string OwnerId;
		bool ResetUpdateInput = true;
		bool operator==(const SourceCommonStepReceipt &) const = default;
	};
	struct SourceCommonFullUpdateCapture {
		std::string_view OwnerId, OwnerType;
		bool SafeMode = false;
		// Required true outside safe mode: the source full-update wrapper reached its
		// final assignment.
		std::optional<bool> Completed{};
	};
	uint64_t RetainedSourceCommonSocketBytes(const SourceCommonSocketSession &session);
	uint64_t RetainedSourceCommonReceiptBytes(const std::vector<SourceCommonStepReceipt> &receipts);
	// Explicit construction/load resets output sockets to source constructor
	// values.
	Status InitializeSourceCommonSockets(
		std::span<const SourceCommonOwner> owners,
		SourceCommonSocketSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Replays one ordered capture per active owner. It does not execute direct
	// update callbacks or reset authored animators. Reset receipts report the
	// proven source setter action for a later bridge.
	Status BeginSourceCommonStep(
		std::span<const SourceCommonOwner> owners,
		std::span<const SourceCommonStepCapture> captures,
		SourceCommonSocketSession &session,
		std::vector<SourceCommonStepReceipt> &receipts,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Independent from BeginStep and readiness. Cache recovery and hidden trigger
	// sockets still pulse when the captured full-update wrapper completes;
	// safe-mode return leaves the old pulse unchanged.
	Status CompleteSourceCommonFullUpdates(
		std::span<const SourceCommonFullUpdateCapture> captures,
		SourceCommonSocketSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
} // namespace engine::imagegraph
