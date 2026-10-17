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
		// Minimum corner of the root bounds in tornado-local coordinates.
		core::Vector3 RootMinimum{-512.0f, 0.0f, -512.0f};
		// Positive width, height, and depth of the root bounds.
		core::Vector3 RootSize{1024.0f, 512.0f, 1024.0f};
		// Maximum subdivision depth, which sets the finest voxel size.
		uint8_t MaximumDepth = 8;
		// Values at or below this density are treated as empty.
		float EmptyThreshold = 1.0e-4f;
	};

	// One node in the packed octree representation consumed by a volume renderer.
	struct alignas(16) CloudDensityGpuNode {
		// Child indices for octants zero through three; UINT32_MAX marks an absent child.
		std::array<uint32_t, 4> ChildrenLow{};
		// Child indices for octants four through seven; UINT32_MAX marks an absent child.
		std::array<uint32_t, 4> ChildrenHigh{};
		// The node density is in element zero; remaining elements are reserved.
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
		// Returns the bounds and depth settings this tree was created with.
		[[nodiscard]] const CloudDensityOctreeConfig &Config() const {
			return config_;
		}
		// Returns the size of one voxel at the configured maximum depth.
		[[nodiscard]] core::Vector3 VoxelSize() const;
		// Returns the recursively averaged density stored at the root.
		[[nodiscard]] float AverageDensity() const;
		// Returns the number of currently active octree nodes.
		[[nodiscard]] size_t NodeCount() const {
			return activeNodeCount_;
		}
		// Returns the number of occupied leaf voxels.
		[[nodiscard]] size_t StoredVoxelCount() const {
			return storedVoxelCount_;
		}
		// Packs nodes for the renderer, retaining slots left by erased branches.
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
