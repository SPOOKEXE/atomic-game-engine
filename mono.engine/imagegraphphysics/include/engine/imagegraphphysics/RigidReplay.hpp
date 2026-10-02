#pragma once

#include <engine/imagegraph/SourceRigid.hpp>

namespace engine::imagegraphphysics {
	// Rebuilds all recorded steps so source seeks preserve solver contact history.
	// No vendor IDs or pointers escape this synchronous bounded host operation.
	imagegraph::Status ReplayRigid(
		const imagegraph::SourceRigidHistory &history,
		uint64_t tick,
		imagegraph::SourceRigidSnapshot &output,
		imagegraph::Diagnostic &diagnostic,
		std::optional<imagegraph::SourceRigidEventPosition> captureAt = {},
		uint64_t maximumSnapshotBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	class RigidProvider final : public imagegraph::SourceRigidProvider {
	  public:
		imagegraph::Status Replay(
			const imagegraph::SourceRigidHistory &history,
			uint64_t tick,
			std::optional<imagegraph::SourceRigidEventPosition> captureAt,
			uint64_t maximumSnapshotBytes,
			imagegraph::SourceRigidSnapshot &output,
			imagegraph::Diagnostic &diagnostic
		) override;
	};
}
