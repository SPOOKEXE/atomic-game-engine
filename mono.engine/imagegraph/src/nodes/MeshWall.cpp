#include "../SourceMeshBuild.hpp"
#include "Path3D.hpp"
namespace engine::imagegraph::detail {
	bool SourceMeshWall(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.wall");
		const Value *input = context.Find("path");
		const auto *spatial = input ? std::get_if<PathValue3D>(input) : nullptr;
		const auto *planar = input ? std::get_if<Path2D>(input) : nullptr;
		if (!input || (spatial && !spatial->Data) ||
			(planar && planar->Anchors.empty() && !planar->SourceOperation && !context.IsLinked("path"))) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!spatial && !planar) return context.Fail(Status::TypeMismatch, "wall requires a path", "path");
		if (!ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "wall path is invalid", "path");
		const int64_t segments = context.Integer("segments", 16);
		const double scale = context.Scalar("path_scale", .01), height = context.Scalar("height", 1),
					 thickness = context.Scalar("thickness", .1);
		const bool loop = context.Boolean("loop"), flip = context.Boolean("flip_y"),
				   separate = context.Boolean("material_per_side");
		if (context.FailureCode != Status::Ok) return false;
		if (segments < 1 || segments > int64_t(Limits::MaximumArrayElements))
			return context.Fail(Status::LimitExceeded, "wall segment count exceeds caps", "segments");
		if (!std::isfinite(scale) || !std::isfinite(height) || !std::isfinite(thickness))
			return context.Fail(Status::InvalidValue, "wall controls must be finite", "height");
		// The source offsets only the first point of each sampled segment, so its final sampled endpoint is
		// unused.
		const size_t count = size_t(segments) - 1 + size_t(loop), vertices = count * 18 + (loop ? 0 : 12),
					 edges = count ? count * 6 + 2 + (loop ? 0 : 4) : size_t(loop ? 0 : 4);
		if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "wall exceeds geometry caps", "segments");
		MeshTransform3D transform;
		SourceBuildMaterial side, side2, cap;
		if (!ReadSourceBuildTransform(context, transform) ||
			!ReadSourceBuildMaterial(context, "side_material", side) ||
			!ReadSourceBuildMaterial(context, "side_material_2", side2) ||
			!ReadSourceBuildMaterial(context, "cap_material", cap))
			return false;
		const uint64_t bytes = sizeof(MeshData3D) + 4 * (sizeof(MeshPart3D) + sizeof(MaterialValue3D)) +
							   vertices * sizeof(MeshVertex3D) + edges * sizeof(MeshEdge3D) +
							   sizeof(MeshTransform3D) + side.Bytes() +
							   (separate ? side2.Bytes() : side.Bytes()) + 2 * cap.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "wall exceeds payload caps", "mesh");
		auto scratch = context.ReserveWorkspace((size_t(segments) * 2 + 3) * sizeof(Vector2), "path");
		if (!scratch) return false;
		std::optional<PathRuntime> flat;
		std::optional<PathRuntime3D> space;
		if (spatial) {
			space.emplace(*spatial->Data, &context);
			if (!space->Valid())
				return context.Fail(Status::InvalidValue, "wall spatial path is invalid", "path");
		} else {
			flat.emplace();
			if (!flat->Init(context, *planar)) return false;
		}
		auto sample = [&](double ratio) {
			Vector3 p;
			if (space)
				p = space->Ratio(ratio).Position;
			else {
				auto q = flat->PointRatio(ratio);
				p = {q.X, q.Y, 0};
			}
			return Vector2{p.X * scale, p.Y * scale * (flip ? -1 : 1)};
		};
		std::vector<Vector2> left, right;
		left.reserve(size_t(segments) + size_t(loop));
		right.reserve(size_t(segments) + size_t(loop));
		Vector2 p0 = sample(0);
		for (int64_t i = 0; i < segments; ++i) {
			auto p1 = sample(double(i + 1) / segments);
			const double dx = p1.X - p0.X, dy = p1.Y - p0.Y, length = std::hypot(dx, dy);
			if (!MeshFinite(p0) || !MeshFinite(p1) || !std::isfinite(length) || length == 0)
				return context.Fail(
					Status::InvalidValue, "wall offset requires nonzero finite sampled segments", "path"
				);
			const Vector2 offset{-dy / length * thickness, dx / length * thickness};
			left.push_back({p0.X + offset.X, p0.Y + offset.Y});
			right.push_back({p0.X - offset.X, p0.Y - offset.Y});
			p0 = p1;
		}
		if (loop) {
			left.push_back(left.front());
			right.push_back(right.front());
		}
		std::reverse(right.begin(), right.end());
		MeshValue3D result;
		auto &data = result.Data.emplace();
		data.LocalTransforms.push_back(transform);
		data.Materials = {side.Clone(), separate ? side2.Clone() : side.Clone(), cap.Clone(), cap.Clone()};
		data.Parts.resize(4);
		data.Edges.reserve(edges);
		for (size_t part = 0; part < 4; ++part) {
			data.Parts[part].MaterialIndex = uint32_t(part);
			data.Parts[part].Vertices.reserve(
				part < 2 ? count * 6 : (part == 2 ? (loop ? 0 : 12) : count * 6)
			);
		}
		auto vertex = [](Vector2 p, double z, Vector2 uv, bool top = false) {
			return MeshVertex3D{{p.X, p.Y, z}, top ? Vector3{0, 0, 1} : Vector3{-p.Y, p.X, 0}, uv};
		};
		auto edge = [&](Vector2 a, double az, Vector2 b, double bz) {
			data.Edges.push_back({{a.X, a.Y, az}, {b.X, b.Y, bz}});
		};
		for (size_t sideIndex = 0; sideIndex < 2; ++sideIndex) {
			const auto &points = sideIndex ? right : left;
			auto &out = data.Parts[sideIndex].Vertices;
			for (size_t i = 0; i < count; ++i) {
				auto a = points[i], b = points[i + 1];
				double u0 = double(i) / count, u1 = double(i + 1) / count;
				out.insert(
					out.end(),
					{vertex(a, 0, {u0, 0}),
					 vertex(b, 0, {u1, 0}),
					 vertex(b, height, {u1, 1}),
					 vertex(a, 0, {u0, 0}),
					 vertex(b, height, {u1, 1}),
					 vertex(a, height, {u0, 1})}
				);
				if (!i) edge(a, 0, a, height);
				edge(b, 0, b, height);
				edge(a, 0, b, 0);
				edge(a, height, b, height);
			}
		}
		if (!loop) {
			auto &out = data.Parts[2].Vertices;
			auto a = left[0], b = right[count];
			out.insert(
				out.end(),
				{vertex(a, 0, {0, 0}),
				 vertex(b, height, {1, 1}),
				 vertex(b, 0, {1, 0}),
				 vertex(a, 0, {0, 0}),
				 vertex(a, height, {0, 1}),
				 vertex(b, height, {1, 1})}
			);
			edge(a, 0, b, 0);
			edge(a, height, b, height);
			a = left[count];
			b = right[0];
			out.insert(
				out.end(),
				{vertex(a, 0, {0, 0}),
				 vertex(b, 0, {1, 0}),
				 vertex(b, height, {1, 1}),
				 vertex(a, 0, {0, 0}),
				 vertex(b, height, {1, 1}),
				 vertex(a, height, {0, 1})}
			);
			edge(a, 0, b, 0);
			edge(a, height, b, height);
		}
		for (size_t i = 0; i < count; ++i) {
			auto a = left[i], b = left[i + 1], c = right[count - 1 - i], d = right[count - i];
			double u0 = double(i) / count, u1 = double(i + 1) / count;
			auto &out = data.Parts[3].Vertices;
			out.insert(
				out.end(),
				{vertex(a, height, {u0, 0}, true),
				 vertex(b, height, {u1, 0}, true),
				 vertex(d, height, {u1, 1}, true),
				 vertex(b, height, {u1, 0}, true),
				 vertex(c, height, {u0, 1}, true),
				 vertex(d, height, {u1, 1}, true)}
			);
		}
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "wall geometry is nonfinite", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
