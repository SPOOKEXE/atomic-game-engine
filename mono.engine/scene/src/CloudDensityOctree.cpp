#include <engine/scene/CloudDensityOctree.hpp>

#include <cmath>
#include <stdexcept>

namespace engine::scene {

	namespace {
		constexpr uint8_t MAXIMUM_SUPPORTED_DEPTH = 20;

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool Positive(const core::Vector3 &value) {
			return value.X > 0.0f && value.Y > 0.0f && value.Z > 0.0f;
		}

		bool Resolvable(const CloudDensityOctreeConfig &config, const core::Vector3 &rootMaximum) {
			const float divisions = static_cast<float>(uint32_t{1} << config.MaximumDepth);
			const core::Vector3 voxelSize = config.RootSize / divisions;
			return config.RootMinimum.X + voxelSize.X > config.RootMinimum.X &&
				   config.RootMinimum.Y + voxelSize.Y > config.RootMinimum.Y &&
				   config.RootMinimum.Z + voxelSize.Z > config.RootMinimum.Z &&
				   rootMaximum.X - voxelSize.X < rootMaximum.X &&
				   rootMaximum.Y - voxelSize.Y < rootMaximum.Y && rootMaximum.Z - voxelSize.Z < rootMaximum.Z;
		}

		uint8_t SelectOctant(const core::Vector3 &position, core::Vector3 &minimum, core::Vector3 &maximum) {
			const core::Vector3 middle = (minimum + maximum) * 0.5f;
			uint8_t octant = 0;
			if (position.X >= middle.X) {
				octant |= 1;
				minimum.X = middle.X;
			} else
				maximum.X = middle.X;
			if (position.Y >= middle.Y) {
				octant |= 2;
				minimum.Y = middle.Y;
			} else
				maximum.Y = middle.Y;
			if (position.Z >= middle.Z) {
				octant |= 4;
				minimum.Z = middle.Z;
			} else
				maximum.Z = middle.Z;
			return octant;
		}
	}

	std::optional<CloudDensityOctree> CloudDensityOctree::Create(CloudDensityOctreeConfig config) {
		const core::Vector3 rootMaximum = config.RootMinimum + config.RootSize;
		if (!Finite(config.RootMinimum) || !Finite(config.RootSize) || !Finite(rootMaximum) ||
			!Positive(config.RootSize) || config.MaximumDepth == 0 ||
			config.MaximumDepth > MAXIMUM_SUPPORTED_DEPTH || !Resolvable(config, rootMaximum) ||
			!std::isfinite(config.EmptyThreshold) || config.EmptyThreshold < 0.0f ||
			config.EmptyThreshold >= 1.0f)
			return std::nullopt;
		return CloudDensityOctree{config};
	}

	CloudDensityOctree::CloudDensityOctree(CloudDensityOctreeConfig config) : config_(config) {
		nodes_.emplace_back();
		activeNodeCount_ = 1;
	}

	bool CloudDensityOctree::Contains(const core::Vector3 &position) const {
		if (!Finite(position)) return false;
		const core::Vector3 maximum = config_.RootMinimum + config_.RootSize;
		return position.X >= config_.RootMinimum.X && position.X < maximum.X &&
			   position.Y >= config_.RootMinimum.Y && position.Y < maximum.Y &&
			   position.Z >= config_.RootMinimum.Z && position.Z < maximum.Z;
	}

	CloudDensityOctree::NodeIndex CloudDensityOctree::AllocateNode() {
		if (!freeNodes_.empty()) {
			const NodeIndex index = freeNodes_.back();
			freeNodes_.pop_back();
			nodes_[index] = Node{};
			++activeNodeCount_;
			return index;
		}
		if (nodes_.size() >= static_cast<size_t>(INVALID_NODE))
			throw std::length_error("cloud density octree exhausted node indices");
		nodes_.emplace_back();
		++activeNodeCount_;
		return static_cast<NodeIndex>(nodes_.size() - 1);
	}

	void CloudDensityOctree::ReleaseNode(NodeIndex index) {
		nodes_[index] = Node{};
		freeNodes_.push_back(index);
		--activeNodeCount_;
	}

	bool CloudDensityOctree::IsLeaf(NodeIndex index) const {
		for (const NodeIndex child : nodes_[index].Children)
			if (child != INVALID_NODE) return false;
		return true;
	}

	void CloudDensityOctree::ReleaseSubtree(NodeIndex index) {
		if (IsLeaf(index)) {
			if (nodes_[index].Density > config_.EmptyThreshold) --storedVoxelCount_;
		} else {
			for (const NodeIndex child : nodes_[index].Children)
				if (child != INVALID_NODE) ReleaseSubtree(child);
		}
		ReleaseNode(index);
	}

	void CloudDensityOctree::SplitOccupiedLeaf(NodeIndex index) {
		const float density = nodes_[index].Density;
		if (density <= config_.EmptyThreshold || !IsLeaf(index)) return;
		--storedVoxelCount_;
		for (size_t octant = 0; octant < nodes_[index].Children.size(); ++octant) {
			const NodeIndex child = AllocateNode();
			nodes_[index].Children[octant] = child;
			nodes_[child].Density = density;
			++storedVoxelCount_;
		}
	}

	void CloudDensityOctree::RecomputeDensity(NodeIndex index) {
		float densitySum = 0.0f;
		for (const NodeIndex child : nodes_[index].Children)
			if (child != INVALID_NODE) densitySum += nodes_[child].Density;
		nodes_[index].Density = densitySum * 0.125f;
	}

	bool CloudDensityOctree::SetDensity(const core::Vector3 &position, float density) {
		return SetDensity(position, density, config_.MaximumDepth);
	}

	bool CloudDensityOctree::SetDensity(const core::Vector3 &position, float density, uint8_t targetDepth) {
		if (!Contains(position) || !std::isfinite(density) || density < 0.0f || density > 1.0f ||
			targetDepth == 0 || targetDepth > config_.MaximumDepth)
			return false;
		if (density <= config_.EmptyThreshold) return EraseDensity(position, targetDepth);
		std::array<NodeIndex, static_cast<size_t>(MAXIMUM_SUPPORTED_DEPTH) + 1> path{};
		size_t pathSize = 1;
		NodeIndex current = 0;
		path[0] = current;
		core::Vector3 minimum = config_.RootMinimum;
		core::Vector3 maximum = config_.RootMinimum + config_.RootSize;
		for (uint8_t depth = 0; depth < targetDepth; ++depth) {
			SplitOccupiedLeaf(current);
			const uint8_t octant = SelectOctant(position, minimum, maximum);
			NodeIndex child = nodes_[current].Children[octant];
			if (child == INVALID_NODE) {
				child = AllocateNode();
				nodes_[current].Children[octant] = child;
			}
			current = child;
			path[pathSize++] = current;
		}
		const bool wasLeaf = IsLeaf(current);
		const bool wasOccupied = wasLeaf && nodes_[current].Density > config_.EmptyThreshold;
		if (!wasLeaf)
			for (NodeIndex &child : nodes_[current].Children) {
				if (child != INVALID_NODE) ReleaseSubtree(child);
				child = INVALID_NODE;
			}
		nodes_[current].Density = density;
		if (!wasOccupied) ++storedVoxelCount_;
		for (size_t offset = pathSize - 1; offset > 0; --offset)
			RecomputeDensity(path[offset - 1]);
		return true;
	}

	bool CloudDensityOctree::EraseDensity(const core::Vector3 &position, uint8_t targetDepth) {
		std::array<NodeIndex, static_cast<size_t>(MAXIMUM_SUPPORTED_DEPTH) + 1> path{};
		std::array<uint8_t, MAXIMUM_SUPPORTED_DEPTH> octants{};
		size_t pathSize = 1;
		NodeIndex current = 0;
		path[0] = current;
		core::Vector3 minimum = config_.RootMinimum;
		core::Vector3 maximum = config_.RootMinimum + config_.RootSize;
		for (uint8_t depth = 0; depth < targetDepth; ++depth) {
			if (IsLeaf(current)) {
				if (nodes_[current].Density <= config_.EmptyThreshold) return true;
				SplitOccupiedLeaf(current);
			}
			const uint8_t octant = SelectOctant(position, minimum, maximum);
			const NodeIndex child = nodes_[current].Children[octant];
			if (child == INVALID_NODE) return true;
			octants[pathSize - 1] = octant;
			current = child;
			path[pathSize++] = current;
		}
		if (IsLeaf(current)) {
			if (nodes_[current].Density <= config_.EmptyThreshold) return true;
			--storedVoxelCount_;
		} else
			for (NodeIndex &child : nodes_[current].Children) {
				if (child != INVALID_NODE) ReleaseSubtree(child);
				child = INVALID_NODE;
			}
		nodes_[current].Density = 0.0f;
		for (size_t offset = pathSize - 1; offset > 0; --offset) {
			const NodeIndex child = path[offset];
			const NodeIndex parent = path[offset - 1];
			if (IsLeaf(child) && nodes_[child].Density <= config_.EmptyThreshold) {
				nodes_[parent].Children[octants[offset - 1]] = INVALID_NODE;
				ReleaseNode(child);
			}
			RecomputeDensity(parent);
		}
		return true;
	}

	std::optional<float> CloudDensityOctree::SampleDensity(const core::Vector3 &position) const {
		if (!Contains(position)) return std::nullopt;
		NodeIndex current = 0;
		core::Vector3 minimum = config_.RootMinimum;
		core::Vector3 maximum = config_.RootMinimum + config_.RootSize;
		for (uint8_t depth = 0; depth < config_.MaximumDepth; ++depth) {
			if (IsLeaf(current)) return nodes_[current].Density;
			current = nodes_[current].Children[SelectOctant(position, minimum, maximum)];
			if (current == INVALID_NODE) return 0.0f;
		}
		return nodes_[current].Density;
	}

	void CloudDensityOctree::Clear() {
		nodes_.clear();
		freeNodes_.clear();
		nodes_.emplace_back();
		activeNodeCount_ = 1;
		storedVoxelCount_ = 0;
	}

	core::Vector3 CloudDensityOctree::VoxelSize() const {
		return config_.RootSize / static_cast<float>(uint32_t{1} << config_.MaximumDepth);
	}

	float CloudDensityOctree::AverageDensity() const {
		return nodes_.front().Density;
	}

	std::vector<CloudDensityGpuNode> CloudDensityOctree::GpuNodes() const {
		std::vector<CloudDensityGpuNode> packed(nodes_.size());
		for (size_t index = 0; index < nodes_.size(); ++index) {
			for (size_t child = 0; child < 4; ++child) {
				packed[index].ChildrenLow[child] = nodes_[index].Children[child];
				packed[index].ChildrenHigh[child] = nodes_[index].Children[child + 4];
			}
			packed[index].Values[0] = nodes_[index].Density;
		}
		return packed;
	}
}
