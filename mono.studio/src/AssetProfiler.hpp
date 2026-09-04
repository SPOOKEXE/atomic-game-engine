#pragma once

// Byte accounting for Studio's content profiler.
//
// The renderer owns the actual resources. These helpers describe the decoded
// payload handed to it, so the content pump and the panel share one definition
// of what a mesh or texture costs.

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace studio {
	struct AssetFootprint {
		uint64_t DecodedBytes = 0;
		uint64_t CpuResidentBytes = 0;
		uint64_t GpuResidentBytes = 0;
	};

	AssetFootprint MeshFootprint(const engine::assets::MeshData &mesh);
	AssetFootprint TextureFootprint(const engine::assets::TextureData &texture);
}
