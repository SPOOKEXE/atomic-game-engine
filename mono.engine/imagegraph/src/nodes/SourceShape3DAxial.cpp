#include "SourceShape3DAxial.hpp"

#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace source_shape3d_axial {
		Vector3 Ring(double turn) {
			const double angle = turn * 2 * std::numbers::pi;
			return {std::cos(angle), -std::sin(angle), 0};
		}
		MeshVertex3D Vertex(Vector3 position, Vector3 normal, Vector2 uv) {
			return {position, normal, uv};
		}
		void Edge(MeshData3D &mesh, Vector3 from, Vector3 to) {
			mesh.Edges.push_back({from, to});
		}
		void Body(const SourceShape3DRecipe &recipe, MeshData3D &mesh, double halfHeight) {
			auto &vertices = mesh.Parts[0].Vertices;
			vertices.reserve(6 * recipe.Side);
			for (int64_t side = 0; side < recipe.Side; ++side) {
				const double u0 = double(side) / recipe.Side, u1 = double(side + 1) / recipe.Side;
				const Vector3 ring0 = Ring(u0), ring1 = Ring(u1), middle = Ring((u0 + u1) / 2);
				const Vector3 normal0 = recipe.Smooth ? ring0 : middle;
				const Vector3 normal1 = recipe.Smooth ? ring1 : middle;
				const Vector3 bottom0{ring0.X * .5, ring0.Y * .5, -halfHeight};
				const Vector3 bottom1{ring1.X * .5, ring1.Y * .5, -halfHeight};
				const Vector3 top0{bottom0.X, bottom0.Y, halfHeight};
				const Vector3 top1{bottom1.X, bottom1.Y, halfHeight};
				const double scaled0 = u0 * recipe.SideScale, scaled1 = u1 * recipe.SideScale;
				vertices.push_back(Vertex(top0, normal0, {scaled0, 1}));
				vertices.push_back(Vertex(bottom0, normal0, {scaled0, 0}));
				vertices.push_back(Vertex(top1, normal1, {scaled1, 1}));
				vertices.push_back(Vertex(bottom0, normal0, {scaled0, 0}));
				vertices.push_back(Vertex(bottom1, normal1, {scaled1, 0}));
				vertices.push_back(Vertex(top1, normal1, {scaled1, 1}));
				Edge(mesh, bottom0, top0);
				Edge(mesh, bottom1, top1);
			}
		}
		void Cylinder(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
			mesh.Parts.resize(recipe.Caps ? 3 : 1);
			mesh.Edges.reserve(4 * recipe.Side);
			if (recipe.Caps) {
				mesh.Parts[1].Vertices.reserve(3 * recipe.Side);
				mesh.Parts[2].Vertices.reserve(3 * recipe.Side);
			}
			for (int64_t side = 0; side < recipe.Side; ++side) {
				const auto a = Ring(double(side) / recipe.Side), b = Ring(double(side + 1) / recipe.Side);
				const Vector2 ua{.5 + a.X * .5, .5 + a.Y * .5}, ub{.5 + b.X * .5, .5 + b.Y * .5};
				const Vector3 at{a.X * .5, a.Y * .5, .5}, bt{b.X * .5, b.Y * .5, .5};
				const Vector3 ab{at.X, at.Y, -.5}, bb{bt.X, bt.Y, -.5};
				if (recipe.Caps) {
					auto &top = mesh.Parts[1].Vertices;
					auto &bottom = mesh.Parts[2].Vertices;
					top.push_back(Vertex({0, 0, .5}, {0, 0, 1}, {.5, .5}));
					top.push_back(Vertex(at, {0, 0, 1}, ua));
					top.push_back(Vertex(bt, {0, 0, 1}, ub));
					bottom.push_back(Vertex({0, 0, -.5}, {0, 0, -1}, {.5, .5}));
					bottom.push_back(Vertex(bb, {0, 0, -1}, ub));
					bottom.push_back(Vertex(ab, {0, 0, -1}, ua));
				}
				Edge(mesh, at, bt);
				Edge(mesh, ab, bb);
			}
			Body(recipe, mesh, .5);
		}
		void Cone(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
			// Source exposes caps but its cone always retains the bottom buffer.
			mesh.Parts.resize(2);
			mesh.Edges.reserve(2 * recipe.Side);
			for (auto &part : mesh.Parts)
				part.Vertices.reserve(3 * recipe.Side);
			for (int64_t side = 0; side < recipe.Side; ++side) {
				const double u0 = double(side) / recipe.Side, u1 = double(side + 1) / recipe.Side;
				const auto a = Ring(u0), b = Ring(u1);
				const Vector3 ab{a.X * .5, a.Y * .5, -.5}, bb{b.X * .5, b.Y * .5, -.5};
				auto &bottom = mesh.Parts[0].Vertices;
				bottom.push_back(Vertex({0, 0, -.5}, {0, 0, -1}, {.5, .5}));
				bottom.push_back(Vertex(bb, {0, 0, -1}, {.5 + b.X * .5, .5 + b.Y * .5}));
				bottom.push_back(Vertex(ab, {0, 0, -1}, {.5 + a.X * .5, .5 + a.Y * .5}));
				Edge(mesh, ab, bb);
			}
			for (int64_t side = 0; side < recipe.Side; ++side) {
				const double u0 = double(side) / recipe.Side, u1 = double(side + 1) / recipe.Side;
				const auto a = Ring(u0), b = Ring(u1), middle = Ring((u0 + u1) / 2);
				const auto na = recipe.Smooth ? a : middle, nb = recipe.Smooth ? b : middle;
				const Vector3 ab{a.X * .5, a.Y * .5, -.5}, bb{b.X * .5, b.Y * .5, -.5};
				auto &vertices = mesh.Parts[1].Vertices;
				// Source cone normals retain nz=.2 without normalization.
				vertices.push_back(
					Vertex({0, 0, .5}, {middle.X, middle.Y, .2}, {(u0 + u1) * recipe.SideScale / 2, 0})
				);
				vertices.push_back(Vertex(ab, {na.X, na.Y, .2}, {u0 * recipe.SideScale, 1}));
				vertices.push_back(Vertex(bb, {nb.X, nb.Y, .2}, {u1 * recipe.SideScale, 1}));
				Edge(mesh, ab, {0, 0, .5});
			}
		}
		void Capsule(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
			mesh.Parts.resize(3);
			mesh.Edges.reserve(2 * recipe.Side);
			for (uint32_t part = 1; part < 3; ++part)
				mesh.Parts[part].Vertices.reserve(6 * recipe.Side * recipe.Side);
			const double halfHeight = recipe.HeightControl / 2;
			for (int64_t side = 0; side < recipe.Side; ++side)
				for (int64_t latitude = 0; latitude < recipe.Side; ++latitude) {
					const double u0 = double(side) / recipe.Side, u1 = double(side + 1) / recipe.Side;
					const double angle0 = double(latitude) / recipe.Side * std::numbers::pi / 2;
					const double angle1 = double(latitude + 1) / recipe.Side * std::numbers::pi / 2;
					const auto a = Ring(u0), b = Ring(u1);
					const double r0 = std::cos(angle0) * .5, r1 = std::cos(angle1) * .5;
					const double h0 = std::sin(angle0) * .5, h1 = std::sin(angle1) * .5;
					// Hemisphere source uses dsin, opposite to the body's lengthdir_y.
					const std::array<Vector3, 4> points{
						{{a.X * r0, -a.Y * r0, h0},
						 {b.X * r0, -b.Y * r0, h0},
						 {a.X * r1, -a.Y * r1, h1},
						 {b.X * r1, -b.Y * r1, h1}}
					};
					const Vector3 v{
						points[2].X - points[0].X, points[2].Y - points[0].Y, points[2].Z - points[0].Z
					};
					const Vector3 w{
						points[1].X - points[0].X, points[1].Y - points[0].Y, points[1].Z - points[0].Z
					};
					Vector3 flat{v.Y * w.Z - v.Z * w.Y, v.Z * w.X - v.X * w.Z, v.X * w.Y - v.Y * w.X};
					const double length = std::sqrt(flat.X * flat.X + flat.Y * flat.Y + flat.Z * flat.Z);
					if (length != 0) flat = {flat.X / length, flat.Y / length, flat.Z / length};
					const std::array<Vector2, 4> uv{
						{{u0 * recipe.SideScale, .5 - h0},
						 {u1 * recipe.SideScale, .5 - h0},
						 {u0 * recipe.SideScale, .5 - h1},
						 {u1 * recipe.SideScale, .5 - h1}}
					};
					for (uint32_t cap = 0; cap < 2; ++cap) {
						const std::array<uint32_t, 6> order = cap == 0
																  ? std::array<uint32_t, 6>{0, 2, 1, 1, 2, 3}
																  : std::array<uint32_t, 6>{0, 1, 2, 1, 3, 2};
						for (const uint32_t corner : order) {
							const auto p = points[corner], n = recipe.Smooth ? p : flat;
							const double sign = cap == 0 ? 1 : -1;
							mesh.Parts[cap + 1].Vertices.push_back(Vertex(
								{p.X, p.Y, sign * (p.Z + halfHeight)},
								{n.X, n.Y, sign * n.Z},
								{uv[corner].X, cap == 0 ? uv[corner].Y : 1 - uv[corner].Y}
							));
						}
					}
				}
			Body(recipe, mesh, halfHeight);
		}
	}
	void FillSourceShape3DAxial(const SourceShape3DRecipe &recipe, MeshData3D &mesh) {
		if (recipe.Shape == SourceShape3DKind::Cylinder)
			source_shape3d_axial::Cylinder(recipe, mesh);
		else if (recipe.Shape == SourceShape3DKind::Cone)
			source_shape3d_axial::Cone(recipe, mesh);
		else if (recipe.Shape == SourceShape3DKind::Capsule)
			source_shape3d_axial::Capsule(recipe, mesh);
		for (uint32_t part = 0; part < mesh.Parts.size(); ++part)
			mesh.Parts[part].MaterialIndex = mesh.Materials.empty() ? 0 : part % mesh.Materials.size();
	}
}
