// Transform family executors.

#include "Families.hpp"
#include "Curve.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		// node_texture_remap.gml and sh_texture_remap: sample the surface at the map's flipped RG position.
		bool TextureRemap(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			const Image *map = context.Input("rg_map");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			if (!map) return context.Fail(Status::InvalidValue, "RG map is missing", "rg_map");
			const Image &size = context.Integer("dimension_source") == 1 ? *map : *source;
			Image *out = context.NewImage("surface_out", size.Width, size.Height);
			if (!out) return false;
			const bool useIndex = context.Boolean("array_index");
			const int64_t start = context.Integer("index_start");
			// A single surface is element 0; with Array Index it draws plainly when 0 < Index Start.
			if (useIndex && 0 < start) {
				for (uint32_t y = 0; y < out->Height; y++)
					for (uint32_t x = 0; x < out->Width; x++)
						WritePixel(*out, x, y, SampleNearest(*source, (x + 0.5) / out->Width, (y + 0.5) / out->Height));
				return true;
			}
			const SamplerSettings sampler = ReadSampler(context);
			const double blend = context.Scalar("blend", 1.0);
			const Vector2 position = context.Vec2("offset");
			const Vector2 scale = context.Vec2("scale", Vector2{1.0, 1.0});
			const double angle = context.Scalar("rotation") * std::numbers::pi / 180.0;
			const double cosine = std::cos(angle), sine = std::sin(angle);
			for (uint32_t y = 0; y < out->Height; y++) {
				for (uint32_t x = 0; x < out->Width; x++) {
					const double u = (x + 0.5) / out->Width, v = (y + 0.5) / out->Height;
					const Rgba remap = TextureInterpolated(*map, u, v, sampler);
					if (useIndex && std::lround(remap[2] * 255.0) != start) continue;
					// pos.x = 1 - pos.x, then pos = 1 - pos: red keeps its value and green flips.
					const double mapX = remap[0], mapY = 1.0 - remap[1];
					const double px = u + (mapX - u) * blend, py = v + (mapY - v) * blend;
					// Row vector times mat2(cos, -sin, sin, cos).
					const double ox = px + position.X - 0.5, oy = py + position.Y - 0.5;
					const double sx = 0.5 + (ox * cosine - oy * sine) / scale.X;
					const double sy = 0.5 + (ox * sine + oy * cosine) / scale.Y;
					Rgba colour = SampleTexture(*source, sx, sy, sampler);
					colour[3] *= remap[3];
					WritePixel(*out, x, y, colour);
				}
			}
			return true;
		}

		// A mapped scalar read with texture2Dintp, as sh_skew and sh_spherize do.
		double MappedScalarIntp(
			const NodeContext &context, std::string_view id, double u, double v, const SamplerSettings &sampler
		) {
			const std::string base(id);
			const Image *map = context.Input(base + "_map");
			if (!map || !context.Boolean(base + "_mapped")) return context.Scalar(id);
			const Vector2 range = context.Vec2(base + "_map_range");
			const Rgba texel = TextureInterpolated(*map, u, v, sampler);
			return range.X + (range.Y - range.X) * ((texel[0] + texel[1] + texel[2]) / 3.0);
		}

		// shaders/sh_level_selector: keep pixels whose luma is within Range of Midpoint.
		bool LevelSelector(NodeContext &context) {
			const double smoothness = context.Scalar("smoothness");
			const bool keep = context.Boolean("keep_original");
			return RunPixelProcessor(context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
				const double middle = MappedScalar(context, "midpoint", u, v), range = MappedScalar(context, "range", u, v);
				Rgba colour = ReadPixel(source, x, y);
				const double luma = colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722;
				if (!keep) colour[0] = colour[1] = colour[2] = 1.0;
				const double distance = std::abs(luma - middle);
				double factor = 0.0;
				if (smoothness == 0.0) {
					factor = 1.0 - (distance < range ? 0.0 : 1.0);
				} else {
					const double t = std::clamp((distance - (range - smoothness)) / (2.0 * smoothness), 0.0, 1.0);
					factor = 1.0 - t * t * (3.0 - 2.0 * t);
				}
				for (size_t channel = 0; channel < 3; channel++)
					colour[channel] *= factor;
				return colour;
			});
		}

		// shaders/sh_skew: shift one axis by the other's distance from Center, optionally scaled by the curve.
		bool Skew(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const bool vertical = context.Integer("axis") == 1;
			const Curve curve = context.Get<Curve>("strength_curve");
			const bool curved = context.Boolean("strength_curved");
			return RunPixelProcessor(context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
				double amount = MappedScalarIntp(context, "strength", u, v, sampler);
				const Vector2 center = UnitVector(context, "center", source.Width, source.Height);
				const double cx = center.X / source.Width, cy = center.Y / source.Height;
				if (curved) amount *= EvalShaderCurve(curve, vertical ? u : v);
				if (!vertical) u += (v - cy) * amount;
				else v += (u - cx) * amount;
				return SampleTexture(source, u, v, sampler);
			});
		}

		// shaders/sh_barrel_distort: radius raised to Intensity around Center, per distance method.
		bool BarrelDistort(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			const Vector2 scale = context.Vec2("scale", Vector2{1, 1});
			const int64_t method = context.Integer("distance_methods");
			return RunPixelProcessor(context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
				const double intensity = MappedScalar(context, "intensity", u, v);
				const Vector2 center = UnitVector(context, "center", source.Width, source.Height);
				const double cx = center.X / source.Width, cy = center.Y / source.Height;
				const double px = (u - cx) / scale.X, py = (v - cy) / scale.Y;
				const double theta = std::atan2(py, px);
				double radius = std::hypot(px, py);
				if (method == 1) radius = std::abs(px) + std::abs(py);
				else if (method == 2) radius = std::max(std::abs(px), std::abs(py));
				else if (method == 3) radius = std::min(std::abs(px), std::abs(py));
				radius = std::pow(radius, intensity);
				return SampleTexture(source, cx + radius * std::cos(theta), cy + radius * std::sin(theta), sampler);
			});
		}

		// shaders/sh_spherize: rotate around Center, push by 1 / sqrt|1 - r^2 / radius|, trim by the depth.
		bool Spherize(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			const bool normalize = context.Boolean("normalize");
			const double trim = context.Scalar("trim_edge"), rotation = context.Scalar("rotation") * std::numbers::pi / 180.0;
			const Vector2 offset = context.Vec2("texture_offset"), textureScale = context.Vec2("texture_scale", Vector2{1, 1});
			return RunPixelProcessor(context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
				const double radius = MappedScalarIntp(context, "radius", u, v, sampler);
				const double strength = MappedScalarIntp(context, "strength", u, v, sampler);
				const Vector2 position = UnitVector(context, "position", source.Width, source.Height);
				const Vector2 center = UnitVector(context, "center", source.Width, source.Height);
				const double tx = u - position.X / source.Width, ty = v - position.Y / source.Height;
				const double cx = center.X / source.Width, cy = center.Y / source.Height;
				// Row vector times mat2(cos, -sin, sin, cos).
				const double ox = tx - cx, oy = ty - cy;
				const double ux = ox * std::cos(rotation) - oy * std::sin(rotation);
				const double uy = ox * std::sin(rotation) + oy * std::cos(rotation);
				const double depth = 1.0 - (ux * ux + uy * uy) / radius;
				double distance = std::sqrt(std::abs(depth));
				if (normalize) distance /= radius;
				double sx = cx + ux + (ux / distance - ux) * strength;
				double sy = cy + uy + (uy / distance - uy) * strength;
				sx = (0.5 + (sx - 0.5) / textureScale.X) - offset.X;
				sy = (0.5 + (sy - 0.5) / textureScale.Y) - offset.Y;
				const Rgba colour = SampleTexture(source, sx, sy, sampler);
				return depth > trim ? colour : Rgba{};
			});
		}

		// shaders/sh_mirror: reflect across the line through Position at Angle; output 1 marks the kept side.
		bool Mirror(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			Image *out = context.NewImage("surface_out", source->Width, source->Height);
			Image *side = out ? context.NewImage("mirror_mask", source->Width, source->Height) : nullptr;
			if (!side) return false;
			constexpr double PI = 3.141592653589793, TAU = 6.283185307179586;
			const Vector2 position = UnitVector(context, "position", source->Width, source->Height);
			const double angle = context.Scalar("angle") * PI / 180.0 + (context.Boolean("flip") ? PI : 0.0);
			const bool both = context.Boolean("both_side");
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
					double su = u, sv = v;
					const double px = u * source->Width - position.X, py = v * source->Height - position.Y;
					double pointAngle = std::atan2(py, px) + angle;
					pointAngle = TAU - (pointAngle - std::floor(pointAngle / TAU) * TAU);
					if (both || pointAngle < PI) {
						const double alpha = (angle + PI) - (pointAngle + angle);
						const double inverse = (angle + PI) + alpha;
						const double distance = std::hypot(px, py);
						su = (position.X + std::cos(inverse) * distance) / source->Width;
						sv = (position.Y - std::sin(inverse) * distance) / source->Height;
					}
					const double mark = pointAngle < PI ? 1.0 : 0.0;
					WritePixel(*side, x, y, Rgba{mark, mark, mark, 1.0});
					Rgba colour{};
					if (both) colour = ReadPixel(*source, x, y);
					if (su > 0.0 && su < 1.0 && sv > 0.0 && sv < 1.0) {
						const Rgba sample = SampleNearest(*source, su, sv);
						for (size_t channel = 0; channel < 4; channel++)
							colour[channel] += sample[channel];
					}
					WritePixel(*out, x, y, colour);
				}
			FinishProcessor(context, *source, *out);
			return true;
		}

		// shaders/sh_dilate: pull pixels toward Center inside Radius by Strength; the UV map's alpha multiplies.
		bool Dilate(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			const UvMap uv = ReadUvMap(context);
			const Curve curve = context.Get<Curve>("strength_curve");
			const bool curved = context.Boolean("strength_curved");
			return RunPixelProcessor(context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
				const double width = source.Width, height = source.Height;
				const double radius = MappedScalarIntp(context, "radius", u, v, sampler) * UnitScale(context, "radius", width);
				const double strength = MappedScalarIntp(context, "strength", u, v, sampler);
				const Vector2 center = UnitVector(context, "center", width, height);
				double uvAlpha = 1.0, mu = u, mv = v;
				UvRemap(uv, mu, mv, 1.0, Filtered(sampler), &uvAlpha);
				const double px = mu * width, py = mv * height;
				const double toX = center.X - px, toY = center.Y - py;
				double distance = std::hypot(toX, toY) / radius;
				if (curved) distance = EvalShaderCurve(curve, distance);
				const double effect = 1.0 - std::clamp(distance, 0.0, 1.0);
				const double tx = (u * width + toX * effect * strength) / width;
				const double ty = (v * height + toY * effect * strength) / height;
				Rgba colour = SampleTexture(source, tx, ty, sampler);
				colour[3] *= uvAlpha;
				return colour;
			});
		}

		// node_downscale.gml and sh_downscale: blocks of Downscale pixels combined by mean, max or min.
		bool Downscale(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const double scale = context.Scalar("downscale", 1.0);
			if (!(scale > 0.0) || !std::isfinite(scale) || scale > 1024.0)
				return context.Fail(Status::InvalidValue, "downscale must be positive and bounded", "downscale");
			const uint32_t width = uint32_t(std::max(1.0, std::ceil(source->Width / scale)));
			const uint32_t height = uint32_t(std::max(1.0, std::ceil(source->Height / scale)));
			Image *out = context.NewImage("surface_out", width, height);
			if (!out) return false;
			const SamplerSettings sampler = ReadSampler(context);
			const int64_t mode = context.Integer("mode");
			const bool multiplyAlpha = context.Boolean("multiply_alpha");
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++) {
					const double u = (x + 0.5) / width, v = (y + 0.5) / height;
					const double px = std::floor(u * width) * scale + 0.5, py = std::floor(v * height) * scale + 0.5;
					const Rgba base = SampleTexture(*source, u, v, sampler);
					Rgba colour = mode == 2 ? Rgba{1, 1, 1, 1} : Rgba{};
					double alpha = 0.0, count = 0.0;
					for (float i = 0.0f; i < float(scale); i++)
						for (float j = 0.0f; j < float(scale); j++) {
							const double sx = (px + i) / source->Width, sy = (py + j) / source->Height;
							if (sx < 0.0 || sx > 1.0 || sy < 0.0 || sy > 1.0) continue;
							Rgba sample = SampleTexture(*source, sx, sy, sampler);
							if (multiplyAlpha)
								for (size_t channel = 0; channel < 3; channel++)
									sample[channel] *= sample[3];
							count++;
							for (size_t channel = 0; channel < 4; channel++) {
								if (mode == 0) colour[channel] += sample[channel] * sample[3];
								else if (mode == 1) colour[channel] = std::max(colour[channel], sample[channel]);
								else colour[channel] = std::min(colour[channel], sample[channel]);
							}
							alpha += sample[3];
						}
					if (mode == 0 && alpha > 0.0) {
						for (size_t channel = 0; channel < 3; channel++)
							colour[channel] /= alpha;
						colour[3] /= count;
					}
					if (mode == 2) colour[3] = base[3];
					WritePixel(*out, x, y, colour);
				}
			return true;
		}

		// shaders/sh_stretch: scale around Anchor along Direction.
		bool Stretch(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const Vector2 anchor = context.Vec2("anchor", Vector2{0.5, 0.5});
			const Vector2 strength = context.Vec2("strength", Vector2{1, 1});
			const double d = context.Scalar("direction") * std::numbers::pi / 180.0;
			return RunPixelProcessor(context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
				double px = u - anchor.X, py = v - anchor.Y;
				// Row vector times mat2(cos, -sin, sin, cos), divided, then the inverse rotation.
				double rx = px * std::cos(d) - py * std::sin(d), ry = px * std::sin(d) + py * std::cos(d);
				rx /= strength.X;
				ry /= strength.Y;
				px = rx * std::cos(-d) - ry * std::sin(-d);
				py = rx * std::sin(-d) + ry * std::cos(-d);
				return SampleTexture(source, anchor.X + px, anchor.Y + py, sampler);
			});
		}

		// node_blur_box.gml: horizontal then vertical sh_blur_box passes, each weighted by the Intensity
		// curve. The first pass's UV map stays bound in the second.
		bool BlurBox(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const SamplerSettings sampler = ReadSampler(context);
			const UvMap uv = ReadUvMap(context);
			const Curve curve = context.Get<Curve>("intensity_modulation");
			const bool separate = context.Boolean("separate_axis");
			const auto pass = [&](const Image &input, bool vertical, std::string_view id) {
				const std::string base(id);
				// The source binds Size Y's own value, not its map, so a separate-axis Y map never applies.
				const Image *map = id == "size_y" ? nullptr : context.Input(base + "_map");
				const bool mapped = map && context.Boolean(base + "_mapped");
				const double scale = UnitScale(context, id, input.Width);
				const Vector2 range = mapped ? context.Vec2(base + "_map_range") : Vector2{context.Scalar(id), context.Scalar(id)};
				const double low = range.X * scale, high = range.Y * scale, largest = std::max(low, high);
				Image output{input.Width, input.Height, std::vector<uint8_t>(input.Pixels.size()), 0};
				for (uint32_t y = 0; y < input.Height; y++)
					for (uint32_t x = 0; x < input.Width; x++) {
						const double u = (x + 0.5) / input.Width, v = (y + 0.5) / input.Height;
						double size = low;
						if (mapped) {
							const Rgba texel = Texture(*map, u, v, Filtered(sampler));
							size = low + (high - low) * ((texel[0] + texel[1] + texel[2]) / 3.0);
						}
						size = std::floor(size);
						Rgba sum{};
						double weight = 0.0;
						for (float i = float(-largest); i <= float(largest); i++) {
							if (i < -size) continue;
							if (i > size) break;
							const double amount = EvalShaderCurve(curve, std::abs(double(i)) / size);
							const Rgba sample = SampleTextureUv(
								input, u + (vertical ? 0.0 : i / input.Width), v + (vertical ? i / input.Height : 0.0),
								amount, uv, sampler
							);
							for (size_t channel = 0; channel < 4; channel++)
								sum[channel] += sample[channel] * amount;
							weight += amount;
						}
						for (double &channel : sum)
							channel /= weight;
						WritePixel(output, x, y, sum);
					}
				return output;
			};
			if (uint64_t(source->Width) * source->Height * 2 * std::max(1.0, std::abs(context.Scalar(separate ? "size_x" : "size"))) >
				256'000'000)
				return context.Fail(Status::LimitExceeded, "box blur size exceeds the work budget", "size");
			const Image horizontal = pass(*source, false, separate ? "size_x" : "size");
			const Image vertical = pass(horizontal, true, separate ? "size_y" : "size");
			Image *out = context.NewImage("surface_out", source->Width, source->Height);
			if (!out) return false;
			out->Pixels = vertical.Pixels;
			FinishProcessor(context, *source, *out);
			return true;
		}

		constexpr ExecutorEntry TRANSFORM_EXECUTORS[] = {
			{"pc.barrel_distort", BarrelDistort},
			{"pc.blur_box", BlurBox},
			{"pc.dilate", Dilate},
			{"pc.downscale", Downscale},
			{"pc.level_selector", LevelSelector},
			{"pc.mirror", Mirror},
			{"pc.skew", Skew},
			{"pc.spherize", Spherize},
			{"pc.stretch", Stretch},
			{"pc.texture_remap", TextureRemap},
		};
	}

	std::span<const ExecutorEntry> TransformExecutors() {
		return TRANSFORM_EXECUTORS;
	}
}
