#include "../SourceMarchTable.hpp"
#include "../SourceMeshBuild.hpp"

#include <engine/imagegraph/SourceSdf.hpp>
namespace engine::imagegraph::detail {
	bool SourceMeshFromSdf(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.from_sdf");
		const Value *input = context.Find("sdf_object");
		const auto *sdf = input ? std::get_if<SdfValue>(input) : nullptr;
		if (!sdf || !sdf->Data) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!ValidateSourceSdfValue(*sdf))
			return context.Fail(
				Status::InvalidValue, "voxel conversion requires a valid source SDF", "sdf_object"
			);
		const double resolution = context.Scalar("resolution", 8);
		const Vector3 midpoint = context.Get<Vector3>("midpoint", {}),
					  span = context.Get<Vector3>("span", {1, 1, 1});
		if (context.FailureCode != Status::Ok) return false;
		if (!std::isfinite(resolution) || resolution < 2 || std::trunc(resolution) != resolution ||
			!MeshFinite(midpoint) || !MeshFinite(span))
			return context.Fail(
				Status::InvalidValue,
				"SDF conversion needs finite controls and an integer resolution at least two",
				"resolution"
			);
		if (resolution > 16)
			return context.Fail(
				Status::LimitExceeded, "SDF occupancy exceeds array sample cap", "resolution"
			);
		const int64_t rs = int64_t(resolution), r2 = rs * rs;
		const size_t cells = size_t(rs * rs * rs);
		for (double value : {midpoint.X, midpoint.Y, midpoint.Z, span.X, span.Y, span.Z})
			if (std::abs(value) > std::numeric_limits<float>::max())
				return context.Fail(
					Status::InvalidValue, "SDF conversion controls exceed f32 range", "midpoint"
				);
		auto scratch = context.ReserveWorkspace(cells, "resolution");
		if (!scratch) return false;
		std::vector<uint8_t> occupancy(cells);
		const float r = float(rs);
		auto coordinate = [&](double middle, double extent, int64_t index) {
			const float m = float(middle), s = float(extent), start = m - s * (r - 1) / r,
						end = m + s * (r - 1) / r, t = float(index) / (r - 1);
			return double(start * (1 - t) + end * t);
		};
		for (int64_t index = 0; index < int64_t(cells); ++index) {
			const int64_t x = index % rs, y = (index / rs) % rs, z = index / r2;
			const Vector3 point{
				coordinate(midpoint.X, span.X, x),
				coordinate(midpoint.Y, span.Y, y),
				coordinate(midpoint.Z, span.Z, z)
			};
			double distance;
			Diagnostic diagnostic;
			if (SampleSourceSdf(*sdf, point, distance, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, "sdf_object");
			occupancy[size_t(index)] = uint8_t(distance < 0);
		}
		const bool voxel = context.Authored.Type == "pc.rm_to_voxel";
		SourceBuildMaterial material;
		if (!ReadSourceBuildMaterial(context, "material", material)) return false;
		size_t count = 0;
		MeshPart3D *output = nullptr;
		auto emit = [&](Vector3 p, Vector3 n, Vector2 uv) {
			++count;
			if (output)
				output->Vertices.push_back(
					{{float(p.X), float(p.Y), float(p.Z)},
					 {float(n.X), float(n.Y), float(n.Z)},
					 {float(uv.X), float(uv.Y)}}
				);
		};
		const double size = 1.0 / rs, ss = size / 2, cs = double(rs) / 2;
		auto build = [&]() {
			for (int64_t i = 0; i < rs; ++i)
				for (int64_t j = 0; j < rs; ++j)
					for (int64_t k = 0; k < rs; ++k) {
						if (voxel) {
							if (!occupancy[size_t(i * r2 + j * rs + k)]) continue;
							const double cx = -(k - cs) * size - ss, cy = (i - cs) * size + ss,
										 cz = -(j - cs) * size - ss;
							emit({cx - ss, cy - ss, cz + ss}, {0, 0, 1}, {0, 0});
							emit({cx + ss, cy + ss, cz + ss}, {0, 0, 1}, {1, 1});
							emit({cx + ss, cy - ss, cz + ss}, {0, 0, 1}, {1, 0});
							emit({cx + ss, cy + ss, cz + ss}, {0, 0, 1}, {1, 1});
							emit({cx - ss, cy - ss, cz + ss}, {0, 0, 1}, {0, 0});
							emit({cx - ss, cy + ss, cz + ss}, {0, 0, 1}, {0, 1});
							emit({cx + ss, cy + ss, cz - ss}, {0, 0, -1}, {1, 1});
							emit({cx - ss, cy - ss, cz - ss}, {0, 0, -1}, {0, 0});
							emit({cx + ss, cy - ss, cz - ss}, {0, 0, -1}, {1, 0});
							emit({cx - ss, cy - ss, cz - ss}, {0, 0, -1}, {0, 0});
							emit({cx + ss, cy + ss, cz - ss}, {0, 0, -1}, {1, 1});
							emit({cx - ss, cy + ss, cz - ss}, {0, 0, -1}, {0, 1});
							emit({cx + ss, cy + ss, cz + ss}, {0, 1, 0}, {1, 1});
							emit({cx - ss, cy + ss, cz - ss}, {0, 1, 0}, {0, 0});
							emit({cx + ss, cy + ss, cz - ss}, {0, 1, 0}, {1, 0});
							emit({cx - ss, cy + ss, cz - ss}, {0, 1, 0}, {0, 0});
							emit({cx + ss, cy + ss, cz + ss}, {0, 1, 0}, {1, 1});
							emit({cx - ss, cy + ss, cz + ss}, {0, 1, 0}, {0, 1});
							emit({cx - ss, cy - ss, cz - ss}, {0, -1, 0}, {0, 0});
							emit({cx + ss, cy - ss, cz + ss}, {0, -1, 0}, {1, 1});
							emit({cx + ss, cy - ss, cz - ss}, {0, -1, 0}, {1, 0});
							emit({cx + ss, cy - ss, cz + ss}, {0, -1, 0}, {1, 1});
							emit({cx - ss, cy - ss, cz - ss}, {0, -1, 0}, {0, 0});
							emit({cx - ss, cy - ss, cz + ss}, {0, -1, 0}, {0, 1});
							emit({cx + ss, cy - ss, cz - ss}, {1, 0, 0}, {0, 0});
							emit({cx + ss, cy + ss, cz + ss}, {1, 0, 0}, {1, 1});
							emit({cx + ss, cy + ss, cz - ss}, {1, 0, 0}, {1, 0});
							emit({cx + ss, cy + ss, cz + ss}, {1, 0, 0}, {1, 1});
							emit({cx + ss, cy - ss, cz - ss}, {1, 0, 0}, {0, 0});
							emit({cx + ss, cy - ss, cz + ss}, {1, 0, 0}, {0, 1});
							emit({cx - ss, cy + ss, cz + ss}, {-1, 0, 0}, {1, 1});
							emit({cx - ss, cy - ss, cz - ss}, {-1, 0, 0}, {0, 0});
							emit({cx - ss, cy + ss, cz - ss}, {-1, 0, 0}, {1, 0});
							emit({cx - ss, cy - ss, cz - ss}, {-1, 0, 0}, {0, 0});
							emit({cx - ss, cy + ss, cz + ss}, {-1, 0, 0}, {1, 1});
							emit({cx - ss, cy - ss, cz + ss}, {-1, 0, 0}, {0, 1});
						} else {
							if (i >= rs - 1 || j >= rs - 1 || k >= rs - 1) continue;
							size_t mask = 0;
							for (size_t bit = 0; bit < 8; ++bit) {
								const int64_t a = i + int64_t(bit >> 2), b = j + int64_t((bit >> 1) & 1),
											  c = k + int64_t(bit & 1);
								mask |= size_t(occupancy[size_t(a * r2 + b * rs + c)]) << bit;
							}
							if (mask == 0 || mask == 255) continue;
							const double x0 = (k - cs) / rs + ss, x1 = (k + 1 - cs) / rs + ss,
										 y0 = (j - cs) / rs + ss, y1 = (j + 1 - cs) / rs + ss,
										 z0 = (i - cs) / rs + ss, z1 = (i + 1 - cs) / rs + ss;
							const std::array<Vector3, 12> edges{
								{{x0, y0, (z0 + z1) / 2},
								 {x1, y0, (z0 + z1) / 2},
								 {x0, y1, (z0 + z1) / 2},
								 {x1, y1, (z0 + z1) / 2},
								 {(x0 + x1) / 2, y0, z0},
								 {x0, (y0 + y1) / 2, z0},
								 {x1, (y0 + y1) / 2, z0},
								 {(x0 + x1) / 2, y1, z0},
								 {(x0 + x1) / 2, y0, z1},
								 {x0, (y0 + y1) / 2, z1},
								 {x1, (y0 + y1) / 2, z1},
								 {(x0 + x1) / 2, y1, z1}}
							};
							const bool inverse = mask >= 128;
							const auto &triangles = SOURCE_MARCH_CASES[inverse ? 255 - mask : mask];
							for (size_t t = 0; t < triangles.Count; t += 3) {
								auto get = [&](size_t offset) {
									return edges
										[triangles
											 .Edges[inverse ? t + offset : triangles.Count - 1 - t - offset]];
								};
								const auto a = get(0), b = get(1), c = get(2);
								const Vector3 u{b.X - a.X, b.Y - a.Y, b.Z - a.Z},
									v{c.X - a.X, c.Y - a.Y, c.Z - a.Z},
									normal{
										u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X
									};
								emit(a, normal, {0, 0});
								emit(b, normal, {1, 0});
								emit(c, normal, {0, 1});
							}
						}
					}
		};
		build();
		if (count > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "SDF conversion exceeds vertex cap", "resolution");
		const size_t admitted = count;
		const uint64_t bytes = sizeof(MeshData3D) + sizeof(MeshPart3D) + sizeof(MaterialValue3D) +
							   sizeof(MeshTransform3D) + count * sizeof(MeshVertex3D) + material.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "SDF conversion exceeds payload caps", "mesh");
		MeshValue3D result;
		auto &data = result.Data.emplace();
		data.LocalTransforms.emplace_back();
		data.Materials.push_back(material.Clone());
		data.Parts.emplace_back();
		data.Parts[0].Vertices.reserve(admitted);
		count = 0;
		output = &data.Parts[0];
		build();
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "SDF conversion geometry is nonfinite", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
