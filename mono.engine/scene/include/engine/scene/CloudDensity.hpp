#pragma once

// Owned packed cloud-density data passed to a volume renderer.
//
// @tier L7 · shared

#include <engine/scene/CloudDensityOctree.hpp>

#include <vector>

namespace engine::scene {

	// An owned, renderer-facing cloud field. Coordinates are volume-local;
	// Centre translates them into the presented world's coordinates.
	struct CloudDensitySnapshot {
		// World-space position of the volume-local field origin.
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

}
