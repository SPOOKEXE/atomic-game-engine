#include "../SourceMeshBuild.hpp"
#include "Path3D.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	bool SourceMeshPathRevolve(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.path_revolve");
		const Value *input = context.Find("path");
		const auto *spatial = input ? std::get_if<PathValue3D>(input) : nullptr;
		const auto *planar = input ? std::get_if<Path2D>(input) : nullptr;
		if (!input || (spatial && (!spatial->Data || !spatial->Data->SourcePresent)) ||
			(planar && planar->Anchors.empty() && !planar->SourceOperation && !context.IsLinked("path"))) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!spatial && !planar)
			return context.Fail(Status::TypeMismatch, "path revolve requires a path", "path");
		if (!ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "path revolve source is invalid", "path");
		const int64_t samples = context.Integer("path_sample", 8),
					  sides = context.Integer("revolve_sample", 8), caps = context.Integer("caps");
		const double scale = context.Scalar("path_scale", .25),
					 axis = context.SourceChoice("project_normal", 2);
		const bool invert = context.Boolean("invert_y"), reverse = context.Boolean("invert_normal"),
				   smooth = context.Boolean("smooth");
		if (context.FailureCode != Status::Ok) return false;
		if (samples < 2 || sides < 3 || samples > int64_t(Limits::MaximumArrayElements) ||
			sides > int64_t(Limits::MaximumArrayElements))
			return context.Fail(
				Status::LimitExceeded, "path revolve sample count exceeds geometry caps", "path_sample"
			);
		if (caps < 0 || caps > 3 || !std::isfinite(scale))
			return context.Fail(Status::InvalidValue, "path revolve controls are invalid", "caps");
		if (axis != 0 && axis != 1 && axis != 2)
			return context.Fail(
				Status::UnsupportedExecution, "path projection axis has no source case", "project_normal"
			);
		const uint64_t quads = uint64_t(samples) * sides,
					   capCount = uint64_t(bool(caps & 1)) + bool(caps & 2),
					   vertices = quads * 6 + capCount * sides * 3, edges = quads * 4;
		if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "path revolve exceeds vertex or edge caps", "path_sample"
			);
		MeshTransform3D transform;
		SourceBuildMaterial side, cap;
		if (!ReadSourceBuildTransform(context, transform) ||
			!ReadSourceBuildMaterial(context, "material_side", side) ||
			!ReadSourceBuildMaterial(context, "material_cap", cap))
			return false;
		const uint64_t bytes = sizeof(MeshData3D) + vertices * sizeof(MeshVertex3D) +
							   edges * sizeof(MeshEdge3D) +
							   (capCount + 1) * (sizeof(MeshPart3D) + sizeof(MaterialValue3D)) +
							   sizeof(MeshTransform3D) + side.Bytes() + capCount * cap.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "path revolve exceeds mesh payload caps", "mesh");
		auto scratch = context.ReserveWorkspace((samples + 1) * sizeof(Vector2), "path");
		if (!scratch) return false;
		std::optional<PathRuntime> flat;
		std::optional<PathRuntime3D> space;
		if (spatial) {
			space.emplace(*spatial->Data, &context);
			if (!space->Valid())
				return context.Fail(Status::InvalidValue, "spatial path length is invalid", "path");
		} else {
			flat.emplace();
			if (!flat->Init(context, *planar)) return false;
		}
		std::vector<Vector2> points;
		points.reserve(size_t(samples) + 1);
		for (int64_t i = 0; i <= samples; ++i) {
			const double ratio = (1.0 / (samples - 1)) * i;
			Vector3 p;
			if (space)
				p = space->Ratio(ratio).Position;
			else {
				const auto point = flat->PointRatio(ratio);
				p = {point.X, point.Y, 0};
			}
			Vector2 point = axis == 0 ? Vector2{p.Y * scale, p.Z * scale}
									  : (axis == 1 ? Vector2{p.X * scale, p.Z * scale}
												   : Vector2{p.X * scale, p.Y * scale});
			if (invert) point.Y = -point.Y;
			if (!MeshFinite(point))
				return context.Fail(Status::InvalidValue, "path revolve sample is nonfinite", "path");
			points.push_back(point);
		}
		if (reverse) std::reverse(points.begin(), points.end());
		MeshValue3D result;
		auto &mesh = result.Data.emplace();
		mesh.Parts.reserve(size_t(capCount) + 1);
		mesh.Materials.reserve(size_t(capCount) + 1);
		mesh.LocalTransforms.push_back(transform);
		mesh.Edges.reserve(size_t(edges));
		mesh.Materials.push_back(side.Clone());
		mesh.Parts.emplace_back();
		auto &surface = mesh.Parts.back();
		surface.Vertices.reserve(size_t(quads * 6));
		const double angleStep = 360.0 / sides, uStep = 1.0 / sides, vStep = 1.0 / points.size();
		for (int64_t i = 0; i < sides; ++i) {
			const double angle = i * angleStep;
			const double a0 = angle * std::numbers::pi / 180,
						 a1 = (angle + angleStep) * std::numbers::pi / 180, u0 = i * uStep, u1 = u0 + uStep;
			for (int64_t j = 0; j < samples; ++j) {
				const auto p0 = points[size_t(j)], p1 = points[size_t(j) + 1];
				const std::array<Vector3, 4> positions{
					{{p0.X * std::cos(a0), -p0.X * std::sin(a0), -p0.Y},
					 {p1.X * std::cos(a0), -p1.X * std::sin(a0), -p1.Y},
					 {p1.X * std::cos(a1), -p1.X * std::sin(a1), -p1.Y},
					 {p0.X * std::cos(a1), -p0.X * std::sin(a1), -p0.Y}}
				};
				const double v0 = j * vStep, v1 = v0 + vStep;
				std::array<Vector3, 4> normals;
				if (smooth) {
					for (size_t k = 0; k < 4; ++k) {
						const double radius = k == 0 || k == 3 ? p0.X : p1.X;
						normals[k] = {positions[k].X / radius, positions[k].Y / radius, 0};
					}
				} else {
					const auto a = positions[0], b = positions[1], c = positions[3];
					normals.fill(SourceBuildNormal(
						{a.X - b.X, a.Y - b.Y, a.Z - b.Z}, {c.X - a.X, c.Y - a.Y, c.Z - a.Z}
					));
				}
				const std::array<Vector2, 4> uv{{{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}}};
				for (size_t k : {0u, 1u, 2u, 2u, 3u, 0u})
					surface.Vertices.push_back({positions[k], normals[k], uv[k]});
				for (size_t k = 0; k < 4; ++k)
					mesh.Edges.push_back({positions[k], positions[(k + 1) % 4]});
			}
		}
		for (unsigned end = 0; end < 2; ++end) {
			if (!(caps & (1 << end))) continue;
			const auto p = end ? points.back() : points.front();
			mesh.Materials.push_back(cap.Clone());
			mesh.Parts.emplace_back();
			auto &part = mesh.Parts.back();
			part.MaterialIndex = uint32_t(mesh.Materials.size() - 1);
			part.Vertices.reserve(size_t(sides * 3));
			for (int64_t i = 0; i < sides; ++i) {
				const double angle = i * angleStep;
				const double a0 = angle * std::numbers::pi / 180,
							 a1 = (angle + angleStep) * std::numbers::pi / 180;
				const Vector3 normal{0, 0, end ? -1.0 : 1.0};
				const Vector2 p0{std::cos(a0), -std::sin(a0)}, p1{std::cos(a1), -std::sin(a1)};
				const MeshVertex3D center{{0, 0, -p.Y}, normal, {.5, .5}},
					first{{p0.X * p.X, p0.Y * p.X, -p.Y}, normal, {.5 + p0.X * .5, .5 + p0.Y * .5}},
					second{{p1.X * p.X, p1.Y * p.X, -p.Y}, normal, {.5 + p1.X * .5, .5 + p1.Y * .5}};
				part.Vertices.push_back(center);
				part.Vertices.push_back(end ? second : first);
				part.Vertices.push_back(end ? first : second);
			}
		}
		// The pinned object accepts Origin and Revolve Axis parameters but its geometry never reads them.
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "path revolve geometry is nonfinite", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
