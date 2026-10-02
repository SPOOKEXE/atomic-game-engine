#include "../SourceMeshBuild.hpp"
namespace engine::imagegraph::detail {
	bool SourceMeshPlanarExtrude(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.planar_extrude");
		const Value *input = context.Find("mesh");
		const auto *source = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!source || !source->Data || source->Data->Triangles.empty()) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "extrusion mesh is invalid", "mesh");
		const auto &mesh = *source->Data;
		const auto &points = mesh.Simulation.Points;
		const double thickness = context.Scalar("thickness", 1), taper = context.Scalar("taper");
		const bool smooth = context.Boolean("smooth");
		if (context.FailureCode != Status::Ok) return false;
		if (!std::isfinite(thickness) || !std::isfinite(taper))
			return context.Fail(Status::InvalidValue, "extrusion controls must be finite", "thickness");
		const auto &bounds = mesh.Bounds;
		const double width = bounds[2] - bounds[0], height = bounds[3] - bounds[1];
		if (!std::isfinite(width) || !std::isfinite(height) || width == 0 || height == 0)
			return context.Fail(
				Status::InvalidValue, "extrusion bounds require nonzero finite extents", "mesh"
			);
		struct Segment {
			uint32_t A, B;
			size_t Count = 1;
		};
		const size_t maximumSegments = mesh.Triangles.size() * 3;
		if (maximumSegments > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "extrusion boundary exceeds caps", "mesh");
		auto scratch = context.ReserveWorkspace(
			maximumSegments * (sizeof(Segment) + 4 * sizeof(uint32_t) + sizeof(std::vector<uint32_t>)) +
				sizeof(uint32_t),
			"mesh"
		);
		if (!scratch) return false;
		std::vector<Segment> segments;
		segments.reserve(maximumSegments);
		auto same = [&](uint32_t a, uint32_t b) { return points[a].Position == points[b].Position; };
		for (const auto &triangle : mesh.Triangles)
			for (size_t i = 0; i < 3; ++i) {
				const auto a = triangle[i], b = triangle[(i + 1) % 3];
				auto found = std::find_if(segments.begin(), segments.end(), [&](const auto &s) {
					return (same(a, s.A) && same(b, s.B)) || (same(a, s.B) && same(b, s.A));
				});
				if (found == segments.end())
					segments.push_back({a, b});
				else {
					found->A = a;
					found->B = b;
					++found->Count;
				}
			}
		std::erase_if(segments, [](const auto &s) { return s.Count != 1; });
		std::vector<std::vector<uint32_t>> paths;
		paths.reserve(segments.size());
		size_t sideCount = 0;
		while (!segments.empty()) {
			std::vector<uint32_t> path;
			path.reserve(2);
			path.push_back(segments.front().A);
			path.push_back(segments.front().B);
			segments.erase(segments.begin());
			for (size_t i = 0; i < segments.size();) {
				if (same(segments[i].A, path.back())) {
					path.push_back(segments[i].B);
					segments.erase(segments.begin() + i);
					i = 0;
				} else if (same(segments[i].B, path.back())) {
					path.push_back(segments[i].A);
					segments.erase(segments.begin() + i);
					i = 0;
				} else
					++i;
			}
			sideCount += path.size();
			paths.push_back(std::move(path));
		}
		const size_t vertices = mesh.Triangles.size() * 6 + sideCount * 6,
					 edges = mesh.Triangles.size() * 6 + sideCount * 2;
		if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "extrusion exceeds geometry caps", "mesh");
		MeshTransform3D transform;
		SourceBuildMaterial face, back, side;
		if (!ReadSourceBuildTransform(context, transform) ||
			!ReadSourceBuildMaterial(context, "face_texture", face) ||
			!ReadSourceBuildMaterial(context, "back_texture", back) ||
			!ReadSourceBuildMaterial(context, "side_texture", side))
			return false;
		const uint64_t bytes = sizeof(MeshData3D) + 3 * (sizeof(MeshPart3D) + sizeof(MaterialValue3D)) +
							   sizeof(MeshTransform3D) + vertices * sizeof(MeshVertex3D) +
							   edges * sizeof(MeshEdge3D) + face.Bytes() + back.Bytes() + side.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "extrusion exceeds payload caps", "mesh");
		MeshValue3D result;
		auto &data = result.Data.emplace();
		data.LocalTransforms.push_back(transform);
		data.Materials = {face.Clone(), back.Clone(), side.Clone()};
		data.Parts.resize(3);
		data.Edges.reserve(edges);
		for (size_t i = 0; i < 3; ++i) {
			data.Parts[i].MaterialIndex = uint32_t(i);
			data.Parts[i].Vertices.reserve(i < 2 ? mesh.Triangles.size() * 3 : sideCount * 6);
		}
		const double z = thickness / 2, t = 1 - taper;
		auto uv = [&](uint32_t index) {
			auto p = points[index].Position;
			return Vector2{(p.X - bounds[0]) / width, (p.Y - bounds[1]) / height};
		};
		auto vertex = [&](Vector2 p, bool front, Vector3 normal, Vector2 tex) {
			return MeshVertex3D{
				{(p.X * 2 - 1) * (front ? t : 1), (p.Y * 2 - 1) * (front ? t : 1), front ? z : -z},
				normal,
				tex
			};
		};
		auto edge = [&](const MeshVertex3D &a, const MeshVertex3D &b) {
			data.Edges.push_back({a.Position, b.Position});
		};
		for (const auto &tri : mesh.Triangles) {
			std::array<Vector2, 3> p{uv(tri[0]), uv(tri[1]), uv(tri[2])};
			auto &front = data.Parts[0].Vertices;
			auto &rear = data.Parts[1].Vertices;
			for (size_t i = 0; i < 3; ++i)
				front.push_back(vertex(p[i], true, {0, 0, 1}, p[i]));
			for (size_t i : {size_t(0), size_t(2), size_t(1)})
				rear.push_back(vertex(p[i], false, {0, 0, -1}, p[i]));
			const size_t offset = front.size() - 3;
			for (size_t i = 0; i < 3; ++i)
				edge(front[offset + i], front[offset + (i + 1) % 3]);
			for (size_t i = 0; i < 3; ++i)
				edge(rear[offset + i], rear[offset + (i + 1) % 3]);
		}
		auto normal = [](Vector2 a, Vector2 b) {
			const double dx = b.X - a.X, dy = b.Y - a.Y, length = std::hypot(dx, dy);
			// Source closes boundaries with a duplicated anchor and divides its zero-edge normal by zero.
			// Retain the zero-area quad with a finite zero normal; visible triangles keep source normals.
			return length == 0 ? Vector3{} : Vector3{-dy / length, dx / length, 0};
		};
		for (const auto &path : paths) {
			const size_t n = path.size();
			for (size_t i = 0; i < n; ++i) {
				auto a = uv(path[i]), b = uv(path[(i + 1) % n]);
				auto n0 = smooth ? normal(uv(path[(i + n - 1) % n]), b) : normal(a, b);
				auto n1 = smooth ? normal(a, uv(path[(i + 2) % n])) : n0;
				const double u0 = double(i) / n, u1 = double(i + 1) / n;
				auto &out = data.Parts[2].Vertices;
				const size_t offset = out.size();
				out.insert(
					out.end(),
					{vertex(a, true, n0, {u0, 0}),
					 vertex(a, false, n0, {u0, 1}),
					 vertex(b, true, n1, {u1, 0}),
					 vertex(b, true, n1, {u1, 0}),
					 vertex(a, false, n0, {u0, 1}),
					 vertex(b, false, n1, {u1, 1})}
				);
				edge(out[offset], out[offset + 1]);
				edge(out[offset + 3], out[offset + 5]);
			}
		}
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "extrusion geometry is nonfinite", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
