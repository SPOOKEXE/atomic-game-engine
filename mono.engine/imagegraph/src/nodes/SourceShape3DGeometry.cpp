#include "SourceShape3DGeometry.hpp"

#include "../MeshPayload.hpp"
#include "../NodeExecutors.hpp"
#include "SourceShape3DAxial.hpp"
#include "SourceShape3DRound.hpp"

#include <array>
#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>

namespace engine::imagegraph::detail {
	namespace source_shape3d_geometry {
		void Plane(MeshData3D &mesh) {
			auto &part = mesh.Parts.emplace_back();
			part.Vertices = {
				{{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
				{{.5, .5, 0}, {0, 0, 1}, {1, 1}},
				{{.5, -.5, 0}, {0, 0, 1}, {0, 1}},
				{{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
				{{-.5, .5, 0}, {0, 0, 1}, {1, 0}},
				{{.5, .5, 0}, {0, 0, 1}, {1, 1}}
			};
			mesh.Edges = {
				{{-.5, -.5, 0}, {.5, -.5, 0}},
				{{.5, -.5, 0}, {.5, .5, 0}},
				{{.5, .5, 0}, {-.5, .5, 0}},
				{{-.5, .5, 0}, {-.5, -.5, 0}}
			};
		}
		void Cube(MeshData3D &mesh) {
			constexpr std::array<Vector3, 8> POINTS{
				{{-.5, -.5, -.5},
				 {.5, -.5, -.5},
				 {-.5, .5, -.5},
				 {.5, .5, -.5},
				 {-.5, -.5, .5},
				 {.5, -.5, .5},
				 {-.5, .5, .5},
				 {.5, .5, .5}}
			};
			// grug source faces use pp00, pp01, pp10, pp11 from four source corner indices.
			constexpr std::array<std::array<size_t, 4>, 6> CORNERS{
				{{7, 5, 6, 4}, {3, 1, 2, 0}, {6, 2, 4, 0}, {5, 1, 7, 3}, {7, 3, 6, 2}, {4, 0, 5, 1}}
			};
			constexpr std::array<std::array<size_t, 6>, 6> TRIANGLES{
				{{3, 0, 1, 3, 2, 0},
				 {3, 1, 0, 3, 0, 2},
				 {2, 1, 0, 2, 3, 1},
				 {0, 2, 3, 0, 3, 1},
				 {2, 1, 0, 2, 3, 1},
				 {0, 2, 3, 0, 3, 1}}
			};
			constexpr std::array<Vector3, 6> NORMALS{
				{{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}}
			};
			constexpr std::array<Vector2, 4> UV{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
			constexpr std::array<size_t, 6> SOURCE_VB_ORDER{1, 3, 5, 0, 2, 4};
			for (size_t sourceFace : SOURCE_VB_ORDER) {
				auto &part = mesh.Parts.emplace_back();
				part.MaterialIndex = static_cast<uint32_t>(mesh.Parts.size() - 1);
				part.Vertices.reserve(6);
				for (size_t corner : TRIANGLES[sourceFace])
					part.Vertices.push_back(
						{POINTS[CORNERS[sourceFace][corner]], NORMALS[sourceFace], UV[corner]}
					);
			}
			constexpr std::array<std::array<size_t, 2>, 12> EDGES{
				{{0, 1},
				 {1, 3},
				 {3, 2},
				 {2, 0},
				 {4, 5},
				 {5, 7},
				 {7, 6},
				 {6, 4},
				 {0, 4},
				 {1, 5},
				 {2, 6},
				 {3, 7}}
			};
			for (const auto &edge : EDGES)
				mesh.Edges.push_back({POINTS[edge[0]], POINTS[edge[1]]});
		}
		void Octahedron(MeshData3D &mesh) {
			constexpr std::array<Vector3, 6> POINTS{
				{{0, 0, .5}, {0, .5, 0}, {.5, 0, 0}, {-.5, 0, 0}, {0, -.5, 0}, {0, 0, -.5}}
			};
			constexpr std::array<std::array<size_t, 3>, 8> FACES{
				{{0, 1, 2}, {0, 3, 1}, {0, 4, 3}, {0, 2, 4}, {5, 2, 1}, {5, 1, 3}, {5, 3, 4}, {5, 4, 2}}
			};
			constexpr std::array<Vector2, 6> UV{{{.5, 1}, {.25, .5}, {0, .5}, {.5, .5}, {.75, .5}, {.5, 0}}};
			for (size_t face = 0; face < FACES.size(); ++face) {
				auto &part = mesh.Parts.emplace_back();
				part.MaterialIndex = static_cast<uint32_t>(face);
				part.Vertices.reserve(3);
				for (size_t corner : FACES[face]) {
					Vector2 uv = UV[corner];
					if ((face == 3 || face == 7) && corner == 2) uv.X = 1;
					// grug source never sets octahedron normals. Keep zero rather than invent lighting.
					part.Vertices.push_back({POINTS[corner], {}, uv});
				}
			}
		}
	}

	bool BuildSourceShape3DGeometry(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		MeshValue3D &result,
		AllocationReservation &charge
	) try {
		ENGINE_PROFILE("imagegraph.shape3d.geometry");
		uint64_t vertices = 0, parts = 0, edges = 0;
		if (recipe.Side < 3 || recipe.Side > static_cast<int64_t>(Limits::MaximumArrayElements) ||
			!MeshFinite(recipe.Sides) || recipe.Sides.X < 3 || recipe.Sides.Y < 2 ||
			recipe.Sides.X > Limits::MaximumArrayElements || recipe.Sides.Y > Limits::MaximumArrayElements ||
			recipe.Sides.X != std::trunc(recipe.Sides.X) || recipe.Sides.Y != std::trunc(recipe.Sides.Y))
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D geometry requires bounded integer subdivisions", "side_2"
			);
		if (!MeshFinite(recipe.Radius) || !std::isfinite(recipe.Ratio) ||
			!std::isfinite(recipe.HeightControl) || !std::isfinite(recipe.SideScale))
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D geometry controls must be finite", "shape"
			);
		const uint64_t side = static_cast<uint64_t>(recipe.Side);
		switch (recipe.Shape) {
		case SourceShape3DKind::Plane:
			vertices = 6;
			parts = 1;
			edges = 4;
			break;
		case SourceShape3DKind::Cube:
			vertices = 36;
			parts = 6;
			edges = 12;
			break;
		case SourceShape3DKind::Octahedron:
			vertices = 24;
			parts = 8;
			break;
		case SourceShape3DKind::Cylinder:
			vertices = side * (recipe.Caps ? 12 : 6);
			parts = recipe.Caps ? 3 : 1;
			edges = side * 4;
			break;
		case SourceShape3DKind::Cone:
			vertices = side * 6;
			parts = 2;
			edges = side * 2;
			break;
		case SourceShape3DKind::Capsule:
			if (recipe.HeightControl == 0)
				return context.Fail(
					Status::InvalidValue, "source Capsule has undefined zero-height normals", "height"
				);
			vertices = side * 6 + side * side * 12;
			parts = 3;
			edges = side * 2;
			break;
		case SourceShape3DKind::Sphere:
			vertices = static_cast<uint64_t>(recipe.Sides.X) * static_cast<uint64_t>(recipe.Sides.Y) * 6;
			parts = 1;
			break;
		case SourceShape3DKind::CutSphere: {
			if (recipe.Ratio <= 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"source Cut Sphere retains a prior cached model at nonpositive ratio",
					"ratio"
				);
			const double latitude = std::ceil(recipe.Sides.X * recipe.Ratio);
			if (!std::isfinite(latitude) || latitude > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Cut Sphere latitude count exceeds bounded geometry", "ratio"
				);
			vertices = static_cast<uint64_t>(recipe.Sides.Y) * (static_cast<uint64_t>(latitude) * 6 + 3);
			parts = 2;
			break;
		}
		case SourceShape3DKind::Torus:
			vertices = 16 * 8 * 6;
			parts = 1;
			break;
		default:
			return context.Fail(Status::InvalidValue, "Draw Shape 3D shape selector is invalid", "shape");
		}
		if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D geometry exceeds element bounds", "shape"
			);
		// 64 units per source vertex conservatively cover constructor trig and normal math.
		const uint64_t rows = context.ProcessorCount ? context.ProcessorCount : 1;
		if (rows > 64000000 || vertices * 64 > 64000000 / rows)
			return context.Fail(
				Status::LimitExceeded, "Shape 3D geometry exceeds whole processor work bounds", "shape"
			);
		const uint64_t bytes = sizeof(MeshData3D) + sizeof(MeshTransform3D) +
							   parts * (sizeof(MeshPart3D) + sizeof(MaterialValue3D)) +
							   vertices * sizeof(MeshVertex3D) + edges * sizeof(MeshEdge3D);
		if (bytes > Limits::MaximumArrayBytes)
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D geometry exceeds payload bytes", "shape"
			);
		auto reservation = context.ReserveWorkspace(bytes, "shape");
		if (!reservation) return false;
		MeshValue3D prepared;
		auto &mesh = prepared.Data.emplace();
		mesh.Parts.reserve(static_cast<size_t>(parts));
		mesh.Edges.reserve(static_cast<size_t>(edges));
		mesh.Materials.reserve(static_cast<size_t>(parts));
		for (uint64_t part = 0; part < parts; ++part)
			mesh.Materials.emplace_back();
		mesh.LocalTransforms.reserve(1);
		mesh.LocalTransforms.emplace_back();
		switch (recipe.Shape) {
		case SourceShape3DKind::Plane:
			source_shape3d_geometry::Plane(mesh);
			break;
		case SourceShape3DKind::Cube:
			source_shape3d_geometry::Cube(mesh);
			break;
		case SourceShape3DKind::Octahedron:
			source_shape3d_geometry::Octahedron(mesh);
			break;
		case SourceShape3DKind::Cylinder:
		case SourceShape3DKind::Cone:
		case SourceShape3DKind::Capsule:
			FillSourceShape3DAxial(recipe, mesh);
			break;
		case SourceShape3DKind::Sphere:
		case SourceShape3DKind::CutSphere:
		case SourceShape3DKind::Torus:
			FillSourceShape3DRound(recipe, mesh);
			break;
		}
		if (!ValidMeshPayload(prepared))
			return context.Fail(
				Status::InvalidValue, "source Draw Shape 3D geometry contains nonfinite values", "shape"
			);
		if (MeshStorageBytes<true>(prepared) > bytes)
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D geometry exceeded its admitted storage", "shape"
			);
		result = std::move(prepared);
		charge = std::move(*reservation);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Draw Shape 3D geometry allocation was refused", "shape");
	} catch (const std::length_error &) {
		return context.Fail(
			Status::LimitExceeded, "Draw Shape 3D geometry allocation length exceeds storage", "shape"
		);
	}
}
