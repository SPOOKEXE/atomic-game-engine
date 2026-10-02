#include "../SourceMeshBuild.hpp"
#include "Curve.hpp"
#include "Path3D.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		Vector3 Difference(Vector3 a, Vector3 b) {
			return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
		}
		Vector3 Unit(Vector3 a) {
			double l = std::hypot(a.X, a.Y, a.Z);
			return l == 0 ? a : Vector3{a.X / l, a.Y / l, a.Z / l};
		}
		Vector3 Cross(Vector3 a, Vector3 b) {
			return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
		}
		Vector3 RingOffset(Vector3 direction, double angle) {
			const Vector3 axis = direction == Vector3{0, 0, 1} ? Vector3{0, 1, 0} : Vector3{0, 0, 1};
			const Vector3 u = Unit(Cross(direction, axis)), w = Cross(direction, u);
			const double radians = angle * std::numbers::pi / 180;
			return {
				u.X * std::cos(radians) + w.X * std::sin(radians),
				u.Y * std::cos(radians) + w.Y * std::sin(radians),
				u.Z * std::cos(radians) + w.Z * std::sin(radians)
			};
		}
		Vector3 AtRing(Vector3 center, Vector3 offset, double radius) {
			return {center.X + offset.X * radius, center.Y + offset.Y * radius, center.Z + offset.Z * radius};
		}
	}
	bool SourceMeshPathExtrude(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.path_extrude");
		const Value *input = context.Find("path");
		const auto *spatial = input ? std::get_if<PathValue3D>(input) : nullptr;
		const auto *planar = input ? std::get_if<Path2D>(input) : nullptr;
		if (!input || (spatial && (!spatial->Data || !spatial->Data->SourcePresent)) ||
			(planar && planar->Anchors.empty() && !planar->SourceOperation && !context.IsLinked("path"))) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!spatial && !planar)
			return context.Fail(Status::TypeMismatch, "path extrusion requires a path", "path");
		if (!ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "path extrusion source is invalid", "path");
		const int64_t samples = context.Integer("subdivision", 8), sides = context.Integer("side", 8);
		const double scale = context.Scalar("path_scale", .1), radius = context.Scalar("radius", .25),
					 yaw = context.Scalar("profile_angle");
		const bool loop = context.Boolean("loop"), caps = context.Boolean("end_caps", true) && !loop,
				   smooth = context.Boolean("smooth"), invert = context.Boolean("inverted");
		Vector2 uvScale{1, 1};
		if (const Value *v = context.Find("texture_scale")) {
			if (const auto *p = std::get_if<Vector2>(v))
				uvScale = *p;
			else
				return context.Fail(Status::TypeMismatch, "texture scale requires Vector2", "texture_scale");
		}
		const Curve *curve = nullptr;
		if (const Value *v = context.Find("radius_over_path")) {
			curve = std::get_if<Curve>(v);
			if (!curve || !ValidRuntimeValue(*v))
				return context.Fail(
					Status::TypeMismatch, "radius over path requires a valid curve", "radius_over_path"
				);
		}
		if (context.FailureCode != Status::Ok) return false;
		if (samples < 2 || sides < 2 || samples > int64_t(Limits::MaximumArrayElements) ||
			sides > int64_t(Limits::MaximumArrayElements))
			return context.Fail(
				Status::LimitExceeded, "path extrusion sampling exceeds geometry caps", "subdivision"
			);
		if (!std::isfinite(scale) || !std::isfinite(radius) || !std::isfinite(yaw) || !MeshFinite(uvScale))
			return context.Fail(Status::InvalidValue, "path extrusion controls must be finite", "radius");
		const uint64_t quads = uint64_t(samples - 1) * sides, vertices = quads * 6 + (caps ? 6 * sides : 0),
					   edges = quads * 4 + 2 * sides;
		if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "path extrusion exceeds vertex or edge caps", "subdivision"
			);
		SourceBuildMaterial side, cap;
		MeshTransform3D transform;
		if (!ReadSourceBuildMaterial(context, "material_side", side) ||
			!ReadSourceBuildMaterial(context, "material_cap", cap) ||
			!ReadSourceBuildTransform(context, transform))
			return false;
		const uint64_t bytes = sizeof(MeshData3D) + vertices * sizeof(MeshVertex3D) +
							   edges * sizeof(MeshEdge3D) + (caps ? 3 : 1) * sizeof(MeshPart3D) +
							   2 * sizeof(MaterialValue3D) + sizeof(MeshTransform3D) + side.Bytes() +
							   cap.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "path extrusion exceeds mesh payload caps", "mesh");
		auto scratch = context.ReserveWorkspace(
			samples * (sizeof(Vector3) + 2 * sizeof(double)) + 3 * (sides + 1) * sizeof(Vector3), "path"
		);
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
		std::vector<Vector3> points(static_cast<size_t>(samples));
		std::vector<double> progress(size_t(samples), 0), radii(size_t(samples), radius);
		double total = 0;
		const double step = 1.0 / (samples - 1);
		for (int64_t i = 0; i < samples; ++i) {
			double ratio = step * i;
			if (!loop) ratio = std::clamp(ratio, 0.0, .999);
			Vector3 p;
			if (space)
				p = space->Ratio(ratio).Position;
			else {
				auto a = flat->PointRatio(ratio);
				p = {a.X, a.Y, 0};
			}
			points[size_t(i)] = {p.X * scale, p.Y * scale, p.Z * scale};
			if (i) {
				auto d = Difference(points[size_t(i)], points[size_t(i) - 1]);
				total += std::hypot(d.X, d.Y, d.Z);
				progress[size_t(i)] = total;
			}
			if (curve) radii[size_t(i)] *= EvalCurveX(*curve, step * i);
		}
		if (!std::isfinite(total) || total == 0)
			return context.Fail(
				Status::InvalidValue, "path extrusion has no finite sampling distance", "path"
			);
		for (double &p : progress)
			p /= total;
		MeshValue3D output;
		auto &mesh = output.Data.emplace();
		mesh.Parts.resize(caps ? 3 : 1);
		mesh.Materials = {side.Clone(), cap.Clone()};
		mesh.LocalTransforms.push_back(transform);
		mesh.Edges.reserve(size_t(edges));
		mesh.Parts[0].Vertices.reserve(size_t(quads * 6));
		if (caps) {
			mesh.Parts[1].MaterialIndex = mesh.Parts[2].MaterialIndex = 1;
			mesh.Parts[1].Vertices.reserve(size_t(sides * 3));
			mesh.Parts[2].Vertices.reserve(size_t(sides * 3));
		}
		std::vector<Vector3> previous(size_t(sides) + 1), current(size_t(sides) + 1),
			first(size_t(sides) + 1);
		const double angleStep = 360.0 / sides;
		Vector3 origin = points[0],
				direction = Unit(Difference(points[1], loop ? points[size_t(samples - 2)] : origin));
		for (int64_t j = 0; j <= sides; ++j)
			previous[size_t(j)] = AtRing(origin, RingOffset(direction, yaw + j * angleStep), radii[0]);
		first = previous;
		auto capVertices =
			[&](unsigned end, Vector3 center, Vector3 normal, const std::vector<Vector3> &ring) {
				for (int64_t j = 0; j < sides; ++j) {
					const double a0 = (yaw + j * angleStep) * std::numbers::pi / 180,
								 a1 = (yaw + (j + 1) * angleStep) * std::numbers::pi / 180;
					MeshVertex3D middle{center, normal, {.5, .5}},
						p0{ring[size_t(j)], normal, {.5 + std::cos(a0) * .5, .5 - std::sin(a0) * .5}},
						p1{ring[size_t(j) + 1], normal, {.5 + std::cos(a1) * .5, .5 - std::sin(a1) * .5}};
					if (caps) {
						auto &target = mesh.Parts[end + 1].Vertices;
						target.push_back(middle);
						target.push_back(end ? p1 : p0);
						target.push_back(end ? p0 : p1);
					}
					mesh.Edges.push_back({p0.Position, p1.Position});
				}
			};
		capVertices(0, origin, {-direction.X, -direction.Y, -direction.Z}, previous);
		for (int64_t i = 1; i < samples; ++i) {
			const Vector3 center = points[size_t(i)];
			direction = Unit(Difference(i < samples - 1 ? points[size_t(i) + 1] : center, origin));
			for (int64_t j = 0; j <= sides; ++j) {
				const auto offset = RingOffset(direction, yaw + j * angleStep);
				current[size_t(j)] = AtRing(center, offset, radii[size_t(i)]);
				if (!j) continue;
				const auto &next = loop && i == samples - 1 ? first : current;
				const std::array<Vector3, 4> p{
					previous[size_t(j) - 1], previous[size_t(j)], next[size_t(j) - 1], next[size_t(j)]
				};
				std::array<Vector3, 4> n;
				if (smooth)
					n = {
						Unit(Difference(p[0], origin)),
						Unit(Difference(p[1], origin)),
						Unit(Difference(p[2], center)),
						Unit(Difference(p[3], center))
					};
				else
					n.fill(Unit(offset));
				const double v0 = 1 - progress[size_t(i) - 1], v1 = 1 - progress[size_t(i)],
							 u0 = double(j - 1) / sides, u1 = double(j) / sides;
				const std::array<Vector2, 4> uv{
					{{v0 * uvScale.Y, 1 - u0 * uvScale.X},
					 {v0 * uvScale.Y, 1 - u1 * uvScale.X},
					 {v1 * uvScale.Y, 1 - u0 * uvScale.X},
					 {v1 * uvScale.Y, 1 - u1 * uvScale.X}}
				};
				const std::array<size_t, 6> order = invert ? std::array<size_t, 6>{0, 1, 2, 1, 3, 2}
														   : std::array<size_t, 6>{0, 2, 1, 1, 2, 3};
				for (size_t k : order)
					mesh.Parts[0].Vertices.push_back({p[k], n[k], uv[k]});
				for (auto [a, b] : {std::pair<size_t, size_t>{0, 1}, {1, 3}, {3, 2}, {2, 0}})
					mesh.Edges.push_back({p[a], p[b]});
			}
			previous = current;
			origin = center;
		}
		capVertices(1, origin, {-direction.X, -direction.Y, -direction.Z}, previous);
		if (!ValidMeshPayload(output))
			return context.Fail(Status::InvalidValue, "path extrusion produced nonfinite geometry", "mesh");
		context.SetValue("mesh", std::move(output));
		return context.FailureCode == Status::Ok;
	}
}
