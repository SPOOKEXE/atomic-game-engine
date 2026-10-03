#include "../SourceMappedInputs.hpp"
#include "Gradient.hpp"
#include "Sampler.hpp"
#include "SourcePatternAdmission.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		struct Mapped {
			Vector2 Range;
			const Image *Map = nullptr;
		};
		bool ReadMapped(NodeContext &c, std::string_view port, double fallback, Mapped &value) {
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, value.Range)) return false;
				value.Map = c.Input(std::string(port) + "_map");
			} else {
				auto v = c.Scalar(port, fallback);
				value.Range = {v, v};
			}
			return true;
		}
		double SampleMapped(const Mapped &control, double u, double v, bool filtered) {
			if (!control.Map) return control.Range.X;
			auto sample = filtered ? BilinearClamp(*control.Map, u, v) : SampleNearest(*control.Map, u, v);
			return control.Range.X +
				   (control.Range.Y - control.Range.X) * (sample[0] + sample[1] + sample[2]) / 3;
		}
		double Mod(double x, double n) {
			return x - n * std::floor(x / n);
		}
	}
	bool SourceHerringboneTile(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.herringbone_tile");
		uint64_t keys = 0;
		auto observe = [&](const auto &leaf) {
			if (const auto *g = std::get_if<Gradient>(&leaf)) keys = std::max(keys, uint64_t(g->Keys.size()));
		};
		if (auto *value = source2d::GeneratorOriginal(c, "tile_color"))
			source_pattern::PatternLeaves(*value, observe);
		if (!source_pattern::PatternBatchAdmission(c, 1024 + 16 * keys)) return false;
		auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		uint32_t w = 0, h = 0;
		Vector2 dimension, position;
		if (!source_pattern::Dimensions(c, w, h, dimension) ||
			!source_pattern::Position(c, dimension, {.5, .5}, position))
			return false;
		const auto settings = ReadSampler(c);
		const bool filtered = Filtered(settings);
		const Image *texture = c.Input("texture");
		const bool safeGray = texture && DescribeSurfaceFormat(texture->Format)->Channels == 1;
		const auto mode = c.Integer("render_type");
		if (mode < 0 || mode > 2)
			return c.Fail(Status::InvalidValue, "Herringbone render type is invalid", "render_type");
		Mapped angle, gap, scale;
		if (!ReadMapped(c, "angle", 0, angle) || !ReadMapped(c, "gap", .25, gap)) return false;
		Vector2 scaling = c.Vec2("scale", {.25, .25});
		if (SourceRangeMapped(c, "scale")) {
			if (!ReadSourceMappedRange(c, "scale", scale.Range)) return false;
			scale.Map = c.Input("scale_map");
		} else
			scale.Range = scaling;
		const auto scaleUnit = c.Integer("scale_unit", 1);
		if (scaleUnit < 0 || scaleUnit > 1)
			return c.Fail(Status::InvalidValue, "Herringbone scale unit is invalid", "scale_unit");
		if (scaleUnit == 1 && !source_pattern::SurfaceGetter(c, "scale")) {
			Vector2 reference;
			if (!source2d::ResolveReferenceDimension(c, dimension, reference)) return false;
			scale.Range.X *= reference.X;
			scale.Range.Y *= reference.Y;
		}
		// The wrapper uploads the integer Surface map slot as phase; authored Shift is unused.
		const double seed = c.Scalar("seed"), length = c.Scalar("tile_length", 2), shift = 0,
					 textureSeed = double(c.Integer("texture_seed"));
		if (!safeGray && !c.Find("seed"))
			return c.Fail(
				Status::UnsupportedExecution, "Herringbone requires an authored source seed", "seed"
			);
		if (!safeGray && length == 0)
			return c.Fail(
				Status::UnsupportedExecution, "Herringbone Tile Length divides by zero", "tile_length"
			);
		if (!safeGray && c.Boolean("truchet") && !c.Find("texture_seed"))
			return c.Fail(
				Status::UnsupportedExecution,
				"Herringbone texture transform requires authored Texture Seed",
				"texture_seed"
			);
		const auto in = c.Vec2("level_in", {0, 1}), out = c.Vec2("level_out", {0, 1}),
				   randomAngle = c.Vec2("random_angle", {0, 0});
		if (!safeGray && mode == 1 && in.X == in.Y)
			return c.Fail(Status::UnsupportedExecution, "Herringbone Level In divides by zero", "level_in");
		const auto randomPosition = c.Get<Vector4>("random_position", {0, 0, 0, 0}),
				   randomScale = c.Get<Vector4>("random_scale", {1, 1, 1, 1});
		const auto *gradient = c.Find("tile_color");
		const auto *g = gradient ? std::get_if<Gradient>(gradient) : nullptr;
		GradientSampler sampler;
		if (!safeGray && mode == 0) {
			if (!g)
				return c.Fail(
					Status::TypeMismatch, "Herringbone Tile Color requires a gradient", "tile_color"
				);
			sampler = ReadGradient(c, "tile_color", *g);
			if (!sampler.Map && (g->Keys.empty() || g->Keys.size() > GRADIENT_KEY_SLOTS))
				return c.Fail(
					Status::UnsupportedExecution,
					"Herringbone GLSL gradient requires 1 to64 keys",
					"tile_color"
				);
		}
		const auto hash = [&](double x, double y) {
			return ShaderFract(
				std::sin((x + 85.456034) * 12.9898 + (y + 64.54065) * 78.233) *
				Mod(43758.5453123 + seed, 100000) / 10
			);
		};
		const auto random = [&](double n) { return hash(n, n); };
		Image *image = c.NewImage("surface_out", w, h, *format);
		if (!image) return false;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				const double u = (x + .5) / w, v = (y + .5) / h;
				Rgba colour{};
				if (safeGray) {
					colour = filtered ? BilinearClamp(*texture, u, v) : SampleNearest(*texture, u, v);
					colour = {colour[0], colour[0], colour[0], 1};
				} else {
					double alpha = 1;
					const auto uv = source2d::GeneratorUv(c, u, v, alpha, filtered);
					const double a = SampleMapped(angle, u, v, filtered) * std::numbers::pi / 180;
					Vector2 sca = scale.Range;
					if (scale.Map) {
						const double n = SampleMapped(scale, u, v, filtered);
						sca = {n, n};
					}
					if (sca.X == 0 || sca.Y == 0)
						return c.Fail(
							Status::UnsupportedExecution, "Herringbone Scale divides by zero", "scale"
						);
					const double px = (uv.X - position.X / dimension.X) * dimension.X / dimension.Y,
								 py = uv.Y - position.Y / dimension.Y;
					Vector2 tile{
						(px * std::cos(a) - py * std::sin(a)) * dimension.X / sca.X,
						(px * std::sin(a) + py * std::cos(a)) * dimension.Y / sca.Y
					};
					if (!std::isfinite(tile.X) || !std::isfinite(tile.Y))
						return c.Fail(
							Status::UnsupportedExecution,
							"Herringbone tile coordinates are undefined",
							"scale"
						);
					const double xshifted = (tile.X - std::floor(tile.Y)) * .5;
					if (ShaderFract(xshifted / length) > .5) {
						tile.X -= length - 1;
						std::swap(tile.X, tile.Y);
					}
					tile.X /= length;
					tile.X -= std::floor(tile.Y) / length;
					const double cell = hash(std::floor(tile.X), std::floor(tile.Y));
					Vector2 local{ShaderFract(tile.X), ShaderFract(tile.Y)};
					const double hx = std::abs(local.X - .5) * 2 * length - length + 1,
								 hy = std::abs(local.Y - .5) * 2;
					const double height = std::min(1 - std::max(hx, hy), 1.);
					if (mode == 1) {
						const double n = out.X + (out.Y - out.X) * (height - in.X) / (in.Y - in.X);
						colour = {n, n, n, alpha};
					} else {
						if (mode == 0)
							colour = GradientEval(sampler, ShaderFract(ShaderFract(cell + shift) + 1));
						else {
							if (c.Boolean("truchet")) {
								const double offset = textureSeed / 1000.;
								if (hash(cell + offset, cell + offset) > c.Scalar("flip_threshold", .5))
									local.X = 1 - local.X;
								if (hash(cell + offset + .4864, cell + offset + .6879) >
									c.Scalar("flip_threshold", .5))
									local.Y = 1 - local.Y;
								const double ts =
									hash(cell + textureSeed / 100 + .9843, cell + textureSeed / 100 + .1636);
								const double rot =
									(randomAngle.X + (randomAngle.Y - randomAngle.X) * random(ts)) *
									std::numbers::pi / 180;
								const Vector2 pos{
									randomPosition.X + (randomPosition.Z - randomPosition.X) * random(ts + 1),
									randomPosition.Y + (randomPosition.W - randomPosition.Y) * random(ts + 2)
								};
								const Vector2 scales{
									randomScale.X + (randomScale.Y - randomScale.X) * random(ts + 3),
									randomScale.Z + (randomScale.W - randomScale.Z) * random(ts + 4)
								};
								if (scales.X == 0 || scales.Y == 0)
									return c.Fail(
										Status::UnsupportedExecution,
										"Herringbone Random Scale divides by zero",
										"random_scale"
									);
								const double lx = local.X - .5, ly = local.Y - .5;
								local = {
									(lx * std::cos(rot) - ly * std::sin(rot)) / scales.X + .5 - pos.X,
									(lx * std::sin(rot) + ly * std::cos(rot)) / scales.Y + .5 - pos.Y
								};
							}
							if (!std::isfinite(local.X) || !std::isfinite(local.Y))
								return c.Fail(
									Status::UnsupportedExecution,
									"Herringbone texture coordinate is undefined",
									"texture"
								);
							// Missing source base texture is the white fx pixel sprite, not an empty image.
							colour = texture ? SampleTextureSimple(
												   *texture, local.X, local.Y, settings.Oversample, filtered
											   )
											 : Rgba{1, 1, 1, 1};
						}
						const double threshold = SampleMapped(gap, u, v, filtered),
									 aa = 3 / std::max(dimension.X, dimension.Y);
						double t = height >= threshold ? 1. : 0.;
						if (c.Boolean("anti_aliasing")) {
							t = std::clamp((height - (threshold - aa)) / aa, 0., 1.);
							t = t * t * (3 - 2 * t);
						}
						const auto gapColour = source2d::InputColour(c, "gap_color", {0, 0, 0, 255});
						for (size_t channel = 0; channel < 4; ++channel)
							colour[channel] = gapColour[channel] + (colour[channel] - gapColour[channel]) * t;
						colour[3] *= alpha;
					}
				}
				if (!WritePixel(*image, x, y, colour))
					return c.Fail(
						Status::UnsupportedExecution, "Herringbone shader sample is undefined", "surface_out"
					);
				if (!source_pattern::Mask(c, *image, x, y, u, v)) return false;
			}
		return true;
	}
}
