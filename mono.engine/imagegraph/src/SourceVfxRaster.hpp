#pragma once
#include "NodeExecutors.hpp"
#include "SourceFlipSprite.hpp"
#include "nodes/Sampler.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	struct VfxVertex {
		Vector2 Position, Uv;
	};
	inline double VfxCross(Vector2 a, Vector2 b, Vector2 c) {
		return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
	}
	inline bool VfxTopLeft(Vector2 a, Vector2 b) {
		return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
	}
	inline bool BlendSourceVfxPixel(Image &image, uint32_t x, uint32_t y, Rgba source, int64_t mode) {
		const auto dest = ReadPixel(image, x, y);
		Rgba out{};
		for (size_t channel = 0; channel < 4; ++channel) {
			if (mode == 0)
				out[channel] = source[channel] * source[3] + dest[channel] * (1 - source[3]);
			else if (mode == 1)
				out[channel] = channel == 3 ? source[channel] + dest[channel]
											: source[channel] + dest[channel] * (1 - source[3]);
			else
				out[channel] = source[channel] * source[3] + dest[channel];
		}
		return StoreSurfacePixel(image, x, y, out);
	}
	inline bool RasterSourceVfxTriangle(
		NodeContext &context,
		Image &image,
		const Image &sprite,
		VfxVertex a,
		VfxVertex b,
		VfxVertex c,
		uint32_t blend,
		double alpha,
		int64_t mode,
		bool filtered,
		uint64_t &work
	) {
		double area = VfxCross(a.Position, b.Position, c.Position);
		if (!std::isfinite(area))
			return context.Fail(Status::InvalidValue, "VFX sprite triangle is undefined");
		if (area == 0) return true;
		if (area < 0) {
			std::swap(b, c);
			area = -area;
		}
		const auto low = [](double v, uint32_t max) {
			return uint32_t(std::clamp(std::ceil(v - .5), 0., double(max)));
		};
		const auto x0 = low(std::min({a.Position.X, b.Position.X, c.Position.X}), image.Width),
				   x1 = low(std::max({a.Position.X, b.Position.X, c.Position.X}), image.Width);
		const auto y0 = low(std::min({a.Position.Y, b.Position.Y, c.Position.Y}), image.Height),
				   y1 = low(std::max({a.Position.Y, b.Position.Y, c.Position.Y}), image.Height);
		const uint64_t samples = uint64_t(x1 - x0) * (y1 - y0);
		if (samples > 16777216 - work)
			return context.Fail(Status::LimitExceeded, "VFX sprite raster exceeds bounded work");
		work += samples;
		for (uint32_t y = y0; y < y1; ++y)
			for (uint32_t x = x0; x < x1; ++x) {
				const Vector2 p{double(x) + .5, double(y) + .5};
				const auto ca = VfxCross(b.Position, c.Position, p), cb = VfxCross(c.Position, a.Position, p),
						   cc = VfxCross(a.Position, b.Position, p);
				if (ca < 0 || (ca == 0 && !VfxTopLeft(b.Position, c.Position)) || cb < 0 ||
					(cb == 0 && !VfxTopLeft(c.Position, a.Position)) || cc < 0 ||
					(cc == 0 && !VfxTopLeft(a.Position, b.Position)))
					continue;
				const double u = (ca * a.Uv.X + cb * b.Uv.X + cc * c.Uv.X) / area,
							 v = (ca * a.Uv.Y + cb * b.Uv.Y + cc * c.Uv.Y) / area;
				auto pixel = Texture(sprite, u, v, filtered);
				pixel[0] *= (blend & 255) / 255.;
				pixel[1] *= ((blend >> 8) & 255) / 255.;
				pixel[2] *= ((blend >> 16) & 255) / 255.;
				pixel[3] *= alpha;
				if (!BlendSourceVfxPixel(image, x, y, pixel, mode))
					return context.Fail(Status::InvalidValue, "VFX sprite blend is nonfinite");
			}
		return true;
	}
	inline bool DrawSourceVfxBaseParticle(
		NodeContext &context,
		Image &image,
		const ParticleData2D &data,
		int64_t mode,
		bool filtered,
		uint64_t &work,
		bool drawInactive = false
	) {
		const auto &part = data.State;
		if (!part.Active && !drawInactive) return true;
		if (!part.SpriteSlot) {
			const double px = double(float(part.Position[0])), py = double(float(part.Position[1] - .01));
			if (!std::isfinite(px) || !std::isfinite(py))
				return context.Fail(Status::InvalidValue, "VFX float32 point geometry is undefined");
			if (px < 0 || py < 0 || px >= image.Width || py >= image.Height) return true;
			if (++work > 16777216)
				return context.Fail(Status::LimitExceeded, "VFX point raster exceeds bounded work");
			if (!BlendSourceVfxPixel(
					image,
					uint32_t(px),
					uint32_t(py),
					{(part.Blend & 255) / 255.,
					 ((part.Blend >> 8) & 255) / 255.,
					 ((part.Blend >> 16) & 255) / 255.,
					 1},
					mode
				))
				return context.Fail(Status::InvalidValue, "VFX point blend is nonfinite");
			return true;
		}
		const auto &sprite = data.Sprites[*part.SpriteSlot];
		const double width = sprite.Width * part.Scale[0], height = sprite.Height * part.Scale[1];
		const double angle = part.RotationDegrees * std::numbers::pi / 180, co = std::cos(angle),
					 si = std::sin(angle);
		const Vector2 u{width * co, -width * si}, v{height * si, height * co},
			origin{part.Position[0] - (u.X + v.X) / 2, part.Position[1] - (u.Y + v.Y) / 2};
		const auto vertex = [&](double x, double y, Vector2 uv) {
			return VfxVertex{{double(float(x)), double(float(y))}, uv};
		};
		const auto a = vertex(origin.X, origin.Y, {0, 0}), b = vertex(origin.X + u.X, origin.Y + u.Y, {1, 0}),
				   c = vertex(origin.X + u.X + v.X, origin.Y + u.Y + v.Y, {1, 1}),
				   d = vertex(origin.X + v.X, origin.Y + v.Y, {0, 1});
		const double alpha = SourceFlipSpriteVertexAlpha(part.Alpha);
		return RasterSourceVfxTriangle(
				   context, image, sprite, a, b, c, part.Blend, alpha, mode, filtered, work
			   ) &&
			   RasterSourceVfxTriangle(
				   context, image, sprite, c, d, a, part.Blend, alpha, mode, filtered, work
			   );
	}
}
