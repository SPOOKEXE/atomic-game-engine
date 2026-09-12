#include <engine/assets/MeshDecimate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace engine::assets {

	namespace {
		using Triangle = std::array<uint32_t, 3>;

		bool SameSkin(const MeshVertex &left, const MeshVertex &right) {
			return std::equal(std::begin(left.Joints), std::end(left.Joints), std::begin(right.Joints)) &&
				   std::equal(std::begin(left.Weights), std::end(left.Weights), std::begin(right.Weights));
		}

		float EdgeCost(const MeshVertex &left, const MeshVertex &right) {
			float squared = 0.0f;
			for (size_t axis = 0; axis < 3; axis++) {
				const float delta = left.Position[axis] - right.Position[axis];
				squared += delta * delta;
			}
			return squared;
		}

		bool HasVertex(const Triangle &triangle, uint32_t vertex) {
			return triangle[0] == vertex || triangle[1] == vertex || triangle[2] == vertex;
		}

		bool Degenerate(const Triangle &triangle) {
			return triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[2] == triangle[0];
		}

		void MergeVertex(MeshVertex &into, const MeshVertex &other) {
			for (size_t axis = 0; axis < 3; axis++) {
				into.Position[axis] = (into.Position[axis] + other.Position[axis]) * 0.5f;
				into.Normal[axis] += other.Normal[axis];
			}
			into.TexCoord[0] = (into.TexCoord[0] + other.TexCoord[0]) * 0.5f;
			into.TexCoord[1] = (into.TexCoord[1] + other.TexCoord[1]) * 0.5f;

			const float length = std::sqrt(
				into.Normal[0] * into.Normal[0] + into.Normal[1] * into.Normal[1] +
				into.Normal[2] * into.Normal[2]
			);
			if (length > 0.0f) {
				for (float &channel : into.Normal) {
					channel /= length;
				}
			}
		}

		bool CollapseOne(
			std::vector<MeshVertex> &vertices,
			std::vector<Triangle> &triangles,
			const std::vector<uint32_t> &owners,
			uint32_t submesh,
			size_t target
		) {
			float bestCost = std::numeric_limits<float>::infinity();
			uint32_t bestLeft = 0;
			uint32_t bestRight = 0;
			bool found = false;

			for (size_t index = 0; index < triangles.size(); index++) {
				if (owners[index] != submesh || Degenerate(triangles[index])) {
					continue;
				}
				const Triangle &triangle = triangles[index];
				for (const std::array<uint32_t, 2> edge : {
						 std::array<uint32_t, 2>{triangle[0], triangle[1]},
						 std::array<uint32_t, 2>{triangle[1], triangle[2]},
						 std::array<uint32_t, 2>{triangle[2], triangle[0]},
					 }) {
					const uint32_t left = std::min(edge[0], edge[1]);
					const uint32_t right = std::max(edge[0], edge[1]);
					if (left == right || !SameSkin(vertices[left], vertices[right])) {
						continue;
					}

					// A shared vertex may belong to another material run. Moving it
					// would alter that run without accounting for its triangles.
					bool local = true;
					for (size_t other = 0; other < triangles.size(); other++) {
						if (owners[other] != submesh &&
							(HasVertex(triangles[other], left) || HasVertex(triangles[other], right))) {
							local = false;
							break;
						}
					}
					if (!local) {
						continue;
					}
					size_t remaining = 0;
					for (size_t other = 0; other < triangles.size(); other++) {
						if (owners[other] != submesh) {
							continue;
						}
						Triangle after = triangles[other];
						for (uint32_t &vertex : after) {
							if (vertex == right) {
								vertex = left;
							}
						}
						remaining += Degenerate(after) ? 0u : 1u;
					}
					if (remaining < target) {
						continue;
					}

					const float cost = EdgeCost(vertices[left], vertices[right]);
					if (!found || cost < bestCost ||
						(cost == bestCost && std::pair(left, right) < std::pair(bestLeft, bestRight))) {
						bestCost = cost;
						bestLeft = left;
						bestRight = right;
						found = true;
					}
				}
			}

			if (!found) {
				return false;
			}
			MergeVertex(vertices[bestLeft], vertices[bestRight]);
			for (Triangle &triangle : triangles) {
				for (uint32_t &index : triangle) {
					if (index == bestRight) {
						index = bestLeft;
					}
				}
			}
			return true;
		}
	}

	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out) {
		if (&source == &out || !source.IsValid() || !(ratio > 0.0f) || ratio > 1.0f) {
			return false;
		}
		if (!source.Submeshes.empty()) {
			uint32_t next = 0;
			for (const Submesh &run : source.Submeshes) {
				if (run.FirstIndex != next) {
					return false;
				}
				next += run.IndexCount;
			}
			if (next != source.Indices.size()) {
				return false;
			}
		}

		std::vector<Triangle> triangles;
		std::vector<uint32_t> owners;
		triangles.reserve(source.Indices.size() / 3);
		owners.reserve(source.Indices.size() / 3);
		if (source.Submeshes.empty()) {
			for (size_t index = 0; index < source.Indices.size(); index += 3) {
				triangles.push_back(
					{source.Indices[index], source.Indices[index + 1], source.Indices[index + 2]}
				);
				owners.push_back(0);
			}
		} else {
			for (size_t submesh = 0; submesh < source.Submeshes.size(); submesh++) {
				const Submesh &run = source.Submeshes[submesh];
				for (size_t index = run.FirstIndex; index < run.FirstIndex + run.IndexCount; index += 3) {
					triangles.push_back(
						{source.Indices[index], source.Indices[index + 1], source.Indices[index + 2]}
					);
					owners.push_back(static_cast<uint32_t>(submesh));
				}
			}
		}

		std::vector<MeshVertex> vertices = source.Vertices;
		const uint32_t runCount =
			source.Submeshes.empty() ? 1u : static_cast<uint32_t>(source.Submeshes.size());
		for (uint32_t submesh = 0; submesh < runCount; submesh++) {
			size_t count = 0;
			for (size_t index = 0; index < triangles.size(); index++) {
				count += owners[index] == submesh && !Degenerate(triangles[index]) ? 1u : 0u;
			}
			const size_t target = std::max<size_t>(1, static_cast<size_t>(std::floor(count * ratio)));
			while (count > target && CollapseOne(vertices, triangles, owners, submesh, target)) {
				count = 0;
				for (size_t index = 0; index < triangles.size(); index++) {
					count += owners[index] == submesh && !Degenerate(triangles[index]) ? 1u : 0u;
				}
			}
			if (count > target) {
				// A skin or material seam can leave no legal collapse. Keep an even,
				// stable subset rather than blending different joint influences just
				// to reach the requested count.
				size_t seen = 0;
				size_t kept = 0;
				for (size_t index = 0; index < triangles.size(); index++) {
					if (owners[index] != submesh || Degenerate(triangles[index])) {
						continue;
					}
					const bool keep = (seen + 1) * target / count > kept;
					seen++;
					if (keep) {
						kept++;
					} else {
						triangles[index][2] = triangles[index][0];
					}
				}
			}
		}

		MeshData reduced;
		reduced.JointCount = source.JointCount;
		std::vector<uint32_t> remap(vertices.size(), std::numeric_limits<uint32_t>::max());
		const uint32_t runCountForOutput =
			source.Submeshes.empty() ? 1u : static_cast<uint32_t>(source.Submeshes.size());
		for (uint32_t submesh = 0; submesh < runCountForOutput; submesh++) {
			Submesh run;
			if (!source.Submeshes.empty()) {
				run = source.Submeshes[submesh];
			}
			run.FirstIndex = static_cast<uint32_t>(reduced.Indices.size());
			for (size_t index = 0; index < triangles.size(); index++) {
				if (owners[index] != submesh || Degenerate(triangles[index])) {
					continue;
				}
				for (const uint32_t old : triangles[index]) {
					if (remap[old] == std::numeric_limits<uint32_t>::max()) {
						remap[old] = static_cast<uint32_t>(reduced.Vertices.size());
						reduced.Vertices.push_back(vertices[old]);
					}
					reduced.Indices.push_back(remap[old]);
				}
			}
			run.IndexCount = static_cast<uint32_t>(reduced.Indices.size()) - run.FirstIndex;
			if (!source.Submeshes.empty()) {
				reduced.Submeshes.push_back(std::move(run));
			}
		}

		if (!reduced.IsValid()) {
			return false;
		}
		reduced.ComputeBounds();
		out = std::move(reduced);
		return true;
	}
}
