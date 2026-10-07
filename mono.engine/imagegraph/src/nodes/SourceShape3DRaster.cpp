#include "SourceShape3DRaster.hpp"

#include "../MeshPayload.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace engine::imagegraph::detail {
	namespace source_shape3d_raster {
		constexpr uint64_t MAXIMUM_RASTER_WORK = 64000000;
		struct Bounds {
			uint32_t Left, Top, Right, Bottom;
		};
		Bounds TriangleBounds(const SourceShape3DRasterTriangle &triangle, uint32_t width, uint32_t height) {
			const auto &vertices = triangle.Vertices;
			const auto bound = [](double value, uint32_t extent) {
				return uint32_t(std::clamp(std::ceil(value - .5), 0., double(extent)));
			};
			return {
				bound(std::min({vertices[0].Screen.X, vertices[1].Screen.X, vertices[2].Screen.X}), width),
				bound(std::min({vertices[0].Screen.Y, vertices[1].Screen.Y, vertices[2].Screen.Y}), height),
				bound(std::max({vertices[0].Screen.X, vertices[1].Screen.X, vertices[2].Screen.X}), width),
				bound(std::max({vertices[0].Screen.Y, vertices[1].Screen.Y, vertices[2].Screen.Y}), height)
			};
		}
		double Edge(Vector2 from, Vector2 to, Vector2 point) {
			return (to.X - from.X) * (point.Y - from.Y) - (to.Y - from.Y) * (point.X - from.X);
		}
		bool Covered(double edge, Vector2 from, Vector2 to) {
			return edge > 0 || (edge == 0 && (to.Y < from.Y || (to.Y == from.Y && to.X > from.X)));
		}
		Rgba ColourChannels(Colour colour) {
			return {colour.Red / 255., colour.Green / 255., colour.Blue / 255., colour.Alpha / 255.};
		}
	}

	using namespace source_shape3d_raster;

	bool RasterSourceShape3D(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		std::span<const SourceShape3DRasterTriangle> triangles,
		std::span<const Image *const> textures,
		const Image *background,
		SurfaceFormat format,
		SourceShape3DRasterResult &result
	) try {
		ENGINE_PROFILE("imagegraph.shape3d.raster");
		if (!recipe.Width || !recipe.Height || recipe.Width > context.Request.MaximumImageDimension ||
			recipe.Height > context.Request.MaximumImageDimension ||
			recipe.Width > Limits::MaximumDimension || recipe.Height > Limits::MaximumDimension ||
			triangles.size() > Limits::MaximumArrayElements / 3 ||
			textures.size() > Limits::MaximumArrayElements || recipe.Colours.empty() ||
			recipe.Colours.size() > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "Draw Shape 3D raster inputs exceed bounds");
		if (!MeshFinite(recipe.UVPosition) || !MeshFinite(recipe.UVScale) || !MeshFinite(recipe.ViewRange) ||
			recipe.ViewRange.X == recipe.ViewRange.Y)
			return context.Fail(Status::InvalidValue, "Draw Shape 3D raster uniforms are invalid");
		SamplerSettings sampler;
		sampler.Interpolation =
			recipe.Interpolation == 0 ? context.InheritedInterpolation : recipe.Interpolation;
		sampler.Oversample = recipe.Oversample == 0 ? context.InheritedOversample : recipe.Oversample;
		if (sampler.Interpolation < 1 || sampler.Interpolation > 4 || sampler.Oversample < 1 ||
			sampler.Oversample > 12 || sampler.Oversample == 5 || sampler.Oversample == 9)
			return context.Fail(Status::UnsupportedExecution, "Draw Shape 3D raster sampling is unresolved");
		const auto layout =
			CheckedSurfaceLayout(recipe.Width, recipe.Height, format, Limits::MaximumOutputBytes / 3);
		if (!layout) return context.Fail(Status::LimitExceeded, "Draw Shape 3D attachments exceed bounds");
		const uint64_t pixels = uint64_t(recipe.Width) * recipe.Height;
		const uint64_t sampleWork = sampler.Interpolation == 4 ? 144 : sampler.Interpolation == 1 ? 1 : 4;
		const uint64_t batchLimit =
			MAXIMUM_RASTER_WORK / std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		uint64_t work = pixels * (background ? 8 : 3) + recipe.Colours.size() + triangles.size() * 3;
		if (work > batchLimit)
			return context.Fail(Status::LimitExceeded, "Draw Shape 3D raster work exceeds bounds");
		// Repeated borrowed entries still incur repeated sample validation and must be admitted individually.
		const auto admitImage = [&](const Image *image) {
			if (!image || !ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
				return context.Fail(
					Status::InvalidValue, "Draw Shape 3D raster input image layout is invalid"
				);
			const uint64_t validationWork = uint64_t(image->Width) * image->Height * 4;
			if (validationWork > batchLimit - work)
				return context.Fail(
					Status::LimitExceeded, "Draw Shape 3D image validation exceeds whole batch work budget"
				);
			work += validationWork;
			return true;
		};
		for (const Image *texture : textures)
			if (!admitImage(texture)) return false;
		if (background && !admitImage(background)) return false;
		// Scan samples only after every input validation cost is admitted.
		for (const Image *texture : textures)
			if (!FiniteSurfaceSamples(*texture))
				return context.Fail(Status::InvalidValue, "Draw Shape 3D raster texture samples are invalid");
		if (background && !FiniteSurfaceSamples(*background))
			return context.Fail(Status::InvalidValue, "Draw Shape 3D raster background samples are invalid");
		for (const auto &colour : recipe.Colours)
			if (!std::holds_alternative<Colour>(colour))
				return context.Fail(
					Status::TypeMismatch, "Draw Shape 3D raster palette contains a non-colour"
				);
		for (const auto &triangle : triangles) {
			for (const auto &vertex : triangle.Vertices) {
				const Vector2 sampleUV{
					vertex.UV.X * recipe.UVScale.X + recipe.UVPosition.X,
					vertex.UV.Y * recipe.UVScale.Y + recipe.UVPosition.Y
				};
				if (!MeshFinite(sampleUV) || std::abs(sampleUV.X) > 0x1p30 || std::abs(sampleUV.Y) > 0x1p30)
					return context.Fail(
						Status::LimitExceeded, "Draw Shape 3D raster UV exceeds bounded sampling"
					);
				const double normalLength =
					std::hypot(vertex.ViewNormal.X, vertex.ViewNormal.Y, vertex.ViewNormal.Z);
				if (!MeshFinite(vertex.Screen) || !MeshFinite(vertex.UV) || !MeshFinite(vertex.ViewNormal) ||
					!std::isfinite(vertex.ClipDepth) || !std::isfinite(vertex.TestDepth) ||
					std::abs(vertex.Screen.X) > Limits::MaximumDimension * 4. ||
					std::abs(vertex.Screen.Y) > Limits::MaximumDimension * 4. ||
					std::abs(normalLength - 1.) > 1e-6)
					return context.Fail(
						Status::InvalidValue,
						"Draw Shape 3D raster requires finite projected vertices and unit normals"
					);
			}
			const auto &vertices = triangle.Vertices;
			const double area = Edge(vertices[0].Screen, vertices[1].Screen, vertices[2].Screen);
			if (area <= 0) continue;
			const auto bounds = TriangleBounds(triangle, recipe.Width, recipe.Height);
			const uint64_t visits = uint64_t(bounds.Right - bounds.Left) * (bounds.Bottom - bounds.Top);
			if (visits > (batchLimit - work) / (sampleWork + 16))
				return context.Fail(
					Status::LimitExceeded, "Draw Shape 3D whole batch triangle work exceeds bounds"
				);
			work += visits * (sampleWork + 16);
		}
		// Admission includes all three attachments and the live depth scratch before any allocation.
		auto charge = context.ReserveWorkspace(layout->Bytes * 3 + pixels * sizeof(double), "surface_out");
		if (!charge) return false;
		SourceShape3DRasterResult candidate;
		candidate.Charge = std::move(*charge);
		for (auto &image : candidate.Images) {
			image.Width = recipe.Width;
			image.Height = recipe.Height;
			image.Format = format;
			image.Pixels.resize(layout->Bytes);
		}
		std::vector<double> depths(size_t(pixels), 1.);
		for (const auto &triangle : triangles) {
			const auto &vertices = triangle.Vertices;
			const double area = Edge(vertices[0].Screen, vertices[1].Screen, vertices[2].Screen);
			if (area <= 0) continue;
			const Bounds bounds = TriangleBounds(triangle, recipe.Width, recipe.Height);
			const Rgba colour =
				ColourChannels(std::get<Colour>(recipe.Colours[triangle.Submesh % recipe.Colours.size()]));
			const Image *texture = textures.empty() ? nullptr : textures[triangle.Submesh % textures.size()];
			for (uint32_t y = bounds.Top; y < bounds.Bottom; ++y)
				for (uint32_t x = bounds.Left; x < bounds.Right; ++x) {
					const Vector2 point{x + .5, y + .5};
					std::array<double, 3> weights;
					bool inside = true;
					for (size_t index = 0; index < 3; ++index) {
						const Vector2 from = vertices[(index + 1) % 3].Screen,
									  to = vertices[(index + 2) % 3].Screen;
						const double edge = Edge(from, to, point);
						inside = inside && Covered(edge, from, to);
						weights[index] = edge / area;
					}
					if (!inside) continue;
					double testDepth = 0, clipDepth = 0, normalZ = 0;
					Vector2 uv;
					Rgba tint{};
					for (size_t index = 0; index < 3; ++index) {
						const auto &vertex = vertices[index];
						const double weight = weights[index];
						testDepth += weight * vertex.TestDepth;
						clipDepth += weight * vertex.ClipDepth;
						normalZ += weight * vertex.ViewNormal.Z;
						uv.X += weight * vertex.UV.X;
						uv.Y += weight * vertex.UV.Y;
						const Rgba vertexColour = ColourChannels(vertex.Tint);
						for (size_t channel = 0; channel < 4; ++channel)
							tint[channel] += weight * vertexColour[channel];
					}
					const size_t pixel = size_t(y) * recipe.Width + x;
					if (testDepth < 0 || testDepth > 1 || testDepth > depths[pixel]) continue;
					uv = {
						uv.X * recipe.UVScale.X + recipe.UVPosition.X,
						uv.Y * recipe.UVScale.Y + recipe.UVPosition.Y
					};
					if (!MeshFinite(uv))
						return context.Fail(Status::InvalidValue, "Draw Shape 3D raster UV overflow");
					const Rgba sampled =
						texture ? SampleTexture(*texture, uv.X, uv.Y, sampler) : Rgba{1, 1, 1, 1};
					SourceShape3DFragment fragment;
					if (!ShadeSourceShape3DFragment(
							recipe, sampled, colour, tint, clipDepth, normalZ, fragment
						) ||
						!WritePixel(candidate.Images[0], x, y, fragment.Surface) ||
						!WritePixel(candidate.Images[1], x, y, fragment.Depth) ||
						!WritePixel(candidate.Images[2], x, y, fragment.RimNormal))
						return context.Fail(
							Status::InvalidValue, "Draw Shape 3D raster fragment exceeds numeric range"
						);
					depths[pixel] = testDepth;
				}
		}
		if (background)
			for (uint32_t y = 0; y < recipe.Height; ++y)
				for (uint32_t x = 0; x < recipe.Width; ++x) {
					Rgba front = ReadPixel(candidate.Images[0], x, y);
					const Rgba back =
						SampleNearest(*background, (x + .5) / recipe.Width, (y + .5) / recipe.Height);
					const double alpha = front[3] + back[3] * (1 - front[3]);
					for (size_t channel = 0; channel < 3; ++channel)
						front[channel] =
							alpha == 0
								? 0
								: (front[channel] * front[3] + back[channel] * back[3] * (1 - front[3])) /
									  alpha;
					front[3] = alpha;
					if (!WritePixel(candidate.Images[0], x, y, front))
						return context.Fail(
							Status::InvalidValue, "Draw Shape 3D background exceeds numeric range"
						);
				}
		std::vector<double>{}.swap(depths);
		if (!candidate.Charge.Resize(layout->Bytes * 3)) std::terminate();
		for (auto &image : candidate.Images)
			image.Hash = SurfaceHash(image);
		// Free prior buffers before their charge, including when replacing an earlier successful result.
		result.Images = std::move(candidate.Images);
		result.Charge = std::move(candidate.Charge);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Draw Shape 3D raster allocation failed");
	} catch (const std::length_error &) {
		return context.Fail(Status::LimitExceeded, "Draw Shape 3D raster allocation length exceeds bounds");
	}
}