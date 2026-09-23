#pragma once

// Sparse adaptive storage for normalized storm-cloud density.
//
// The tree is in tornado-local coordinates and only retains occupied paths.
// Its compact packed form is renderer-ready data, while gameplay reads remain
// deterministic through `WindLineCloudDensity` in CloudDensity.hpp.
//
// @tier L7 · shared

#include <engine/core/types/Vector3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::scene {

	// Bounds and precision limits for a normalized cloud-density octree.
	struct CloudDensityOctreeConfig {
		core::Vector3 RootMinimum{-512.0f, 0.0f, -512.0f};
		core::Vector3 RootSize{1024.0f, 512.0f, 1024.0f};
		uint8_t MaximumDepth = 8;
		float EmptyThreshold = 1.0e-4f;
	};

	// One node in the packed octree representation consumed by a volume renderer.
	struct alignas(16) CloudDensityGpuNode {
		std::array<uint32_t, 4> ChildrenLow{};
		std::array<uint32_t, 4> ChildrenHigh{};
		std::array<float, 4> Values{};
	};

	static_assert(sizeof(CloudDensityGpuNode) == 48);

	// A sparse voxel octree for normalized cloud density.
	class CloudDensityOctree {
	  public:
		// Creates a tree when its bounds and depth retain distinguishable voxels.
		[[nodiscard]] static std::optional<CloudDensityOctree> Create(CloudDensityOctreeConfig config);

		// Writes a max-depth voxel. Values at or below EmptyThreshold erase it.
		[[nodiscard]] bool SetDensity(const core::Vector3 &position, float density);
		// Writes a density leaf at a requested adaptive depth.
		[[nodiscard]] bool SetDensity(const core::Vector3 &position, float density, uint8_t targetDepth);
		// Returns nullopt outside the root and zero for unoccupied in-bounds space.
		[[nodiscard]] std::optional<float> SampleDensity(const core::Vector3 &position) const;

		// Removes occupied branches while retaining this tree's bounds.
		void Clear();
		[[nodiscard]] const CloudDensityOctreeConfig &Config() const {
			return config_;
		}
		[[nodiscard]] core::Vector3 VoxelSize() const;
		[[nodiscard]] float AverageDensity() const;
		[[nodiscard]] size_t NodeCount() const {
			return activeNodeCount_;
		}
		[[nodiscard]] size_t StoredVoxelCount() const {
			return storedVoxelCount_;
		}
		[[nodiscard]] std::vector<CloudDensityGpuNode> GpuNodes() const;

	  private:
		using NodeIndex = uint32_t;
		static constexpr NodeIndex INVALID_NODE = UINT32_MAX;

		struct Node {
			std::array<NodeIndex, 8> Children{
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE,
				INVALID_NODE
			};
			float Density = 0.0f;
		};

		explicit CloudDensityOctree(CloudDensityOctreeConfig config);
		[[nodiscard]] bool Contains(const core::Vector3 &position) const;
		[[nodiscard]] NodeIndex AllocateNode();
		void ReleaseNode(NodeIndex index);
		void ReleaseSubtree(NodeIndex index);
		void SplitOccupiedLeaf(NodeIndex index);
		[[nodiscard]] bool IsLeaf(NodeIndex index) const;
		void RecomputeDensity(NodeIndex index);
		[[nodiscard]] bool EraseDensity(const core::Vector3 &position, uint8_t targetDepth);

		CloudDensityOctreeConfig config_;
		std::vector<Node> nodes_;
		std::vector<NodeIndex> freeNodes_;
		size_t activeNodeCount_ = 0;
		size_t storedVoxelCount_ = 0;
	};
}
