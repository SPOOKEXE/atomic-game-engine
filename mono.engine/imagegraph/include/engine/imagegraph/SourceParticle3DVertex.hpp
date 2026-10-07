#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Particle3D.hpp>

namespace engine::imagegraph {
	struct SourceParticle3DVertex {
		Vector3 Position{}, Normal{};
		Vector2 UV{};
		std::array<double, 4> Colour{};
		// Inactive shader instances return a degenerate clip position, not ordinary geometry.
		bool Active = false;
	};
	// Row-major HLSL objectTransform, applied only to position. Camera is the source uniform vector.
	// This stage ends before world/view/projection and preserves the shader's unnormalized normal.
	// Failure leaves output unchanged; no geometry allocation or successful node execution is claimed.
	bool PrepareSourceParticle3DVertex(
		const MeshVertex3D &vertex,
		const MeshInstance3D &transform,
		const ParticleRecord3D &particle,
		const std::array<double, 16> &objectTransform,
		Vector3 cameraPosition,
		SourceParticle3DVertex &output
	);
}
