#pragma once

// Geometry ray queries (R08): bounding volume hierarchies over triangles and
// over instances, answering closest-hit and any-hit rays against the real
// scene rather than the screen.
//
//     MeshBvh mesh;
//     mesh.Build(positions, indices);
//     SceneBvh scene;
//     scene.Build({{&mesh, frame, scale}});
//     const RayHit hit = scene.Closest({origin, direction});
//
// Built with binned surface-area heuristic splits. The flattened arrays are
// what the GPU traversal reads (see `raytrace/PathTracer`).
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace engine::render {
	struct Ray {
		glm::vec3 Origin{0.0f};
		glm::vec3 Direction{0.0f, 0.0f, -1.0f};
		float Minimum = 0.0f;
		float Maximum = std::numeric_limits<float>::infinity();
	};

	struct RayHit {
		bool Found = false;
		float Distance = std::numeric_limits<float>::infinity();
		// The instance (scene queries) and the triangle within its mesh.
		uint32_t Instance = 0;
		uint32_t Triangle = 0;
		// Barycentrics of the second and third corners.
		float U = 0.0f;
		float V = 0.0f;
	};

	// One node: a box, and either two children (`Count` zero, children at
	// `First` and `First + 1`) or `Count` items from `First`.
	struct BvhNode {
		glm::vec3 Minimum{0.0f};
		uint32_t First = 0;
		glm::vec3 Maximum{0.0f};
		uint32_t Count = 0;
	};
	static_assert(sizeof(BvhNode) == 32);

	// Most triangles or instances a leaf holds.
	inline constexpr uint32_t BVH_LEAF_SIZE = 4;

	// A mesh's triangles, in object space.
	class MeshBvh {
	  public:
		// `positions` are xyz triples; `indices` three per triangle.
		void Build(std::span<const float> positions, std::span<const uint32_t> indices);
		RayHit Closest(const Ray &ray) const;
		bool Any(const Ray &ray) const;

		const std::vector<BvhNode> &Nodes() const {
			return BvhNodes;
		}
		// Triangle corners in leaf order: three positions per triangle.
		const std::vector<glm::vec3> &Corners() const {
			return TriangleCorners;
		}
		// The source triangle each leaf-order triangle came from.
		const std::vector<uint32_t> &Order() const {
			return TriangleOrder;
		}
		glm::vec3 Minimum() const;
		glm::vec3 Maximum() const;

	  private:
		template <bool AnyHit> RayHit Trace(const Ray &ray) const;
		std::vector<BvhNode> BvhNodes;
		std::vector<glm::vec3> TriangleCorners;
		std::vector<uint32_t> TriangleOrder;
	};

	// One placed mesh: its frame and per-axis scale.
	struct BvhInstance {
		const MeshBvh *Mesh = nullptr;
		core::CFrame Frame;
		glm::vec3 Scale{1.0f};
	};

	// Instances, each tracing its mesh in object space.
	class SceneBvh {
	  public:
		void Build(std::span<const BvhInstance> instances);
		RayHit Closest(const Ray &ray) const;
		bool Any(const Ray &ray) const;

		const std::vector<BvhNode> &Nodes() const {
			return BvhNodes;
		}
		// Instance indices in leaf order.
		const std::vector<uint32_t> &Order() const {
			return InstanceOrder;
		}

	  private:
		template <bool AnyHit> RayHit Trace(const Ray &ray) const;
		std::vector<BvhInstance> Instances;
		std::vector<BvhNode> BvhNodes;
		std::vector<uint32_t> InstanceOrder;
	};

	// Whether a ray meets a box within its range, and where it enters.
	bool RayBox(const Ray &ray, const glm::vec3 &inverse, const glm::vec3 &minimum, const glm::vec3 &maximum, float &entry);

	// Moeller-Trumbore: the distance and barycentrics where a ray meets a triangle.
	bool RayTriangle(const Ray &ray, const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, float &t, float &u, float &v);
}
