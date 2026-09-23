#pragma once

// Analytical and adaptive representations of the storm cloud volume.
//
// The analytical function is the authoritative query path. A renderer can
// rebuild an octree from it without causing gameplay visibility to depend on a
// cache update or allocation order.
//
// @tier L7 · shared

#include <engine/scene/CloudDensityOctree.hpp>
#include <engine/scene/Storm.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	// Controls the coarse outer volume and funnel-local refinement.
	struct CloudDensityBuildConfig {
		uint8_t CoarseDepth = 5;
		uint8_t FineDepth = 7;
		float FineRadiusInCoreRadii = 2.5f;
		float MinimumDensity = 0.026f;
	};

	// Counts produced by one adaptive cloud-density rebuild.
	struct CloudDensityBuildStats {
		size_t EvaluatedCells = 0;
		size_t CoarseCells = 0;
		size_t FineCells = 0;
		size_t StoredLeaves = 0;
		size_t Nodes = 0;
	};

	// Maps tornado-local airflow into normalized visible moisture.
	[[nodiscard]] float WindLineCloudDensity(
		const PreparedTornadoField &field, const core::Vector3 &localPosition, float timeSeconds
	);

	// Rebuilds the tree in tornado-local coordinates with funnel-local refinement.
	[[nodiscard]] CloudDensityBuildStats BuildCloudDensity(
		CloudDensityOctree &tree,
		const PreparedTornadoField &field,
		float timeSeconds,
		CloudDensityBuildConfig config = {}
	);
}
