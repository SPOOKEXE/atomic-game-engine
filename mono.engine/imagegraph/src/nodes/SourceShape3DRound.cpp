#include "SourceShape3DRound.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace source_shape3d_round {
		double Sin(double degrees) {
			return std::sin(degrees * std::numbers::pi / 180);
		}
		double Cos(double degrees) {
			return std::cos(degrees * std::numbers::pi / 180);
		}
		Vector3 Sub(Vector3 a, Vector3 b) {
			return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
		}
		Vector3 FlatNormal(Vector3 a, Vector3 b, Vector3 c) {
			const Vector3 u = Sub(c, a), v = Sub(b, a);
			Vector3 n{u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X};
			const double length = std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
			n = {n.X / length, n.Y / length, n.Z / length};
			return n;
		}
		Vector3 SpherePoint(double longitude, double latitude) {
			const double radius = Cos(latitude) * .5;
			return {Cos(longitude) * radius, Sin(longitude) * radius, Sin(latitude) * .5};
		}
		void FillSphere(const SourceShape3DRecipe &recipe, MeshData3D &mesh, uint64_t rows) {
			const bool cut = recipe.Shape == SourceShape3DKind::CutSphere;
			const uint64_t longitudeCount = static_cast<uint64_t>(recipe.Sides.Y);
			const double top = -90 + 180 * recipe.Ratio;
			auto &vertices = mesh.Parts[0].Vertices;
			vertices.reserve(longitudeCount * rows * 6);
			for (uint64_t i = 0; i < longitudeCount; ++i) {
				const double ha0 = double(i) / longitudeCount * 360,
							 ha1 = double(i + 1) / longitudeCount * 360;
				for (uint64_t j = 0; j < rows; ++j) {
					const double va0 = cut ? std::min(-90 + double(j + 1) / recipe.Sides.X * 180, top)
										   : 90 - double(j) / recipe.Sides.X * 180;
					const double va1 = cut ? -90 + double(j) / recipe.Sides.X * 180
										   : 90 - double(j + 1) / recipe.Sides.X * 180;
					const std::array<Vector3, 4> p{
						SpherePoint(ha0, va0),
						SpherePoint(ha1, va0),
						SpherePoint(ha0, va1),
						SpherePoint(ha1, va1)
					};
					const Vector3 flat = FlatNormal(p[0], p[1], p[2]);
					const double u0 = ha0 / 360 * recipe.SideScale, u1 = ha1 / 360 * recipe.SideScale;
					const double v0 = .5 - .5 * Sin(va0), v1 = .5 - .5 * Sin(va1);
					const std::array<Vector2, 4> uv{{{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}}};
					for (const size_t index : {0u, 1u, 2u, 1u, 3u, 2u})
						vertices.push_back({p[index], recipe.Smooth ? p[index] : flat, uv[index]});
				}
			}
			if (!cut) return;
			auto &cap = mesh.Parts[1].Vertices;
			cap.reserve(longitudeCount * 3);
			const double height = Sin(top) * .5, radius = Cos(top) * .5;
			for (uint64_t i = 0; i < longitudeCount; ++i) {
				const double a0 = double(i) / longitudeCount * 360, a1 = double(i + 1) / longitudeCount * 360;
				cap.push_back(
					{{Cos(a1) * radius, Sin(a1) * radius, height},
					 {0, 0, 1},
					 {.5 + Cos(a1) * .5, .5 + Sin(a1) * .5}}
				);
				cap.push_back(
					{{Cos(a0) * radius, Sin(a0) * radius, height},
					 {0, 0, 1},
					 {.5 + Cos(a0) * .5, .5 + Sin(a0) * .5}}
				);
				cap.push_back({{0, 0, height}, {0, 0, 1}, {.5, .5}});
			}
		}
		void FillTorus(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
			// Source node writes hori/vert, but constructor reads unchanged sideT/sideP defaults.
			constexpr uint64_t SIDE_T = 16, SIDE_P = 8;
			auto &vertices = mesh.Parts[0].Vertices;
			vertices.reserve(SIDE_T * SIDE_P * 6);
			for (uint64_t i = 0; i < SIDE_T; ++i) {
				const double a0 = double(i) / SIDE_T * 360, a1 = double(i + 1) / SIDE_T * 360;
				const Vector3 center0{Cos(a0) * recipe.Radius.X, -Sin(a0) * recipe.Radius.X, 0};
				const Vector3 center1{Cos(a1) * recipe.Radius.X, -Sin(a1) * recipe.Radius.X, 0};
				for (uint64_t j = 0; j < SIDE_P; ++j) {
					const double b0 = double(j) / SIDE_P * 360, b1 = double(j + 1) / SIDE_P * 360;
					const auto point = [&](double a, double b) -> Vector3 {
						const double radius = recipe.Radius.X + Cos(b) * recipe.Radius.Y;
						return {Cos(a) * radius, -Sin(a) * radius, -Sin(b) * recipe.Radius.Y};
					};
					const std::array<Vector3, 4> p{
						point(a0, b0), point(a1, b0), point(a1, b1), point(a0, b1)
					};
					const Vector3 flat{
						(p[0].X + p[1].X + p[2].X + p[3].X) / 4 - (center0.X + center1.X) / 2,
						(p[0].Y + p[1].Y + p[2].Y + p[3].Y) / 4 - (center0.Y + center1.Y) / 2,
						(p[0].Z + p[1].Z + p[2].Z + p[3].Z) / 4
					};
					const std::array<Vector3, 4> normals{
						Sub(p[0], center0), Sub(p[1], center1), Sub(p[2], center1), Sub(p[3], center0)
					};
					const double u0 = (1 - double(i) / SIDE_T) * recipe.SideScale,
								 u1 = (1 - double(i + 1) / SIDE_T) * recipe.SideScale;
					const double v0 = 1 - double(j) / SIDE_P, v1 = 1 - double(j + 1) / SIDE_P;
					const std::array<Vector2, 4> uv{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
					for (const size_t index : {0u, 2u, 1u, 0u, 3u, 2u})
						vertices.push_back({p[index], recipe.Smooth ? normals[index] : flat, uv[index]});
				}
			}
		}
	}
	bool SourceShape3DRoundCounts(const SourceShape3DRecipe &recipe, std::array<uint64_t, 2> &counts) {
		counts = {};
		if (recipe.Shape == SourceShape3DKind::Torus) {
			counts[0] = 16 * 8 * 6;
			return true;
		}
		if (!std::isfinite(recipe.Sides.X) || !std::isfinite(recipe.Sides.Y) || recipe.Sides.X < 1 ||
			recipe.Sides.Y < 1 || recipe.Sides.X > 4096 || recipe.Sides.Y > 4096)
			return false;
		const double rows = recipe.Shape == SourceShape3DKind::CutSphere
								? std::ceil(recipe.Sides.X * recipe.Ratio)
								: recipe.Sides.X;
		if (!std::isfinite(rows) || rows <= 0 || rows > double(std::numeric_limits<uint64_t>::max() / 24576))
			return false;
		counts[0] = uint64_t(rows) * uint64_t(recipe.Sides.Y) * 6;
		if (recipe.Shape == SourceShape3DKind::CutSphere) counts[1] = uint64_t(recipe.Sides.Y) * 3;
		return true;
	}
	void FillSourceShape3DRound(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
		std::array<uint64_t, 2> counts{};
		if (!SourceShape3DRoundCounts(recipe, counts)) return;
		mesh.Parts.resize(counts[1] ? 2 : 1);
		for (size_t i = 0; i < mesh.Parts.size(); ++i) {
			mesh.Parts[i].MaterialIndex = static_cast<uint32_t>(i);
			mesh.Parts[i].Vertices.clear();
		}
		if (recipe.Shape == SourceShape3DKind::Torus)
			source_shape3d_round::FillTorus(recipe, mesh);
		else
			source_shape3d_round::FillSphere(recipe, mesh, counts[0] / uint64_t(recipe.Sides.Y) / 6);
	}
}
