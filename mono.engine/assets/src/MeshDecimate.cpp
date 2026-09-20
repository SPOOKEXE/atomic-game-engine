#include <engine/assets/MeshDecimate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
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

		struct FaceAdjacency {
			std::vector<size_t> Offsets;
			std::vector<size_t> Faces;

			std::span<const size_t> ForVertex(uint32_t vertex) const {
				return std::span(Faces).subspan(Offsets[vertex], Offsets[vertex + 1] - Offsets[vertex]);
			}
		};

		FaceAdjacency BuildFaceAdjacency(size_t vertexCount, const std::vector<Triangle> &triangles) {
			FaceAdjacency adjacency;
			adjacency.Offsets.resize(vertexCount + 1);
			for (const Triangle &triangle : triangles) {
				adjacency.Offsets[triangle[0] + 1]++;
				if (triangle[1] != triangle[0]) adjacency.Offsets[triangle[1] + 1]++;
				if (triangle[2] != triangle[0] && triangle[2] != triangle[1])
					adjacency.Offsets[triangle[2] + 1]++;
			}
			for (size_t vertex = 1; vertex < adjacency.Offsets.size(); vertex++) {
				adjacency.Offsets[vertex] += adjacency.Offsets[vertex - 1];
			}
			adjacency.Faces.resize(adjacency.Offsets.back());
			std::vector<size_t> next = adjacency.Offsets;
			for (size_t face = 0; face < triangles.size(); face++) {
				const Triangle &triangle = triangles[face];
				adjacency.Faces[next[triangle[0]]++] = face;
				if (triangle[1] != triangle[0]) adjacency.Faces[next[triangle[1]]++] = face;
				if (triangle[2] != triangle[0] && triangle[2] != triangle[1])
					adjacency.Faces[next[triangle[2]]++] = face;
			}
			return adjacency;
		}

		template <typename Callback>
		void ForEachIncidentFace(
			const FaceAdjacency &adjacency, uint32_t left, uint32_t right, Callback callback
		) {
			const std::span<const size_t> leftFaces = adjacency.ForVertex(left);
			const std::span<const size_t> rightFaces = adjacency.ForVertex(right);
			size_t leftIndex = 0;
			size_t rightIndex = 0;
			while (leftIndex < leftFaces.size() || rightIndex < rightFaces.size()) {
				if (rightIndex == rightFaces.size() ||
					(leftIndex < leftFaces.size() && leftFaces[leftIndex] < rightFaces[rightIndex])) {
					callback(leftFaces[leftIndex++]);
				} else if (leftIndex == leftFaces.size() || rightFaces[rightIndex] < leftFaces[leftIndex]) {
					callback(rightFaces[rightIndex++]);
				} else {
					callback(leftFaces[leftIndex]);
					leftIndex++;
					rightIndex++;
				}
			}
		}

		std::array<float, 3> FaceVector(
			const std::vector<MeshVertex> &vertices,
			const Triangle &triangle,
			uint32_t replace = std::numeric_limits<uint32_t>::max(),
			const std::array<float, 3> &position = {}
		) {
			std::array<std::array<float, 3>, 3> points{};
			for (size_t corner = 0; corner < 3; corner++) {
				const uint32_t index = triangle[corner];
				for (size_t axis = 0; axis < 3; axis++)
					points[corner][axis] = index == replace ? position[axis] : vertices[index].Position[axis];
			}
			const std::array<float, 3> left{
				points[1][0] - points[0][0], points[1][1] - points[0][1], points[1][2] - points[0][2]
			};
			const std::array<float, 3> right{
				points[2][0] - points[0][0], points[2][1] - points[0][1], points[2][2] - points[0][2]
			};
			return {
				left[1] * right[2] - left[2] * right[1],
				left[2] * right[0] - left[0] * right[2],
				left[0] * right[1] - left[1] * right[0],
			};
		}

		float FaceAreaSquared(const std::vector<MeshVertex> &vertices, const Triangle &triangle) {
			const auto face = FaceVector(vertices, triangle);
			return face[0] * face[0] + face[1] * face[1] + face[2] * face[2];
		}

		float SurfaceCost(
			const std::vector<MeshVertex> &vertices,
			const std::vector<Triangle> &triangles,
			const std::vector<uint32_t> &owners,
			const FaceAdjacency &adjacency,
			uint32_t owner,
			uint32_t left,
			uint32_t right
		) {
			float area = 0.0f;
			ForEachIncidentFace(adjacency, left, right, [&](size_t index) {
				if (owners[index] == owner &&
					(HasVertex(triangles[index], left) || HasVertex(triangles[index], right))) {
					area += std::sqrt(FaceAreaSquared(vertices, triangles[index]));
				}
			});
			return area;
		}

		bool PreservesWinding(
			const std::vector<MeshVertex> &vertices,
			const std::vector<Triangle> &triangles,
			const std::vector<uint32_t> &owners,
			const FaceAdjacency &adjacency,
			uint32_t owner,
			uint32_t left,
			uint32_t right
		) {
			std::array<float, 3> merged{};
			for (size_t axis = 0; axis < 3; axis++)
				merged[axis] = (vertices[left].Position[axis] + vertices[right].Position[axis]) * 0.5f;

			bool preserves = true;
			ForEachIncidentFace(adjacency, left, right, [&](size_t index) {
				if (!preserves) return;
				if (owners[index] != owner ||
					(!HasVertex(triangles[index], left) && !HasVertex(triangles[index], right)))
					return;
				Triangle after = triangles[index];
				for (uint32_t &vertex : after)
					if (vertex == right) vertex = left;
				if (Degenerate(after)) return;

				const auto beforeFace = FaceVector(vertices, triangles[index]);
				const auto afterFace = FaceVector(vertices, after, left, merged);
				const float alignment = beforeFace[0] * afterFace[0] + beforeFace[1] * afterFace[1] +
										beforeFace[2] * afterFace[2];
				if (!(alignment > 0.0f)) preserves = false;
			});
			return preserves;
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
			size_t &count,
			size_t target,
			MeshReduction reduction,
			std::stop_token stop
		) {
			const FaceAdjacency adjacency = BuildFaceAdjacency(vertices.size(), triangles);
			float bestCost = std::numeric_limits<float>::infinity();
			uint32_t bestLeft = 0;
			uint32_t bestRight = 0;
			size_t bestRemoved = 0;
			bool found = false;

			for (size_t index = 0; index < triangles.size(); index++) {
				if (stop.stop_requested()) return false;
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
					size_t edgeUses = 0;
					size_t removed = 0;
					const std::span<const size_t> leftFaces = adjacency.ForVertex(left);
					const std::span<const size_t> rightFaces = adjacency.ForVertex(right);
					size_t leftFace = 0;
					size_t rightFace = 0;
					while (leftFace < leftFaces.size() && rightFace < rightFaces.size()) {
						if (leftFaces[leftFace] < rightFaces[rightFace]) {
							leftFace++;
							continue;
						}
						if (rightFaces[rightFace] < leftFaces[leftFace]) {
							rightFace++;
							continue;
						}
						const size_t other = leftFaces[leftFace++];
						rightFace++;
						if (owners[other] == submesh) {
							edgeUses++;
							removed += Degenerate(triangles[other]) ? 0u : 1u;
						}
					}
					if (edgeUses < 2 ||
						!PreservesWinding(vertices, triangles, owners, adjacency, submesh, left, right))
						continue;

					// A shared vertex may belong to another material run. Moving it
					// would alter that run without accounting for its triangles.
					bool local = true;
					ForEachIncidentFace(adjacency, left, right, [&](size_t other) {
						if (owners[other] != submesh) local = false;
					});
					if (!local) {
						continue;
					}
					if (count - removed < target) {
						continue;
					}

					float cost = EdgeCost(vertices[left], vertices[right]);
					if (reduction == MeshReduction::SurfaceArea) {
						// The product ranks a collapse by its geometric error and by
						// the expected projected area it disturbs. A large face must
						// not disappear merely because one of its edges is short.
						cost *= SurfaceCost(vertices, triangles, owners, adjacency, submesh, left, right);
					}
					if (!found || cost < bestCost ||
						(cost == bestCost && std::pair(left, right) < std::pair(bestLeft, bestRight))) {
						bestCost = cost;
						bestLeft = left;
						bestRight = right;
						bestRemoved = removed;
						found = true;
					}
				}
			}

			if (!found) {
				return false;
			}
			MergeVertex(vertices[bestLeft], vertices[bestRight]);
			for (Triangle &triangle : triangles) {
				if (stop.stop_requested()) return false;
				for (uint32_t &index : triangle) {
					if (index == bestRight) {
						index = bestLeft;
					}
				}
			}
			count -= bestRemoved;
			return true;
		}
	}

	namespace {
		bool Reduce(
			const MeshData &source, float ratio, MeshData &out, MeshReduction reduction, std::stop_token stop
		) {
			if (&source == &out || !source.IsValid() || !(ratio > 0.0f) || ratio > 1.0f) {
				return false;
			}
			std::vector<Triangle> triangles;
			std::vector<uint32_t> owners;
			triangles.reserve(source.Indices.size() / 3 + source.Submeshes.size());
			owners.reserve(triangles.capacity());
			uint32_t runCount = 1;
			if (source.Submeshes.empty()) {
				for (size_t index = 0; index < source.Indices.size(); index += 3) {
					triangles.push_back(
						{source.Indices[index], source.Indices[index + 1], source.Indices[index + 2]}
					);
					owners.push_back(0);
				}
			} else {
				std::vector<bool> covered(source.Indices.size() / 3, false);
				for (size_t submesh = 0; submesh < source.Submeshes.size(); submesh++) {
					const Submesh &run = source.Submeshes[submesh];
					for (size_t index = run.FirstIndex; index < run.FirstIndex + run.IndexCount; index += 3) {
						triangles.push_back(
							{source.Indices[index], source.Indices[index + 1], source.Indices[index + 2]}
						);
						owners.push_back(static_cast<uint32_t>(submesh));
						covered[index / 3] = true;
					}
				}
				runCount = static_cast<uint32_t>(source.Submeshes.size());
				const uint32_t uncoveredOwner = runCount;
				for (size_t triangle = 0; triangle < covered.size(); triangle++) {
					if (covered[triangle]) continue;
					const size_t index = triangle * 3;
					triangles.push_back(
						{source.Indices[index], source.Indices[index + 1], source.Indices[index + 2]}
					);
					owners.push_back(uncoveredOwner);
				}
				if (std::find(owners.begin(), owners.end(), uncoveredOwner) != owners.end()) runCount++;
			}

			std::vector<MeshVertex> vertices = source.Vertices;
			for (uint32_t submesh = 0; submesh < runCount; submesh++) {
				if (stop.stop_requested()) return false;
				size_t count = 0;
				for (size_t index = 0; index < triangles.size(); index++) {
					count += owners[index] == submesh && !Degenerate(triangles[index]) ? 1u : 0u;
				}
				if (count == 0) continue;
				const size_t target = std::max<size_t>(1, static_cast<size_t>(std::floor(count * ratio)));
				while (!stop.stop_requested() && count > target &&
					   CollapseOne(vertices, triangles, owners, submesh, count, target, reduction, stop)) {}
				if (stop.stop_requested()) return false;
				if (count > target) {
					// A boundary, skin or material seam can leave no legal collapse. Keep
					// the largest faces so the fallback removes the least visible area.
					std::vector<size_t> ranked;
					for (size_t index = 0; index < triangles.size(); index++) {
						if (owners[index] == submesh && !Degenerate(triangles[index]))
							ranked.push_back(index);
					}
					std::stable_sort(ranked.begin(), ranked.end(), [&](size_t left, size_t right) {
						return FaceAreaSquared(vertices, triangles[left]) >
							   FaceAreaSquared(vertices, triangles[right]);
					});
					for (size_t index = target; index < ranked.size(); index++)
						triangles[ranked[index]][2] = triangles[ranked[index]][0];
				}
			}

			MeshData reduced;
			reduced.JointCount = source.JointCount;
			std::vector<uint32_t> remap(vertices.size(), std::numeric_limits<uint32_t>::max());
			for (uint32_t submesh = 0; submesh < runCount; submesh++) {
				Submesh run;
				if (submesh < source.Submeshes.size()) {
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
				if (submesh < source.Submeshes.size()) {
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

	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out) {
		return DecimateMesh(source, ratio, out, {});
	}

	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out, std::stop_token stop) {
		return Reduce(source, ratio, out, MeshReduction::Decimation, stop);
	}

	bool ReduceMesh(const MeshData &source, float ratio, MeshData &out) {
		return ReduceMesh(source, ratio, out, {});
	}

	bool ReduceMesh(const MeshData &source, float ratio, MeshData &out, std::stop_token stop) {
		return Reduce(source, ratio, out, MeshReduction::SurfaceArea, stop);
	}

	bool BuildMeshLodLadder(const MeshData &source, std::span<const float> ratios, std::span<MeshData> out) {
		if (ratios.size() != out.size()) {
			return false;
		}
		for (size_t level = 0; level < ratios.size(); level++) {
			if (!DecimateMesh(source, ratios[level], out[level])) {
				return false;
			}
		}
		return true;
	}

	bool BuildReducedMeshLodLadder(
		const MeshData &source, std::span<const float> ratios, std::span<MeshData> out
	) {
		if (ratios.size() != out.size()) {
			return false;
		}
		for (size_t level = 0; level < ratios.size(); level++) {
			if (!ReduceMesh(source, ratios[level], out[level])) {
				return false;
			}
		}
		return true;
	}
}
