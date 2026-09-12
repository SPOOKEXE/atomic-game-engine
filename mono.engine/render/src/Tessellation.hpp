#pragma once

// CPU-side contract for the buffers tessellate.comp consumes.  The shader uses
// uint words rather than GLSL vec3 fields so std430 cannot add hidden padding.

#include <engine/render/MeshTable.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace engine::render {
	inline constexpr uint32_t TESSELLATION_VERTEX_WORDS = 12;
	inline constexpr uint32_t TESSELLATION_VERTEX_BYTES = TESSELLATION_VERTEX_WORDS * sizeof(uint32_t);
	inline constexpr uint32_t TESSELLATION_COMMAND_WORDS = 5;

	struct GpuTessellationVertex {
		uint32_t Words[TESSELLATION_VERTEX_WORDS]{};
	};
	static_assert(sizeof(GpuTessellationVertex) == TESSELLATION_VERTEX_BYTES);

	// One material range of one draw slot.  Offsets are element offsets in the
	// shared resident and generated streams, never byte addresses.
	struct GpuTessellationPlan {
		uint32_t SourceFirstIndex = 0;
		uint32_t SourceIndexCount = 0;
		int32_t SourceVertexOffset = 0;
		uint32_t Material = 0;
		uint32_t Instance = 0;
		uint32_t Factor = 1;
		uint32_t OutputFirstVertex = 0;
		uint32_t OutputFirstIndex = 0;
		uint32_t Command = 0;
	};
	static_assert(sizeof(GpuTessellationPlan) == 36);

	struct TessellationCapacity {
		uint32_t Vertices = 0;
		uint32_t Indices = 0;
		uint32_t Commands = 0;
	};

	struct TessellationPlan {
		std::vector<GpuTessellationPlan> Entries;
		uint32_t VertexCount = 0;
		uint32_t IndexCount = 0;

		static constexpr uint32_t VerticesPerTriangle(uint32_t factor) {
			return factor == 0 || factor > 65534 ? 0 : (factor + 1) * (factor + 2) / 2;
		}
		static constexpr uint32_t IndicesPerTriangle(uint32_t factor) {
			return factor == 0 || factor > 37837 ? 0 : factor * factor * 3;
		}

		bool
		Add(const MeshRange &source,
			uint32_t material,
			uint32_t instance,
			uint32_t factor,
			TessellationCapacity capacity) {
			if (source.IndexCount == 0 || source.IndexCount % 3 != 0 || factor == 0 ||
				Entries.size() >= capacity.Commands)
				return false;
			const uint64_t triangles = source.IndexCount / 3;
			const uint64_t vertices = triangles * VerticesPerTriangle(factor);
			const uint64_t indices = triangles * IndicesPerTriangle(factor);
			if (vertices == 0 || indices == 0 || vertices > capacity.Vertices - VertexCount ||
				indices > capacity.Indices - IndexCount)
				return false;
			Entries.push_back(
				{source.FirstIndex,
				 source.IndexCount,
				 source.VertexOffset,
				 material,
				 instance,
				 factor,
				 VertexCount,
				 IndexCount,
				 static_cast<uint32_t>(Entries.size())}
			);
			VertexCount += static_cast<uint32_t>(vertices);
			IndexCount += static_cast<uint32_t>(indices);
			return true;
		}
	};
}
