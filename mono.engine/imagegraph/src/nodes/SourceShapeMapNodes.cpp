#include "../SourceSafeDraw.hpp"
#include "Sampler.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t ShapeMapWorkLimit = 64'000'000;
		constexpr uint64_t ShapeMapPixelCost = 64;

		template <class Variant> uint64_t ShapeMapValuePixels(const Variant &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value))
				return uint64_t(surface->Data.Width) * surface->Data.Height;
			if (const auto *atlas = std::get_if<AtlasValue>(&value); atlas && atlas->Data)
				return uint64_t(atlas->Data->Surface.Data.Width) * atlas->Data->Surface.Data.Height;
			return 0;
		}
		uint64_t ShapeMapOriginalPixels(const Value &value) {
			uint64_t pixels = ShapeMapValuePixels(value);
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				for (const auto &leaf : array->Elements)
					pixels = std::max(pixels, ShapeMapValuePixels(leaf));
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						pixels = std::max(pixels, ShapeMapValuePixels(leaf));
				const auto visit =
					[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
					if (depth > Limits::MaximumArrayDepth) return;
					for (const auto &item : items) {
						if (const auto *image = std::get_if<Image>(&item.Data))
							pixels = std::max(pixels, uint64_t(image->Width) * image->Height);
						else if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
							pixels = std::max(pixels, ShapeMapValuePixels(*leaf));
						else
							self(self, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1);
					}
				};
				visit(visit, array->Items, 0);
			}
			return pixels;
		}

		// Covers the largest original image before row selection can hide a later expensive row.
		bool ShapeMapAdmitBatch(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			uint64_t pixels = 0;
			if (const auto *image = c.Input("surface_in")) pixels = uint64_t(image->Width) * image->Height;
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "surface_in" && array)
					for (const auto &image : array->Images)
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			const Value *original = nullptr;
			for (auto v = c.ProcessorOriginalValues.rbegin(); v != c.ProcessorOriginalValues.rend(); ++v)
				if (v->first == "surface_in") {
					original = v->second;
					break;
				}
			if (!original)
				for (const auto &[port, value] : c.Values)
					if (port == "surface_in") original = &value;
			if (original) pixels = std::max(pixels, ShapeMapOriginalPixels(*original));
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(c.ProcessorCount));
			if (rows > ShapeMapWorkLimit / ShapeMapPixelCost ||
				pixels > ShapeMapWorkLimit / ShapeMapPixelCost / rows)
				return c.Fail(
					Status::LimitExceeded,
					"Shape Map complete processor batch exceeds work budget",
					"surface_in"
				);
			return true;
		}

		bool ShapeMapCoordinate(NodeContext &c, double u, double v) {
			return (std::isfinite(u) && std::isfinite(v)) ||
				   c.Fail(Status::InvalidValue, "Shape Map sampling coordinate is undefined", "surface_out");
		}

		// Preserve the source's nine paired Lanczos taps, refusing undefined divisions before a texel cast.
		Rgba ShapeMapTexture(
			NodeContext &c, const Image &image, double u, double v, const SamplerSettings &settings
		) {
			if (!ShapeMapCoordinate(c, u, v)) return {};
			if (settings.Interpolation != 4) return TextureInterpolated(image, u, v, settings);
			const double centerU = u - (Fract(u * image.Width) - .5) / image.Width;
			const double centerV = v - (Fract(v * image.Height) - .5) / image.Height;
			const double offsetX = (u - centerU) * image.Width, offsetY = (v - centerV) * image.Height;
			Rgba colour{};
			double weight = 0;
			for (int x = -1; x <= 1; ++x)
				for (int y = -1; y <= 1; ++y) {
					const double wxa = LanczosWeight(x * 2 - 1 - offsetX, 3),
								 wxb = LanczosWeight(x * 2 - offsetX, 3);
					const double wya = LanczosWeight(y * 2 - 1 - offsetY, 3),
								 wyb = LanczosWeight(y * 2 - offsetY, 3);
					const double wx = wxa + wxb, wy = wya + wyb, w = wx * wy;
					const double su = centerU + (x * 2 - .5 + wxb / wx) / image.Width;
					const double sv = centerV + (y * 2 - .5 + wyb / wy) / image.Height;
					if (!ShapeMapCoordinate(c, su, sv)) return {};
					const auto sample = Texture(image, su, sv, true);
					for (size_t channel = 0; channel < 4; ++channel)
						colour[channel] += w * sample[channel];
					weight += w;
				}
			if (weight == 0 || !std::isfinite(weight)) {
				c.Fail(Status::InvalidValue, "Shape Map Lanczos weight is undefined", "interpolate");
				return {};
			}
			for (auto &channel : colour)
				channel /= weight;
			return colour;
		}
	}

	bool SourceShapeMap(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.shape_map");
		if (!ShapeMapAdmitBatch(c)) return false;
		bool failed = false;
		if (CopyWhenInactive(c, failed)) return !failed;
		const Image *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Shape Map requires its surface", "surface_in");
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		const bool singleRed = source->Format == SurfaceFormat::R8Unorm ||
							   source->Format == SurfaceFormat::R16Float ||
							   source->Format == SurfaceFormat::R32Float;
		const int64_t shape = c.Integer("shape"), sides = c.Integer("sides", 4);
		const double scale = c.Scalar("scale", 1), angle = c.Scalar("angle") * (std::numbers::pi / 180);
		const Vector2 mapScale = c.Vec2("map_scale", {4, 1});
		const auto settings = ReadSampler(c);
		if (!singleRed) {
			if (shape < 0 || shape > 1)
				return c.Fail(Status::InvalidValue, "Shape Map shape choice is invalid", "shape");
			if (!std::isfinite(scale) || scale == 0)
				return c.Fail(Status::InvalidValue, "Shape Map scale divisor is undefined", "scale");
			if (shape == 1 && sides == 0)
				return c.Fail(Status::InvalidValue, "Shape Map polygon divisor is undefined", "sides");
			if (!ShapeMapCoordinate(c, mapScale.X, mapScale.Y) || !std::isfinite(angle))
				return c.Fail(
					Status::InvalidValue, "Shape Map controls exceed finite coordinate range", "angle"
				);
			if (!SupportedSampler(c, settings)) return false;
		}
		Image *output = c.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				Rgba colour{};
				if (singleRed)
					colour = SourceSafeDrawPixel(*source, x, y);
				else {
					const double px = (double(x) + .5) / output->Width - .5;
					const double py = (double(y) + .5) / output->Height - .5;
					double u = 0, v = 0;
					if (shape == 0) {
						v = std::hypot(px, py) * 2 / scale;
						if (v > 1) continue;
						u = ((std::atan2(py, px) + angle) / std::numbers::pi + 1) / 2;
					} else {
						const double sx = px * 2 / scale, sy = py * 2 / scale;
						const double a = std::atan2(sy, sx) + std::numbers::pi + angle;
						const double r = 2 * std::numbers::pi / double(sides);
						const double sector = .5 + a / r;
						if (!ShapeMapCoordinate(c, sector, std::hypot(sx, sy))) return false;
						v = std::cos(std::floor(sector) * r - a) * std::hypot(sx, sy);
						if (sides == 3) v *= std::sqrt(3.);
						if (v < 0 || v > 1) continue;
						u = 1 - (a + r / 2) / (2 * std::numbers::pi);
					}
					const double mu = u * mapScale.X, mv = v * mapScale.Y;
					if (!ShapeMapCoordinate(c, mu, mv)) return false;
					colour = ShapeMapTexture(c, *source, Fract(mu), Fract(mv), settings);
					if (c.FailureCode != Status::Ok) return false;
				}
				if (!WritePixel(*output, x, y, colour))
					return c.Fail(
						Status::InvalidValue, "Shape Map sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}
}
