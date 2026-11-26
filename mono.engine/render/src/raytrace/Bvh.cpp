#include <engine/render/raytrace/Bvh.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <numeric>

namespace engine::render {
	namespace {
		struct Box {
			glm::vec3 Minimum{std::numeric_limits<float>::infinity()};
			glm::vec3 Maximum{-std::numeric_limits<float>::infinity()};
			void Grow(const glm::vec3 &point) {
				Minimum = glm::min(Minimum, point);
				Maximum = glm::max(Maximum, point);
			}
			void Grow(const Box &other) {
				Minimum = glm::min(Minimum, other.Minimum);
				Maximum = glm::max(Maximum, other.Maximum);
			}
			float Area() const {
				const glm::vec3 size = glm::max(Maximum - Minimum, glm::vec3(0.0f));
				return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
			}
		};

		// Builds a tree over `boxes` (with `centres`), reordering `order` so
		// each leaf's items are contiguous. Binned SAH, twelve bins per axis.
		std::vector<BvhNode> BuildTree(const std::vector<Box> &boxes, const std::vector<glm::vec3> &centres, std::vector<uint32_t> &order) {
			std::vector<BvhNode> nodes;
			order.resize(boxes.size());
			std::iota(order.begin(), order.end(), 0u);
			if (boxes.empty()) {
				nodes.push_back({});
				return nodes;
			}
			nodes.reserve(boxes.size() * 2);
			nodes.push_back({});
			struct Task {
				uint32_t Node, First, Count;
			};
			std::vector<Task> tasks{{0, 0, uint32_t(boxes.size())}};
			constexpr int BINS = 12;
			while (!tasks.empty()) {
				const Task task = tasks.back();
				tasks.pop_back();
				Box bounds, centroid;
				for (uint32_t index = task.First; index < task.First + task.Count; ++index) {
					bounds.Grow(boxes[order[index]]);
					centroid.Grow(centres[order[index]]);
				}
				nodes[task.Node].Minimum = bounds.Minimum;
				nodes[task.Node].Maximum = bounds.Maximum;
				const auto leaf = [&] {
					nodes[task.Node].First = task.First;
					nodes[task.Node].Count = task.Count;
				};
				if (task.Count <= BVH_LEAF_SIZE) {
					leaf();
					continue;
				}
				float bestCost = std::numeric_limits<float>::infinity();
				int bestAxis = -1, bestSplit = 0;
				for (int axis = 0; axis < 3; ++axis) {
					const float low = centroid.Minimum[axis], high = centroid.Maximum[axis];
					if (high - low < 1e-12f) continue;
					std::array<Box, BINS> bins;
					std::array<uint32_t, BINS> counts{};
					for (uint32_t index = task.First; index < task.First + task.Count; ++index) {
						const int bin = std::min(BINS - 1, int((centres[order[index]][axis] - low) / (high - low) * BINS));
						bins[bin].Grow(boxes[order[index]]);
						++counts[bin];
					}
					for (int split = 1; split < BINS; ++split) {
						Box left, right;
						uint32_t leftCount = 0, rightCount = 0;
						for (int bin = 0; bin < split; ++bin)
							if (counts[bin]) left.Grow(bins[bin]), leftCount += counts[bin];
						for (int bin = split; bin < BINS; ++bin)
							if (counts[bin]) right.Grow(bins[bin]), rightCount += counts[bin];
						if (leftCount == 0 || rightCount == 0) continue;
						const float cost = left.Area() * leftCount + right.Area() * rightCount;
						if (cost < bestCost) bestCost = cost, bestAxis = axis, bestSplit = split;
					}
				}
				// Splitting does not pay: one leaf, however many it holds.
				if (bestAxis < 0 || bestCost >= bounds.Area() * task.Count) {
					leaf();
					continue;
				}
				const float low = centroid.Minimum[bestAxis], high = centroid.Maximum[bestAxis];
				const auto middle = std::partition(order.begin() + task.First, order.begin() + task.First + task.Count, [&](uint32_t item) {
					return std::min(BINS - 1, int((centres[item][bestAxis] - low) / (high - low) * BINS)) < bestSplit;
				});
				const uint32_t leftCount = uint32_t(middle - (order.begin() + task.First));
				const uint32_t child = uint32_t(nodes.size());
				nodes.push_back({});
				nodes.push_back({});
				nodes[task.Node].First = child;
				nodes[task.Node].Count = 0;
				tasks.push_back({child, task.First, leftCount});
				tasks.push_back({child + 1, task.First + leftCount, task.Count - leftCount});
			}
			return nodes;
		}

		glm::vec3 Inverse(const glm::vec3 &direction) {
			const auto safe = [](float v) { return std::abs(v) < 1e-20f ? (v < 0 ? -1e20f : 1e20f) : 1.0f / v; };
			return {safe(direction.x), safe(direction.y), safe(direction.z)};
		}
	}

	bool RayBox(const Ray &ray, const glm::vec3 &inverse, const glm::vec3 &minimum, const glm::vec3 &maximum, float &entry) {
		const glm::vec3 t0 = (minimum - ray.Origin) * inverse, t1 = (maximum - ray.Origin) * inverse;
		const glm::vec3 near = glm::min(t0, t1), far = glm::max(t0, t1);
		entry = std::max(std::max(near.x, near.y), std::max(near.z, ray.Minimum));
		const float exit = std::min(std::min(far.x, far.y), std::min(far.z, ray.Maximum));
		return entry <= exit;
	}

	bool RayTriangle(const Ray &ray, const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, float &t, float &u, float &v) {
		const glm::vec3 e1 = b - a, e2 = c - a;
		const glm::vec3 p = glm::cross(ray.Direction, e2);
		const float det = glm::dot(e1, p);
		if (std::abs(det) < 1e-12f) return false;
		const float inverse = 1.0f / det;
		const glm::vec3 s = ray.Origin - a;
		u = glm::dot(s, p) * inverse;
		if (u < 0.0f || u > 1.0f) return false;
		const glm::vec3 q = glm::cross(s, e1);
		v = glm::dot(ray.Direction, q) * inverse;
		if (v < 0.0f || u + v > 1.0f) return false;
		t = glm::dot(e2, q) * inverse;
		return t >= ray.Minimum && t <= ray.Maximum;
	}

	void MeshBvh::Build(std::span<const float> positions, std::span<const uint32_t> indices) {
		const size_t triangles = indices.size() / 3;
		std::vector<Box> boxes(triangles);
		std::vector<glm::vec3> centres(triangles);
		const auto corner = [&](uint32_t index) {
			return glm::vec3(positions[index * 3], positions[index * 3 + 1], positions[index * 3 + 2]);
		};
		for (size_t t = 0; t < triangles; ++t) {
			for (int k = 0; k < 3; ++k) boxes[t].Grow(corner(indices[t * 3 + k]));
			centres[t] = (boxes[t].Minimum + boxes[t].Maximum) * 0.5f;
		}
		BvhNodes = BuildTree(boxes, centres, TriangleOrder);
		TriangleCorners.resize(triangles * 3);
		for (size_t index = 0; index < triangles; ++index)
			for (int k = 0; k < 3; ++k)
				TriangleCorners[index * 3 + k] = corner(indices[TriangleOrder[index] * 3 + k]);
	}

	glm::vec3 MeshBvh::Minimum() const {
		return BvhNodes.empty() ? glm::vec3(0.0f) : BvhNodes[0].Minimum;
	}
	glm::vec3 MeshBvh::Maximum() const {
		return BvhNodes.empty() ? glm::vec3(0.0f) : BvhNodes[0].Maximum;
	}

	template <bool AnyHit> RayHit MeshBvh::Trace(const Ray &query) const {
		RayHit hit;
		if (TriangleCorners.empty()) return hit;
		Ray ray = query;
		const glm::vec3 inverse = Inverse(ray.Direction);
		std::array<uint32_t, 64> stack;
		uint32_t depth = 0;
		stack[depth++] = 0;
		while (depth > 0) {
			const BvhNode &node = BvhNodes[stack[--depth]];
			float entry = 0;
			if (!RayBox(ray, inverse, node.Minimum, node.Maximum, entry)) continue;
			if (node.Count == 0) {
				stack[depth++] = node.First;
				stack[depth++] = node.First + 1;
				continue;
			}
			for (uint32_t index = node.First; index < node.First + node.Count; ++index) {
				float t = 0, u = 0, v = 0;
				if (!RayTriangle(ray, TriangleCorners[index * 3], TriangleCorners[index * 3 + 1], TriangleCorners[index * 3 + 2], t, u, v)) continue;
				hit = {true, t, 0, TriangleOrder[index], u, v};
				if constexpr (AnyHit) return hit;
				ray.Maximum = t;
			}
		}
		return hit;
	}

	RayHit MeshBvh::Closest(const Ray &ray) const {
		return Trace<false>(ray);
	}
	bool MeshBvh::Any(const Ray &ray) const {
		return Trace<true>(ray).Found;
	}

	void SceneBvh::Build(std::span<const BvhInstance> instances) {
		Instances.assign(instances.begin(), instances.end());
		std::vector<Box> boxes(Instances.size());
		std::vector<glm::vec3> centres(Instances.size());
		for (size_t index = 0; index < Instances.size(); ++index) {
			const BvhInstance &instance = Instances[index];
			const glm::vec3 low = instance.Mesh->Minimum(), high = instance.Mesh->Maximum();
			const glm::quat rotation = instance.Frame.Rotation();
			const glm::vec3 position(instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z);
			for (int corner = 0; corner < 8; ++corner) {
				const glm::vec3 local((corner & 1) ? high.x : low.x, (corner & 2) ? high.y : low.y, (corner & 4) ? high.z : low.z);
				boxes[index].Grow(position + rotation * (local * instance.Scale));
			}
			centres[index] = (boxes[index].Minimum + boxes[index].Maximum) * 0.5f;
		}
		BvhNodes = BuildTree(boxes, centres, InstanceOrder);
	}

	template <bool AnyHit> RayHit SceneBvh::Trace(const Ray &query) const {
		RayHit hit;
		if (Instances.empty()) return hit;
		Ray ray = query;
		const glm::vec3 inverse = Inverse(ray.Direction);
		std::array<uint32_t, 64> stack;
		uint32_t depth = 0;
		stack[depth++] = 0;
		while (depth > 0) {
			const BvhNode &node = BvhNodes[stack[--depth]];
			float entry = 0;
			if (!RayBox(ray, inverse, node.Minimum, node.Maximum, entry)) continue;
			if (node.Count == 0) {
				stack[depth++] = node.First;
				stack[depth++] = node.First + 1;
				continue;
			}
			for (uint32_t index = node.First; index < node.First + node.Count; ++index) {
				const uint32_t which = InstanceOrder[index];
				const BvhInstance &instance = Instances[which];
				// Into object space: the distance along the ray is unchanged,
				// since the direction is carried without renormalising.
				const glm::quat inverseRotation = glm::conjugate(instance.Frame.Rotation());
				const glm::vec3 position(instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z);
				Ray local;
				local.Origin = (inverseRotation * (ray.Origin - position)) / instance.Scale;
				local.Direction = (inverseRotation * ray.Direction) / instance.Scale;
				local.Minimum = ray.Minimum;
				local.Maximum = ray.Maximum;
				RayHit found = AnyHit ? (instance.Mesh->Any(local) ? RayHit{true} : RayHit{}) : instance.Mesh->Closest(local);
				if (!found.Found) continue;
				found.Instance = which;
				hit = found;
				if constexpr (AnyHit) return hit;
				ray.Maximum = found.Distance;
			}
		}
		return hit;
	}

	RayHit SceneBvh::Closest(const Ray &ray) const {
		return Trace<false>(ray);
	}
	bool SceneBvh::Any(const Ray &ray) const {
		return Trace<true>(ray).Found;
	}
}
