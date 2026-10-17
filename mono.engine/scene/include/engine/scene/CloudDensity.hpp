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
#include <vector>

namespace engine::scene {

	// Controls the coarse outer volume and funnel-local refinement.
	struct CloudDensityBuildConfig {
		// Octree depth used outside the locally refined region.
		uint8_t CoarseDepth = 5;
		// Octree depth used inside the locally refined region.
		uint8_t FineDepth = 7;
		// Radius of the refined region, measured in tornado core radii.
		float FineRadiusInCoreRadii = 2.5f;
		// Densities below this cutoff are omitted from the octree.
		float MinimumDensity = 0.026f;
	};

	// Counts produced by one adaptive cloud-density rebuild.
	struct CloudDensityBuildStats {
		// Total coarse and fine cell centers evaluated by the density function.
		size_t EvaluatedCells = 0;
		// Coarse-depth cells retained in the tree.
		size_t CoarseCells = 0;
		// Fine-depth cells retained in the tree.
		size_t FineCells = 0;
		// Occupied leaf voxels after the rebuild.
		size_t StoredLeaves = 0;
		// Active octree nodes after the rebuild.
		size_t Nodes = 0;
	};

	// An owned, renderer-facing cloud field. Coordinates are tornado-local;
	// Centre translates them into the presented world's coordinates.
	struct CloudDensitySnapshot {
		// World-space position of the tornado-local field origin.
		core::Vector3 Centre;
		// Bounds and depth settings used to build the packed nodes.
		CloudDensityOctreeConfig Config;
		// Renderer-ready packed octree nodes.
		std::vector<CloudDensityGpuNode> Nodes;

		// Reports whether the snapshot contains packed octree data.
		//
		// @return `true` when at least one node is present.
		[[nodiscard]] bool IsValid() const {
			return !Nodes.empty();
		}
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
