#pragma once

#include <engine/imagegraph/GroupRenderSession.hpp>

namespace engine::imagegraph {
	// Actual runtime names are needed only when the source archive did not contain a saved name.
	struct SourceCommonRuntimeMetadataObservation {
		std::string_view OwnerId, SourceType, RuntimeName;
	};
	// Collection overrides the base common phase. Missing pending-flag observations are not a no-op.
	struct SourceCommonRuntimeCollectionStepObservation {
		std::string_view OwnerId, SourceType;
		bool RefreshNodesPending = false;
		bool RefreshNodeDisplayPending = false;
		bool NodeDisplayRefreshHandled = false;
		std::optional<SourcePurityRefresh> Refresh{};
	};
	struct SourceCommonRuntimeObservations {
		std::span<const SourceCommonRuntimeMetadataObservation> Metadata{};
		std::span<const SourceCommonRuntimeCollectionStepObservation> Collections{};
	};
	// Explicit reconstruction starts outputs/common sockets cold and resets animator writes/readiness.
	// Supplied canonical temporal journals are retained independently.
	Status InitializeNativeSourceCommonRuntime(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		SourceNodeInitialState initialState,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	struct SourceCommonWriterIdentity {
		std::string_view OwnerId, Port;
	};
	struct SourceCommonRuntimeReconcile {
		SourceNodeInitialState NewOwnerState = SourceNodeInitialState::Constructed;
		// Explicit edited writer identities replace their retained payload with current authored storage.
		std::span<const SourceCommonWriterIdentity> ResetWriters{};
	};

	// Explicit structural edits preserve matching owners, construct new owners, and adopt source order.
	Status ReconcileNativeSourceCommonRuntime(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourceCommonRuntimeReconcile &operation,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

	// Executes active source owners in durable source order. Failure preserves the entire session.
	// Display refresh and dummy-input presentation remain host duties; their success is not inferred.
	Status NativeSourceStepBounded(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourceCommonRuntimeObservations &observations,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

	// Observes the current candidate getter without processing its owner or changing readiness.
	Status ReadNativeSourceCommonGetter(
		const Document &document,
		const Plan &plan,
		std::string_view ownerId,
		SourceCommonSelector selector,
		const EvaluationRequest &request,
		const GroupRenderSession &session,
		EvaluatedValue &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
