#include <engine/assets/MeshDecimate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <unordered_map>
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

		// Which source vertices each working vertex blends, with weights.
		using Blends = std::vector<std::vector<std::pair<uint32_t, float>>>;

		uint64_t EdgeKey(uint32_t first, uint32_t second) {
			const uint32_t low = std::min(first, second);
			const uint32_t high = std::max(first, second);
			return (static_cast<uint64_t>(low) << 32u) | high;
		}

		// One collapse pass over a submesh.
		//
		// An edge is judged by its two canonical points (`welds`), so a seam
		// edge whose faces sit in two UV islands is interior. Every actual edge
		// joining the same two points is one candidate, collapsed all together
		// or not at all, and only when those edges reach every duplicate of
		// both points - a duplicate left behind would open the seam. Without
		// welds every point is one vertex and this is a plain edge collapse.
		bool CollapseBatch(
			std::vector<MeshVertex> &vertices,
			std::vector<Triangle> &triangles,
			const std::vector<uint32_t> &owners,
			const std::vector<bool> &boundary,
			uint32_t submesh,
			size_t &count,
			size_t target,
			MeshReduction reduction,
			MeshDecimationCancelToken cancel,
			std::vector<uint32_t> &welds,
			Blends *blends
		) {
			using Edge = std::array<uint32_t, 2>;
			struct Candidate {
				float Cost = 0.0f;
				uint32_t Left = 0;
				uint32_t Right = 0;
				size_t Removed = 0;
				size_t MemberFirst = 0;
				size_t MemberCount = 0;
			};
			const FaceAdjacency adjacency = BuildFaceAdjacency(vertices.size(), triangles);
			std::vector<Candidate> candidates;
			std::vector<Edge> members;
			std::unordered_map<uint64_t, bool> seen;

			// Live vertices per point, over every run, so a duplicate in another
			// material counts and blocks a collapse it could not follow.
			std::vector<uint32_t> pointUses(vertices.size(), 0);
			{
				std::vector<bool> referenced(vertices.size(), false);
				for (const Triangle &triangle : triangles)
					if (!Degenerate(triangle))
						for (const uint32_t vertex : triangle)
							referenced[vertex] = true;
				for (uint32_t vertex = 0; vertex < vertices.size(); vertex++)
					if (referenced[vertex]) pointUses[welds[vertex]]++;
			}
			const auto twinned = [&](uint32_t vertex) { return pointUses[welds[vertex]] > 1; };
			std::unordered_map<uint64_t, std::vector<Edge>> seams;
			for (size_t index = 0; index < triangles.size(); index++) {
				if (owners[index] != submesh || Degenerate(triangles[index])) continue;
				const Triangle &triangle = triangles[index];
				for (size_t corner = 0; corner < 3; corner++) {
					const uint32_t a = triangle[corner];
					const uint32_t b = triangle[(corner + 1) % 3];
					if (welds[a] == welds[b] || (!twinned(a) && !twinned(b))) continue;
					auto &edges = seams[EdgeKey(welds[a], welds[b])];
					const Edge edge{std::min(a, b), std::max(a, b)};
					if (std::find(edges.begin(), edges.end(), edge) == edges.end()) edges.push_back(edge);
				}
			}

			for (size_t index = 0; index < triangles.size(); index++) {
				if (cancel.StopRequested()) return false;
				if (owners[index] != submesh || Degenerate(triangles[index])) {
					continue;
				}
				const Triangle &triangle = triangles[index];
				for (const Edge edge : {
						 Edge{triangle[0], triangle[1]},
						 Edge{triangle[1], triangle[2]},
						 Edge{triangle[2], triangle[0]},
					 }) {
					const uint32_t left = std::min(edge[0], edge[1]);
					const uint32_t right = std::max(edge[0], edge[1]);
					if (left == right || welds[left] == welds[right] || boundary[welds[left]] ||
						boundary[welds[right]] || !SameSkin(vertices[left], vertices[right]))
						continue;
					const uint64_t key = EdgeKey(welds[left], welds[right]);
					if (!seen.emplace(key, true).second) continue;

					const size_t memberFirst = members.size();
					if (twinned(left) || twinned(right)) {
						const auto &edges = seams.at(key);
						members.insert(members.end(), edges.begin(), edges.end());
					} else {
						members.push_back({left, right});
					}
					const std::span<const Edge> group(
						members.data() + memberFirst, members.size() - memberFirst
					);

					bool legal = true;
					size_t edgeUses = 0;
					size_t removed = 0;
					std::vector<uint32_t> touched;
					for (const Edge &member : group) {
						if (boundary[welds[member[0]]] || boundary[welds[member[1]]] ||
							!SameSkin(vertices[member[0]], vertices[member[1]])) {
							legal = false;
							break;
						}
						touched.push_back(member[0]);
						touched.push_back(member[1]);
						const std::span<const size_t> leftFaces = adjacency.ForVertex(member[0]);
						const std::span<const size_t> rightFaces = adjacency.ForVertex(member[1]);
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
						if (!PreservesWinding(
								vertices, triangles, owners, adjacency, submesh, member[0], member[1]
							)) {
							legal = false;
							break;
						}
						// A shared vertex may belong to another material run. Moving it
						// would alter that run without accounting for its triangles.
						ForEachIncidentFace(adjacency, member[0], member[1], [&](size_t other) {
							if (owners[other] != submesh) legal = false;
						});
						if (!legal) break;
					}
					if (legal && edgeUses >= 2 && (twinned(left) || twinned(right))) {
						// Every duplicate of both points, each once.
						std::sort(touched.begin(), touched.end());
						legal = std::adjacent_find(touched.begin(), touched.end()) == touched.end() &&
								touched.size() == pointUses[welds[left]] + pointUses[welds[right]];
					}
					if (!legal || edgeUses < 2) {
						members.resize(memberFirst);
						continue;
					}

					float cost = EdgeCost(vertices[left], vertices[right]);
					if (reduction == MeshReduction::SurfaceArea) {
						// The product ranks a collapse by its geometric error and by
						// the expected projected area it disturbs. A large face must
						// not disappear merely because one of its edges is short.
						float area = 0.0f;
						for (const Edge &member : group)
							area += SurfaceCost(
								vertices, triangles, owners, adjacency, submesh, member[0], member[1]
							);
						cost *= area;
					}
					candidates.push_back({cost, left, right, removed, memberFirst, group.size()});
				}
			}

			std::sort(
				candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
					if (left.Cost != right.Cost) return left.Cost < right.Cost;
					return std::pair(left.Left, left.Right) < std::pair(right.Left, right.Right);
				}
			);
			std::vector<bool> claimedVertices(vertices.size(), false);
			std::vector<bool> claimedFaces(triangles.size(), false);
			std::vector<uint32_t> replacement(vertices.size());
			for (uint32_t vertex = 0; vertex < replacement.size(); vertex++)
				replacement[vertex] = vertex;
			size_t removed = 0;
			for (const Candidate &candidate : candidates) {
				if (count - removed < target + candidate.Removed) continue;
				const std::span<const Edge> group(
					members.data() + candidate.MemberFirst, candidate.MemberCount
				);
				bool overlaps = false;
				for (const Edge &member : group) {
					if (claimedVertices[member[0]] || claimedVertices[member[1]]) overlaps = true;
					ForEachIncidentFace(adjacency, member[0], member[1], [&](size_t face) {
						if (claimedFaces[face]) overlaps = true;
					});
				}
				if (overlaps) continue;
				const uint32_t point = std::min(welds[candidate.Left], welds[candidate.Right]);
				for (const Edge &member : group) {
					claimedVertices[member[0]] = true;
					claimedVertices[member[1]] = true;
					ForEachIncidentFace(adjacency, member[0], member[1], [&](size_t face) {
						claimedFaces[face] = true;
					});
					replacement[member[1]] = member[0];
					MergeVertex(vertices[member[0]], vertices[member[1]]);
					welds[member[0]] = point;
					if (blends != nullptr) {
						auto &into = (*blends)[member[0]];
						auto &other = (*blends)[member[1]];
						for (auto &entry : into)
							entry.second *= 0.5f;
						for (const auto &entry : other)
							into.emplace_back(entry.first, entry.second * 0.5f);
						other.clear();
					}
				}
				removed += candidate.Removed;
			}
			if (removed == 0) return false;
			for (Triangle &triangle : triangles) {
				if (cancel.StopRequested()) return false;
				for (uint32_t &index : triangle) {
					index = replacement[index];
				}
			}
			count -= removed;
			return true;
		}

		// Which points of one submesh lie on an open edge, by canonical point.
		std::vector<bool> BoundaryVertices(
			size_t vertexCount,
			const std::vector<Triangle> &triangles,
			const std::vector<uint32_t> &owners,
			uint32_t submesh,
			const std::vector<uint32_t> &welds
		) {
			std::unordered_map<uint64_t, size_t> edgeUses;
			for (size_t index = 0; index < triangles.size(); index++) {
				if (owners[index] != submesh || Degenerate(triangles[index])) continue;
				const Triangle &triangle = triangles[index];
				for (size_t corner = 0; corner < 3; corner++) {
					const uint32_t a = welds[triangle[corner]];
					const uint32_t b = welds[triangle[(corner + 1) % 3]];
					if (a != b) edgeUses[EdgeKey(a, b)]++;
				}
			}

			std::vector<bool> boundary(vertexCount, false);
			for (const auto &[key, uses] : edgeUses) {
				if (uses != 1) continue;
				boundary[static_cast<uint32_t>(key >> 32u)] = true;
				boundary[static_cast<uint32_t>(key)] = true;
			}
			return boundary;
		}
	}

	namespace {
		bool Reduce(
			const MeshData &source,
			float ratio,
			MeshData &out,
			MeshReduction reduction,
			MeshDecimationCancelToken cancel,
			const MeshReductionOptions &options = {}
		) {
			if (&source == &out || !source.IsValid() || !(ratio > 0.0f) || ratio > 1.0f) {
				return false;
			}
			std::vector<uint32_t> welds(source.Vertices.size());
			if (options.Welds.empty()) {
				for (uint32_t vertex = 0; vertex < welds.size(); vertex++)
					welds[vertex] = vertex;
			} else {
				if (options.Welds.size() != welds.size()) return false;
				for (size_t vertex = 0; vertex < welds.size(); vertex++) {
					if (options.Welds[vertex] >= welds.size()) return false;
					welds[vertex] = options.Welds[vertex];
				}
			}
			Blends blends;
			if (options.Sources != nullptr) {
				blends.resize(source.Vertices.size());
				for (uint32_t vertex = 0; vertex < blends.size(); vertex++)
					blends[vertex].emplace_back(vertex, 1.0f);
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
				if (cancel.StopRequested()) return false;
				size_t count = 0;
				for (size_t index = 0; index < triangles.size(); index++) {
					count += owners[index] == submesh && !Degenerate(triangles[index]) ? 1u : 0u;
				}
				if (count == 0) continue;
				const std::vector<bool> boundary =
					BoundaryVertices(vertices.size(), triangles, owners, submesh, welds);
				const size_t target = std::max<size_t>(1, static_cast<size_t>(std::floor(count * ratio)));
				while (!cancel.StopRequested() && count > target &&
					   CollapseBatch(
						   vertices,
						   triangles,
						   owners,
						   boundary,
						   submesh,
						   count,
						   target,
						   reduction,
						   cancel,
						   welds,
						   options.Sources != nullptr ? &blends : nullptr
					   )) {}
				if (cancel.StopRequested()) return false;
				// Ratio is a target, not permission to punch holes. A boundary, skin or
				// material seam can leave no legal collapse before the target is met.
				// Keep the connected remainder instead of deleting unrelated faces.
			}

			MeshData reduced;
			reduced.JointCount = source.JointCount;
			MeshVertexSources sources;
			if (options.Sources != nullptr) sources.Offsets.push_back(0);
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
							if (options.Sources != nullptr) {
								for (const auto &[vertex, weight] : blends[old]) {
									sources.Vertices.push_back(vertex);
									sources.Weights.push_back(weight);
								}
								sources.Offsets.push_back(static_cast<uint32_t>(sources.Vertices.size()));
							}
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
			if (options.Sources != nullptr) *options.Sources = std::move(sources);
			return true;
		}
	}

	bool ReduceMesh(
		const MeshData &source,
		float ratio,
		MeshData &out,
		MeshReduction reduction,
		MeshDecimationCancelToken cancel,
		const MeshReductionOptions &options
	) {
		return Reduce(source, ratio, out, reduction, cancel, options);
	}

	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out) {
		return DecimateMesh(source, ratio, out, {});
	}

	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out, MeshDecimationCancelToken cancel) {
		return Reduce(source, ratio, out, MeshReduction::Decimation, cancel);
	}

	bool ReduceMesh(const MeshData &source, float ratio, MeshData &out) {
		return ReduceMesh(source, ratio, out, {});
	}

	bool ReduceMesh(const MeshData &source, float ratio, MeshData &out, MeshDecimationCancelToken cancel) {
		return Reduce(source, ratio, out, MeshReduction::SurfaceArea, cancel);
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
