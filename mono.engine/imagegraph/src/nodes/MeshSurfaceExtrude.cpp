#include "../SourceMeshBuild.hpp"
#include "Sampler.hpp"
namespace engine::imagegraph::detail {
	bool SourceMeshSurfaceExtrude(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.surface_extrude");
		SourceBuildMaterial front, backMaterial, faceTexture, backTexture, sideTexture;
		if (!ReadSourceBuildMaterial(context, "front_surface", front) ||
			!ReadSourceBuildMaterial(context, "back_surface", backMaterial) ||
			!ReadSourceBuildMaterial(context, "front_texture", faceTexture) ||
			!ReadSourceBuildMaterial(context, "back_texture", backTexture) ||
			!ReadSourceBuildMaterial(context, "side_texture", sideTexture))
			return false;
		auto surface = [](const SourceBuildMaterial &material) -> const Image * {
			return material.Surface ? material.Surface
									: (material.Material && material.Material->Get().Surface
										   ? &*material.Material->Get().Surface
										   : nullptr);
		};
		const Image *image = surface(front), *height = context.Input("front_height"),
					*backHeight = context.Input("back_height");
		if (!image) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		const bool useH = height != nullptr, back = context.Boolean("double_side"),
				   voxel = context.Boolean("voxel_scale");
		const double voxelScale = context.Scalar("voxel_size", .1);
		const Vector2 frontLevel = context.Get<Vector2>("front_height_level", {0, 1}),
					  backLevel = context.Get<Vector2>("back_height_level", {0, 1});
		if (context.FailureCode != Status::Ok) return false;
		if (!std::isfinite(voxelScale) || !MeshFinite(frontLevel) || !MeshFinite(backLevel) ||
			(useH && (frontLevel.X == frontLevel.Y || (back && backLevel.X == backLevel.Y))))
			return context.Fail(
				Status::InvalidValue,
				"surface extrusion levels and scale must be finite and defined",
				"front_height_level"
			);
		if (!ValidMaterialSurface(*image) || (height && !ValidMaterialSurface(*height)) ||
			(backHeight && !ValidMaterialSurface(*backHeight)))
			return context.Fail(Status::InvalidValue, "surface extrusion input is invalid", "front_surface");
		const int64_t ww = image->Width, hh = image->Height;
		const double asp = double(ww) / hh, tw = asp / ww, th = 1.0 / hh, sw = -asp / 2, sh = .5,
					 fw = 1.0 / ww, fh = 1.0 / hh;
		const double sx = voxel ? voxelScale * ww : 1, sy = voxel ? voxelScale * hh : 1,
					 sz = voxel ? voxelScale : 1;
		auto alpha = [&](int64_t index) {
			return index >= 0 && index < ww * hh &&
				   ReadPixel(*image, uint32_t(index % ww), uint32_t(index / ww))[3] > 0;
		};
		auto level = [&](const Image *map, Vector2 range, int64_t index) {
			const auto pixel =
				Texture(*map, (double(index % ww) + .5) / ww, (double(index / ww) + .5) / hh, false);
			const float r = float(pixel[0]), g = float(pixel[1]), b = float(pixel[2]), a = float(pixel[3]);
			const float bri = ((r + g) + b) / 3.f * a;
			return double(DecodeHalf(EncodeHalf((bri - float(range.X)) / (float(range.Y) - float(range.X)))));
		};
		auto h = [&](int64_t index) { return level(height, frontLevel, index); };
		auto hb = [&](int64_t index) { return level(backHeight ? backHeight : height, backLevel, index); };
		MeshTransform3D transform;
		if (!ReadSourceBuildTransform(context, transform)) return false;
		if (!surface(faceTexture)) faceTexture = front;
		if (!surface(backTexture)) backTexture = back ? backMaterial : front;
		if (!surface(sideTexture)) sideTexture = front;
		std::array<size_t, 3> counts{};
		size_t edgeCount = 0;
		bool finite = true;
		MeshData3D *output = nullptr;
		auto add =
			[&](
				size_t part, double x, double y, double z, double nx, double ny, double nz, double u, double v
			) {
				MeshVertex3D vertex{{x, y, z}, {nx, ny, nz}, {u, v}};
				finite = finite && MeshFinite(vertex.Position) && MeshFinite(vertex.Normal) &&
						 MeshFinite(vertex.UV);
				++counts[part];
				if (output) output->Parts[part].Vertices.push_back(vertex);
			};
		auto edge = [&](Vector3 a, Vector3 b) {
			++edgeCount;
			if (output) output->Edges.push_back({a, b});
		};
		auto build = [&]() {
			for (int64_t n = 0; n < ww * hh; ++n) {
				const int64_t i = n % ww;
				const int64_t j = n / ww;

				const bool _solid = alpha(n);
				if (_solid == 0) continue;

				double i0 = sw + i * tw;
				double j0 = sh - j * th;
				double i1 = i0 + tw;
				double j1 = j0 - th;

				double tx0 = i * fw;
				double tx1 = tx0 + fw;

				double ty0 = j * fh;
				double ty1 = ty0 + fh;

				double dep = useH ? h(((j * ww) + i)) * .5 : 0.5;
				double depb = useH && back ? hb(((j * ww) + i)) * .5 : dep;
				depb = -depb;

				i0 *= sx;
				i1 *= sx;
				j0 *= sy;
				j1 *= sy;
				dep *= sz;
				depb *= sz;

				double k0 = depb;
				double k1 = dep;

				// -Z
				add(1, i1, j0, k0, 0, 0, -1, tx1, ty0);
				add(1, i0, j0, k0, 0, 0, -1, tx0, ty0);
				add(1, i1, j1, k0, 0, 0, -1, tx1, ty1);

				add(1, i1, j1, k0, 0, 0, -1, tx1, ty1);
				add(1, i0, j0, k0, 0, 0, -1, tx0, ty0);
				add(1, i0, j1, k0, 0, 0, -1, tx0, ty1);

				// +Z
				add(0, i1, j0, k1, 0, 0, 1, tx1, ty0);
				add(0, i1, j1, k1, 0, 0, 1, tx1, ty1);
				add(0, i0, j0, k1, 0, 0, 1, tx0, ty0);

				add(0, i1, j1, k1, 0, 0, 1, tx1, ty1);
				add(0, i0, j1, k1, 0, 0, 1, tx0, ty1);
				add(0, i0, j0, k1, 0, 0, 1, tx0, ty0);

				edge({i0, j0, k0}, {i0, j1, k0});
				edge({i0, j1, k0}, {i1, j1, k0});
				edge({i1, j1, k0}, {i1, j0, k0});
				edge({i1, j0, k0}, {i0, j0, k0});

				edge({i0, j0, k1}, {i0, j1, k1});
				edge({i0, j1, k1}, {i1, j1, k1});
				edge({i1, j1, k1}, {i1, j0, k1});
				edge({i1, j0, k1}, {i0, j0, k1});

				edge({i0, j0, k0}, {i0, j0, k1});
				edge({i0, j1, k0}, {i0, j1, k1});
				edge({i1, j0, k0}, {i1, j0, k1});
				edge({i1, j1, k0}, {i1, j1, k1});

				if (voxel) {
					// -X
					add(2, i0, j0, k1, -1, 0, 0, tx1, ty0);
					add(2, i0, j1, k1, -1, 0, 0, tx1, ty1);
					add(2, i0, j0, k0, -1, 0, 0, tx0, ty0);

					add(2, i0, j0, k0, -1, 0, 0, tx0, ty0);
					add(2, i0, j1, k1, -1, 0, 0, tx1, ty1);
					add(2, i0, j1, k0, -1, 0, 0, tx0, ty1);

					// +X
					add(0, i1, j0, k1, 1, 0, 0, tx1, ty0);
					add(0, i1, j0, k0, 1, 0, 0, tx0, ty0);
					add(0, i1, j1, k1, 1, 0, 0, tx1, ty1);

					add(0, i1, j1, k0, 1, 0, 0, tx0, ty1);
					add(0, i1, j1, k1, 1, 0, 0, tx1, ty1);
					add(0, i1, j0, k0, 1, 0, 0, tx0, ty0);

					// -Y
					add(2, i0, j0, k1, 0, -1, 0, tx1, ty0);
					add(2, i0, j0, k0, 0, -1, 0, tx0, ty0);
					add(2, i1, j0, k1, 0, -1, 0, tx1, ty1);

					add(2, i1, j0, k1, 0, -1, 0, tx1, ty1);
					add(2, i0, j0, k0, 0, -1, 0, tx0, ty0);
					add(2, i1, j0, k0, 0, -1, 0, tx0, ty1);

					// +Y
					add(0, i0, j1, k1, 0, 1, 0, tx1, ty0);
					add(0, i1, j1, k1, 0, 1, 0, tx1, ty1);
					add(0, i0, j1, k0, 0, 1, 0, tx0, ty0);

					add(0, i1, j1, k1, 0, 1, 0, tx1, ty1);
					add(0, i1, j1, k0, 0, 1, 0, tx0, ty1);
					add(0, i0, j1, k0, 0, 1, 0, tx0, ty0);
					continue;
				}

				// Old "Accurate" height
				add(1, i1, j0, depb, 0, 0, -1, tx1, ty0);
				add(1, i0, j0, depb, 0, 0, -1, tx0, ty0);
				add(1, i1, j1, depb, 0, 0, -1, tx1, ty1);

				add(1, i1, j1, depb, 0, 0, -1, tx1, ty1);
				add(1, i0, j0, depb, 0, 0, -1, tx0, ty0);
				add(1, i0, j1, depb, 0, 0, -1, tx0, ty1);

				add(0, i1, j0, dep, 0, 0, 1, tx1, ty0);
				add(0, i1, j1, dep, 0, 0, 1, tx1, ty1);
				add(0, i0, j0, dep, 0, 0, 1, tx0, ty0);

				add(0, i1, j1, dep, 0, 0, 1, tx1, ty1);
				add(0, i0, j1, dep, 0, 0, 1, tx0, ty1);
				add(0, i0, j0, dep, 0, 0, 1, tx0, ty0);

				if (back) {
					if ((useH && dep * 2 > h((i + std::max<int64_t>(0, j - 1) * ww))) ||
						(j == 0 || alpha((j - 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j0, dep, 0, 1, 0, tx0, ty1);
						add(2, i0, j0, 0, 0, 1, 0, tx0, ty0);
						add(2, i1, j0, dep, 0, 1, 0, tx1, ty1);

						add(2, i0, j0, 0, 0, 1, 0, tx0, ty0);
						add(2, i1, j0, 0, 0, 1, 0, tx1, ty0);
						add(2, i1, j0, dep, 0, 1, 0, tx1, ty1);
					}

					if ((useH && std::abs(depb) * 2 > hb((i + std::max<int64_t>(0, j - 1) * ww))) ||
						(j == 0 || alpha((j - 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j0, 0, 0, 1, 0, tx0, ty0);
						add(2, i0, j0, depb, 0, 1, 0, tx0, ty1);
						add(2, i1, j0, 0, 0, 1, 0, tx1, ty0);

						add(2, i0, j0, depb, 0, 1, 0, tx0, ty1);
						add(2, i1, j0, depb, 0, 1, 0, tx1, ty1);
						add(2, i1, j0, 0, 0, 1, 0, tx1, ty0);
					}

					if ((useH && dep * 2 > h((i + std::min<int64_t>(j + 1, hh - 1) * ww))) ||
						(j == hh - 1 || alpha((j + 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j1, dep, 0, -1, 0, tx0, ty1);
						add(2, i1, j1, dep, 0, -1, 0, tx1, ty1);
						add(2, i0, j1, 0, 0, -1, 0, tx0, ty0);

						add(2, i0, j1, 0, 0, -1, 0, tx0, ty0);
						add(2, i1, j1, dep, 0, -1, 0, tx1, ty1);
						add(2, i1, j1, 0, 0, -1, 0, tx1, ty0);
					}

					if ((useH && std::abs(depb) * 2 > hb((i + std::min<int64_t>(j + 1, hh - 1) * ww))) ||
						(j == hh - 1 || alpha((j + 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j1, 0, 0, -1, 0, tx0, ty0);
						add(2, i1, j1, 0, 0, -1, 0, tx1, ty0);
						add(2, i0, j1, depb, 0, -1, 0, tx0, ty1);

						add(2, i0, j1, depb, 0, -1, 0, tx0, ty1);
						add(2, i1, j1, 0, 0, -1, 0, tx1, ty0);
						add(2, i1, j1, depb, 0, -1, 0, tx1, ty1);
					}

					if ((useH && dep * 2 > h((std::max<int64_t>(0, i - 1) + j * ww))) ||
						(i == 0 || alpha((j)*ww + (i - 1)) == 0)) { // x side

						add(2, i0, j0, dep, -1, 0, 0, tx0, ty1);
						add(2, i0, j1, dep, -1, 0, 0, tx1, ty1);
						add(2, i0, j0, 0, -1, 0, 0, tx0, ty0);

						add(2, i0, j0, 0, -1, 0, 0, tx0, ty0);
						add(2, i0, j1, dep, -1, 0, 0, tx1, ty1);
						add(2, i0, j1, 0, -1, 0, 0, tx1, ty0);
					}

					if ((useH && std::abs(depb) * 2 > hb((std::max<int64_t>(0, i - 1) + j * ww))) ||
						(i == 0 || alpha((j)*ww + (i - 1)) == 0)) { // x side

						add(2, i0, j0, 0, -1, 0, 0, tx0, ty0);
						add(2, i0, j1, 0, -1, 0, 0, tx1, ty0);
						add(2, i0, j0, depb, -1, 0, 0, tx0, ty1);

						add(2, i0, j0, depb, -1, 0, 0, tx0, ty1);
						add(2, i0, j1, 0, -1, 0, 0, tx1, ty0);
						add(2, i0, j1, depb, -1, 0, 0, tx1, ty0);
					}

					if ((useH && dep * 2 > h((std::min<int64_t>(i + 1, ww - 1) + j * ww))) ||
						(i == ww - 1 || alpha((j)*ww + (i + 1)) == 0)) { // x side

						add(2, i1, j0, dep, 1, 0, 0, tx0, ty1);
						add(2, i1, j0, 0, 1, 0, 0, tx0, ty0);
						add(2, i1, j1, dep, 1, 0, 0, tx1, ty1);

						add(2, i1, j0, 0, 1, 0, 0, tx0, ty0);
						add(2, i1, j1, 0, 1, 0, 0, tx1, ty0);
						add(2, i1, j1, dep, 1, 0, 0, tx1, ty1);
					}

					if ((useH && std::abs(depb) * 2 > hb((std::min<int64_t>(i + 1, ww - 1) + j * ww))) ||
						(i == ww - 1 || alpha((j)*ww + (i + 1)) == 0)) { // x side

						add(2, i1, j0, 0, 1, 0, 0, tx0, ty0);
						add(2, i1, j0, depb, 1, 0, 0, tx0, ty1);
						add(2, i1, j1, 0, 1, 0, 0, tx1, ty0);

						add(2, i1, j0, depb, 1, 0, 0, tx0, ty1);
						add(2, i1, j1, depb, 1, 0, 0, tx1, ty1);
						add(2, i1, j1, 0, 1, 0, 0, tx1, ty0);
					}

				} else {

					if ((useH && dep * 2 > h((i + std::max<int64_t>(0, j - 1) * ww))) ||
						(j == 0 || alpha((j - 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j0, dep, 0, 1, 0, tx0, ty1);
						add(2, i0, j0, depb, 0, 1, 0, tx0, ty0);
						add(2, i1, j0, dep, 0, 1, 0, tx1, ty1);

						add(2, i0, j0, depb, 0, 1, 0, tx0, ty0);
						add(2, i1, j0, depb, 0, 1, 0, tx1, ty0);
						add(2, i1, j0, dep, 0, 1, 0, tx1, ty1);
					}

					if ((useH && dep * 2 > h((i + std::min<int64_t>(j + 1, hh - 1) * ww))) ||
						(j == hh - 1 || alpha((j + 1) * ww + (i)) == 0)) { // y side

						add(2, i0, j1, dep, 0, -1, 0, tx0, ty1);
						add(2, i1, j1, dep, 0, -1, 0, tx1, ty1);
						add(2, i0, j1, depb, 0, -1, 0, tx0, ty0);

						add(2, i0, j1, depb, 0, -1, 0, tx0, ty0);
						add(2, i1, j1, dep, 0, -1, 0, tx1, ty1);
						add(2, i1, j1, depb, 0, -1, 0, tx1, ty0);
					}

					if ((useH && dep * 2 > h((std::max<int64_t>(0, i - 1) + j * ww))) ||
						(i == 0 || alpha((j)*ww + (i - 1)) == 0)) { // x side

						add(2, i0, j0, dep, -1, 0, 0, tx0, ty1);
						add(2, i0, j1, dep, -1, 0, 0, tx1, ty1);
						add(2, i0, j0, depb, -1, 0, 0, tx0, ty0);

						add(2, i0, j0, depb, -1, 0, 0, tx0, ty0);
						add(2, i0, j1, dep, -1, 0, 0, tx1, ty1);
						add(2, i0, j1, depb, -1, 0, 0, tx1, ty0);
					}

					if ((useH && dep * 2 > h((std::min<int64_t>(i + 1, ww - 1) + j * ww))) ||
						(i == ww - 1 || alpha((j)*ww + (i + 1)) == 0)) { // x side

						add(2, i1, j0, dep, 1, 0, 0, tx0, ty1);
						add(2, i1, j0, depb, 1, 0, 0, tx0, ty0);
						add(2, i1, j1, dep, 1, 0, 0, tx1, ty1);

						add(2, i1, j0, depb, 1, 0, 0, tx0, ty0);
						add(2, i1, j1, depb, 1, 0, 0, tx1, ty0);
						add(2, i1, j1, dep, 1, 0, 0, tx1, ty1);
					}
				}
			}
		};
		build();
		const auto admitted = counts;
		const size_t admittedEdges = edgeCount, vertices = counts[0] + counts[1] + counts[2];
		if (!finite)
			return context.Fail(
				Status::InvalidValue, "surface extrusion height exceeds finite f16 range", "front_height"
			);
		if (vertices > Limits::MaximumArrayElements || edgeCount > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "surface extrusion exceeds geometry caps", "front_surface"
			);
		const uint64_t bytes = sizeof(MeshData3D) + 3 * (sizeof(MeshPart3D) + sizeof(MaterialValue3D)) +
							   sizeof(MeshTransform3D) + vertices * sizeof(MeshVertex3D) +
							   edgeCount * sizeof(MeshEdge3D) + faceTexture.Bytes() + backTexture.Bytes() +
							   sideTexture.Bytes();
		if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 64, "mesh"))
			return context.Fail(Status::LimitExceeded, "surface extrusion exceeds payload caps", "mesh");
		MeshValue3D result;
		auto &data = result.Data.emplace();
		data.Materials = {faceTexture.Clone(), backTexture.Clone(), sideTexture.Clone()};
		data.LocalTransforms.push_back(transform);
		data.Parts.resize(3);
		data.Edges.reserve(admittedEdges);
		for (size_t i = 0; i < 3; ++i) {
			data.Parts[i].MaterialIndex = uint32_t(i);
			data.Parts[i].Vertices.reserve(admitted[i]);
		}
		counts = {};
		edgeCount = 0;
		output = &data;
		build();
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "surface extrusion output is invalid", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
