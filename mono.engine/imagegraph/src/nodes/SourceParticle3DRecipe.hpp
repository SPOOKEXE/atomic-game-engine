#pragma once

#include "Path3D.hpp"
#include "SourceParticle3DState.hpp"

#include <memory>

namespace engine::imagegraph::detail {
	// Stack-owned for one processor row. Controls borrow these buffers and the resolved context values.
	struct SourceParticle3DPrepared {
		AllocationReservation Charge;
		std::vector<Colour> Palette;
		std::vector<Vector3> SpawnData;
		std::vector<std::vector<Vector3>> SpawnMesh;
		std::vector<std::span<const Vector3>> SpawnMeshViews;
		std::unique_ptr<PathData3D> SpawnPlanar, FollowPlanar;
		std::unique_ptr<PathRuntime3D> SpawnRuntime, FollowRuntime;
		SourceParticle3DControls Controls;
		SourceParticle3DPrepared() = default;
		SourceParticle3DPrepared(const SourceParticle3DPrepared &) = delete;
		SourceParticle3DPrepared &operator=(const SourceParticle3DPrepared &) = delete;
		SourceParticle3DPrepared(SourceParticle3DPrepared &&) = delete;
		SourceParticle3DPrepared &operator=(SourceParticle3DPrepared &&) = delete;
	};
	// Requires a fresh prepared object. Failure publishes nothing; caller discards its partial preparation.
	bool PrepareSourceParticle3DControls(NodeContext &, SourceParticle3DPrepared &);
}
