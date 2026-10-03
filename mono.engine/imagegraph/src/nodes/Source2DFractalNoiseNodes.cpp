#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Blur.hpp"
#include "ColorSpace.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <limits>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t FRACTAL_NOISE_WORK_LIMIT = 64'000'000;
		double NoiseFraction(double n) {
			return n - std::floor(n);
		}
		double NoiseDot(Vector2 a, Vector2 b) {
			return a.X * b.X + a.Y * b.Y;
		}
		Vector2 NoiseHash(Vector2 p, double seed) {
			return {
				-1 + 2 * NoiseFraction(std::sin(NoiseDot(p, {127.1, 311.7})) * (seed / 100.)),
				-1 + 2 * NoiseFraction(std::sin(NoiseDot(p, {269.5, 183.3})) * (seed / 100.))
			};
		}
		// Both pinned shaders use this IQ two-dimensional kernel; the unused Ian3D
		// function is unrelated.
		double NoiseIq(Vector2 p, double seed) {
			constexpr double k1 = .366025404, k2 = .211324865;
			const double skew = (p.X + p.Y) * k1;
			const Vector2 cell{std::floor(p.X + skew), std::floor(p.Y + skew)};
			const double unskew = (cell.X + cell.Y) * k2;
			const Vector2 a{p.X - cell.X + unskew, p.Y - cell.Y + unskew};
			const double m = a.X >= a.Y ? 1 : 0;
			const Vector2 offset{m, 1 - m}, b{a.X - offset.X + k2, a.Y - offset.Y + k2},
				c{a.X - 1 + 2 * k2, a.Y - 1 + 2 * k2};
			const std::array<Vector2, 3> corners{a, b, c};
			const std::array<Vector2, 3> cells{
				cell, Vector2{cell.X + offset.X, cell.Y + offset.Y}, Vector2{cell.X + 1, cell.Y + 1}
			};
			double sum = 0;
			for (size_t i = 0; i < 3; i++) {
				const double h = std::max(.5 - NoiseDot(corners[i], corners[i]), 0.);
				sum += h * h * h * h * NoiseDot(corners[i], NoiseHash(cells[i], seed)) * 70.;
			}
			return sum * .7 + .6;
		}
		bool NoiseDimensions(NodeContext &c, uint32_t &width, uint32_t &height) {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit != 2) return ResolveDimension(c, "dimension", width, height);
			auto size = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension")) {
				const auto *mask = c.Input("mask");
				if (!mask) return c.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				size.X *= mask->Width;
				size.Y *= mask->Height;
			}
			if (!std::isfinite(size.X) || !std::isfinite(size.Y))
				return c.Fail(Status::InvalidValue, "dimensions must be finite", "dimension");
			const auto x = std::max(1., DriverRoundHalfEven(size.X)),
					   y = std::max(1., DriverRoundHalfEven(size.Y));
			if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
				return c.Fail(Status::LimitExceeded, "dimensions exceed native limits", "dimension");
			width = uint32_t(x);
			height = uint32_t(y);
			return c.FailureCode == Status::Ok;
		}
		const Value *NoiseOriginal(const NodeContext &c, std::string_view port) {
			for (auto i = c.ProcessorOriginalValues.rbegin(); i != c.ProcessorOriginalValues.rend(); ++i)
				if (i->first == port) return i->second;
			for (const auto &[id, value] : c.Values)
				if (id == port) return &value;
			return c.Find(port);
		}
		struct NoiseBounds {
			double Maximum = 0, Absolute = 0, MinimumPositive = std::numeric_limits<double>::infinity();
			double Width = 0, Height = 0, Minimum = 0, MinimumWidth = 0, MinimumHeight = 0;
			bool Zero = false, HasVectors = false;
			uint8_t Modes = 0;
		};
		template <class Leaf> void NoiseBoundLeaf(const Leaf &leaf, NoiseBounds &bound) {
			const auto number = [&](double value) {
				if (value == 0) bound.Modes |= 1;
				if (value == 1) bound.Modes |= 2;
				if (value == 2) bound.Modes |= 4;
				bound.Maximum = std::max(bound.Maximum, value);
				bound.Minimum = std::min(bound.Minimum, value);
				bound.Absolute = std::max(bound.Absolute, std::abs(value));
				if (value == 0)
					bound.Zero = true;
				else
					bound.MinimumPositive = std::min(bound.MinimumPositive, std::abs(value));
			};
			if (const auto *v = std::get_if<double>(&leaf))
				number(*v);
			else if (const auto *v = std::get_if<int64_t>(&leaf))
				number(double(*v));
			else if (const auto *v = std::get_if<EnumValue>(&leaf))
				number(double(v->Value));
			else if (const auto *v = std::get_if<bool>(&leaf))
				number(*v ? 1 : 0);
			else if (const auto *v = std::get_if<Vector2>(&leaf)) {
				number(v->X);
				number(v->Y);
				bound.Width = std::max(bound.Width, v->X);
				bound.Height = std::max(bound.Height, v->Y);
				bound.MinimumWidth = std::min(bound.MinimumWidth, v->X);
				bound.MinimumHeight = std::min(bound.MinimumHeight, v->Y);
				bound.HasVectors = true;
			}
		}
		NoiseBounds NoiseAllBounds(const NodeContext &c, std::string_view port) {
			NoiseBounds bound;
			const Value *value = NoiseOriginal(c, port);
			if (!value) return bound;
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				for (const auto &leaf : array->Elements)
					NoiseBoundLeaf(leaf, bound);
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						NoiseBoundLeaf(leaf, bound);
				const auto visit =
					[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
					if (depth > Limits::MaximumArrayDepth) return;
					for (const auto &item : items) {
						if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
							NoiseBoundLeaf(*leaf, bound);
						else
							self(self, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1);
					}
				};
				visit(visit, array->Items, 0);
			} else
				NoiseBoundLeaf(*value, bound);
			return bound;
		}
		bool NoiseBatchDimensions(NodeContext &c, uint32_t &width, uint32_t &height) {
			const auto size = NoiseAllBounds(c, "dimension");
			const auto units = NoiseAllBounds(c, "dimension_unit");
			const bool pixel = (units.Modes & 1) != 0, project = (units.Modes & 2) != 0,
					   maskUnit = (units.Modes & 4) != 0;
			const double x = size.HasVectors ? size.Width : size.Maximum,
						 y = size.HasVectors ? size.Height : size.Maximum,
						 minX = size.HasVectors ? size.MinimumWidth : size.Minimum,
						 minY = size.HasVectors ? size.MinimumHeight : size.Minimum;
			const auto observe = [&](double rawWidth, double rawHeight) {
				if (!std::isfinite(rawWidth) || !std::isfinite(rawHeight))
					return c.Fail(Status::InvalidValue, "dimensions must be finite", "dimension");
				const auto w = std::max(1., DriverRoundHalfEven(rawWidth)),
						   h = std::max(1., DriverRoundHalfEven(rawHeight));
				if (w > Limits::MaximumDimension || h > Limits::MaximumDimension)
					return c.Fail(Status::LimitExceeded, "dimensions exceed native limits", "dimension");
				width = std::max(width, uint32_t(w));
				height = std::max(height, uint32_t(h));
				return true;
			};
			const auto projected = [&](double scaleX, double scaleY) {
				if (!std::isfinite(minX * scaleX) || !std::isfinite(minY * scaleY))
					return c.Fail(Status::InvalidValue, "dimensions must be finite", "dimension");
				return observe(x * scaleX, y * scaleY);
			};
			// Source unit conversion belongs to the originating Dimension input.
			if (c.IsLinked("dimension")) return observe(x, y);
			if (pixel && !observe(x, y)) return false;
			if (project && !projected(c.Project.SurfaceWidth, c.Project.SurfaceHeight)) return false;
			if (maskUnit) {
				double maskWidth = 1, maskHeight = 1;
				if (const auto *mask = c.Input("mask")) {
					maskWidth = mask->Width;
					maskHeight = mask->Height;
				}
				for (const auto &[port, images] : c.ImageArrays)
					if (port == "mask" && images)
						for (const auto &image : images->Images) {
							maskWidth = std::max(maskWidth, double(image.Width));
							maskHeight = std::max(maskHeight, double(image.Height));
						}
				// Independent maxima conservatively bound every selected dimension/mask pairing.
				if (!projected(maskWidth, maskHeight)) return false;
			}
			return true;
		}
		bool NoiseHasMask(const NodeContext &c) {
			if (c.Input("mask")) return true;
			for (const auto &[port, images] : c.ImageArrays)
				if (port == "mask" && images && !images->Images.empty()) return true;
			return false;
		}
		bool NoiseWork(NodeContext &c, uint32_t width, uint32_t height, uint64_t perPixel) {
			if (!NoiseBatchDimensions(c, width, height)) return false;
			const uint64_t rows = std::max<uint64_t>(1, c.ProcessorCount), pixels = uint64_t(width) * height;
			if (perPixel > FRACTAL_NOISE_WORK_LIMIT ||
				rows > FRACTAL_NOISE_WORK_LIMIT / std::max<uint64_t>(1, perPixel) ||
				pixels > FRACTAL_NOISE_WORK_LIMIT / std::max<uint64_t>(1, perPixel) / rows)
				return c.Fail(
					Status::LimitExceeded, "Noise entire processor batch exceeds bounded work", "iteration"
				);
			return true;
		}
		bool NoiseSample(
			NodeContext &c, const Image &image, double u, double v, Rgba &out, std::string_view port
		) {
			if (!std::isfinite(u) || !std::isfinite(v))
				return c.Fail(Status::UnsupportedExecution, "Noise texture coordinates are nonfinite", port);
			out = SampleNearest(image, u, v);
			return true;
		}
		bool NoiseUv(NodeContext &c, double u, double v, Vector2 &uv, double &alpha) {
			uv = {u, v};
			alpha = 1;
			if (const auto *map = c.Input("uv_map")) {
				Rgba p;
				if (!NoiseSample(c, *map, u, v, p, "uv_map")) return false;
				const double mix = c.Scalar("uv_mix", 1);
				uv = {u + (p[0] - u) * mix, v + (1 - p[1] - v) * mix};
				alpha = p[3];
			}
			return (std::isfinite(uv.X) && std::isfinite(uv.Y)) ||
				   c.Fail(Status::UnsupportedExecution, "Noise UV arithmetic is nonfinite", "uv_map");
		}
		Vector2 NoisePosition(NodeContext &c, uint32_t width, uint32_t height) {
			const auto position = UnitVector(c, "position", width, height);
			return {position.X / width, position.Y / height};
		}
		bool NoiseSingleChannel(SurfaceFormat format) {
			return format == SurfaceFormat::R8Unorm || format == SurfaceFormat::R16Float ||
				   format == SurfaceFormat::R32Float;
		}
		// The RGBA8 mask temporary quantizes every format. Safe single-channel draws
		// replace its mask shader.
		bool NoiseMask(NodeContext &c, Image &out) {
			const auto *mask = c.Input("mask");
			if (!mask) return true;
			auto temporary = MakeSurfaceScratch(c, out.Width, out.Height, SurfaceFormat::RGBA8Unorm, "mask");
			if (!temporary) return false;
			for (uint32_t y = 0; y < out.Height; y++)
				for (uint32_t x = 0; x < out.Width; x++) {
					const double u = (x + .5) / out.Width, v = (y + .5) / out.Height;
					const auto p = SampleNearest(*mask, u, v);
					auto generated = SourceSafeDrawPixel(out, x, y);
					if (!NoiseSingleChannel(out.Format)) generated[3] *= (p[0] + p[1] + p[2]) / 3 * p[3];
					if (!WritePixel(temporary->Data, x, y, generated))
						return c.Fail(
							Status::InvalidValue, "Noise mask sample is outside surface range", "mask"
						);
				}
			return CopySurfaceSamples(c, temporary->Data, out, "surface_out");
		}
		struct SimplexSettings {
			Vector2 Scale{}, Iteration{}, Position{}, LevelIn{0, 1}, LevelOut{0, 1};
			std::array<Vector2, 3> ColourRange{};
			double Seed = 0, Rotation = 0, Scaling = 2, Amplitude = .5;
			int ColourMode = 0;
			bool Tile = true;
			const Image *ScaleMap = nullptr, *IterationMap = nullptr;
		};
		double NoiseOctaves(const SimplexSettings &s, Vector2 point, double count) {
			if (count <= 0) return 0;
			const double inverse = 1 / s.Amplitude;
			double amplitude = std::pow(inverse, count - 1) / (std::pow(inverse, count) - 1), noise = 0;
			point = {point.X / 2, point.Y / 2};
			const double maximum = std::max(s.Iteration.X, s.Iteration.Y);
			for (double i = 0; i < maximum; i++) {
				if (i >= count) break;
				noise += NoiseIq(point, s.Seed) * amplitude;
				point.X *= s.Scaling;
				point.Y *= s.Scaling;
				amplitude *= s.Amplitude;
			}
			return noise;
		}
		double NoiseSimplexTiled(const SimplexSettings &s, Vector2 uv, Vector2 scale, double count) {
			const auto evaluate = [&](double x, double y) {
				auto p = source2d::Rotate({uv.X - s.Position.X + x, uv.Y - s.Position.Y + y}, s.Rotation);
				return NoiseOctaves(s, {p.X * scale.X, p.Y * scale.Y}, count);
			};
			const double n00 = evaluate(0, 0);
			if (!s.Tile) return n00;
			const double n01 = evaluate(0, 1), n10 = evaluate(1, 0), n11 = evaluate(1, 1);
			const double a = n00 + (n10 - n00) * (1 - uv.X), b = n01 + (n11 - n01) * (1 - uv.X),
						 n = a + (b - a) * (1 - uv.Y);
			return s.LevelOut.X +
				   (s.LevelOut.Y - s.LevelOut.X) * (n - s.LevelIn.X) / (s.LevelIn.Y - s.LevelIn.X);
		}

	} // namespace
	bool SourceSimplexNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.noise.simplex");
		if (!c.Find("seed"))
			return c.Fail(Status::InvalidValue, "Simplex noise requires a resolved source seed", "seed");
		uint32_t width = 0, height = 0;
		if (!NoiseDimensions(c, width, height)) return false;
		SimplexSettings s;
		s.Seed = c.Scalar("seed");
		s.Position = NoisePosition(c, width, height);
		s.Rotation = c.Scalar("rotation") * std::numbers::pi / 180;
		s.Scaling = c.Scalar("scaling", 2);
		s.Amplitude = c.Scalar("amplitude", .5);
		s.Tile = c.Boolean("tile", true);
		s.LevelIn = c.Vec2("level_in", {0, 1});
		s.LevelOut = c.Vec2("level_out", {0, 1});
		const auto mode = c.SourceChoice("color_mode");
		if (mode != 0 && mode != 1 && mode != 2)
			return c.Fail(Status::InvalidValue, "Simplex color mode is invalid", "color_mode");
		s.ColourMode = int(mode);
		s.ColourRange = {
			c.Vec2("color_r_range", {0, 1}), c.Vec2("color_g_range", {0, 1}), c.Vec2("color_b_range", {0, 1})
		};
		if (c.Boolean("scale_mapped")) {
			if (!ReadSourceMappedRange(c, "scale", s.Scale)) return false;
			s.ScaleMap = c.Input("scale_map");
		} else
			s.Scale = c.Vec2("scale", {.25, .25});
		if (c.Integer("scale_unit", 1) == 1) {
			s.Scale.X *= width;
			s.Scale.Y *= height;
		}
		if (c.Boolean("iteration_mapped")) {
			if (!ReadSourceMappedRange(c, "iteration", s.Iteration)) return false;
			s.Iteration = {DriverRoundHalfEven(s.Iteration.X), DriverRoundHalfEven(s.Iteration.Y)};
			s.IterationMap = c.Input("iteration_map");
		} else {
			const double i = DriverRoundHalfEven(c.Scalar("iteration", 1));
			s.Iteration = {i, i};
		}
		const double maxIterations = std::max(s.Iteration.X, s.Iteration.Y);
		if (!std::isfinite(maxIterations) || maxIterations > double(FRACTAL_NOISE_WORK_LIMIT))
			return c.Fail(Status::LimitExceeded, "Simplex octave count exceeds work budget", "iteration");
		const uint64_t octaves = uint64_t(std::ceil(std::max(0., maxIterations)));
		if (c.ProcessorRow == 0) {
			const double batchIterations = std::max(
				{maxIterations,
				 DriverRoundHalfEven(NoiseAllBounds(c, "iteration").Maximum),
				 DriverRoundHalfEven(NoiseAllBounds(c, "iteration_map_range").Maximum)}
			);
			if (batchIterations > FRACTAL_NOISE_WORK_LIMIT)
				return c.Fail(
					Status::LimitExceeded, "Noise entire processor batch exceeds bounded work", "iteration"
				);
			const uint64_t batchOctaves = uint64_t(std::ceil(std::max(0., batchIterations)));
			const bool batchTile = s.Tile || NoiseAllBounds(c, "tile").Maximum != 0;
			const bool batchColour = s.ColourMode != 0 || NoiseAllBounds(c, "color_mode").Absolute != 0;
			const uint64_t pixelCost = 4 + (NoiseHasMask(c) ? 3 : 0) +
									   batchOctaves * 3 * (batchTile ? 4 : 1) * (batchColour ? 3 : 1);
			if (!NoiseWork(c, width, height, pixelCost)) return false;
		}
		if (s.Tile && s.LevelIn.X == s.LevelIn.Y)
			return c.Fail(
				Status::UnsupportedExecution, "Tiled simplex source levels divide by zero", "level_in"
			);
		if (octaves && (s.Amplitude == 0 || s.Amplitude == 1))
			return c.Fail(
				Status::UnsupportedExecution, "Simplex source octave normalization is undefined", "amplitude"
			);
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		auto *out = c.NewImage("surface_out", width, height, *format);
		if (!out) return false;
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++) {
				Vector2 uv;
				double alpha;
				if (!NoiseUv(c, (x + .5) / width, (y + .5) / height, uv, alpha)) return false;
				const Vector2 aspect{uv.X, uv.Y * double(height) / width};
				Vector2 range = s.Scale;
				double count = s.Iteration.X;
				if (s.ScaleMap) {
					Rgba p;
					if (!NoiseSample(c, *s.ScaleMap, aspect.X, aspect.Y, p, "scale_map")) return false;
					const double value = range.X + (range.Y - range.X) * (p[0] + p[1] + p[2]) / 3;
					range = {value, value};
				}
				if (s.IterationMap) {
					Rgba p;
					if (!NoiseSample(c, *s.IterationMap, aspect.X, aspect.Y, p, "iteration_map"))
						return false;
					count = s.Iteration.X + (s.Iteration.Y - s.Iteration.X) * (p[0] + p[1] + p[2]) / 3;
				}
				if (range.X == 0 || range.Y == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Simplex source scale divides by zero", "scale"
					);
				const Vector2 scale{width / range.X, height / range.Y};
				Rgba colour{0, 0, 0, alpha};
				if (!s.ColourMode) {
					const double value = NoiseSimplexTiled(s, aspect, scale, count);
					colour = {value, value, value, alpha};
				} else {
					const std::array<Vector2, 3> offsets{
						Vector2{0, 0}, Vector2{1.7227, 4.55529}, Vector2{6.9950, 6.82063}
					};
					Rgb3 rgb{};
					for (size_t i = 0; i < 3; i++) {
						const double value = NoiseSimplexTiled(
							s, {aspect.X + offsets[i].X, aspect.Y + offsets[i].Y}, scale, count
						);
						rgb[i] = s.ColourRange[i].X + value * (s.ColourRange[i].Y - s.ColourRange[i].X);
					}
					if (s.ColourMode == 2) rgb = ShaderHsvToRgb(rgb);
					colour = {rgb[0], rgb[1], rgb[2], alpha};
				}
				if (!WritePixel(*out, x, y, colour))
					return c.Fail(
						Status::UnsupportedExecution,
						"Simplex source arithmetic produces nonfinite or "
						"unrepresentable samples",
						"surface_out"
					);
			}
		return NoiseMask(c, *out);
	}
	namespace {
		struct RidgeSettings {
			Vector2 Position{}, Scale{2, 2}, Level{0, 1};
			double Seed = 0, Rotation = 0, Amplitude = 1, CellScale = 4, RidgeScale = 32, RidgeAngle = 0,
				   Contrast = .5, MultiplyFactor = 16;
			int Mode = 0, Blend = 0;
			bool Multiply = false;
		};
		Vector2 RidgeRandom(Vector2 p) {
			return {
				NoiseFraction(std::sin(NoiseDot(p, {127.1, 311.7})) * 43758.5453),
				NoiseFraction(std::sin(NoiseDot(p, {269.5, 183.3})) * 43758.5453)
			};
		}
		bool RidgePass(NodeContext &c, const Image &source, Image &target, const RidgeSettings &s) {
			for (uint32_t y = 0; y < target.Height; y++)
				for (uint32_t x = 0; x < target.Width; x++) {
					const double u = (x + .5) / target.Width, v = (y + .5) / target.Height;
					Vector2 uv;
					double alpha;
					if (!NoiseUv(c, u, v, uv, alpha)) return false;
					auto p = source2d::Rotate(
						{uv.X - s.Position.X, uv.Y * double(target.Height) / target.Width - s.Position.Y},
						s.Rotation
					);
					p.X *= s.Scale.X;
					p.Y *= s.Scale.Y;
					const auto base = SampleNearest(source, u, v);
					const Vector2 grad{
						SampleNearest(source, u, v + 1. / target.Height)[0] -
							SampleNearest(source, u, v - 1. / target.Height)[0],
						SampleNearest(source, u + 1. / target.Width, v)[0] -
							SampleNearest(source, u - 1. / target.Width, v)[0]
					};
					// The native CPU profile defines a flat gradient direction with atan2(0, 0) = 0.
					const double dir = std::atan2(grad.Y, grad.X) + std::numbers::pi / 2 + s.RidgeAngle,
								 dis = std::sqrt(NoiseDot(grad, grad)) * s.MultiplyFactor;
					Vector2 origin{.5, .5};
					if (s.CellScale > 0) {
						const Vector2 cs{s.Scale.X * s.CellScale, s.Scale.Y * s.CellScale},
							st{p.X * cs.X, p.Y * cs.Y};
						if (!std::isfinite(st.X) || !std::isfinite(st.Y) || cs.X == 0 || cs.Y == 0)
							return c.Fail(
								Status::UnsupportedExecution,
								"Ridge source cell coordinates are undefined",
								"cell_scale"
							);
						const Vector2 cell{std::floor(st.X), std::floor(st.Y)},
							fraction{NoiseFraction(st.X), NoiseFraction(st.Y)};
						double nearest = 100;
						for (int yy = -1; yy <= 1; yy++)
							for (int xx = -1; xx <= 1; xx++) {
								auto point = RidgeRandom({cell.X + xx, cell.Y + yy});
								point = {
									.5 + .5 * std::sin(s.Seed + 2 * std::numbers::pi * point.X),
									.5 + .5 * std::sin(s.Seed + 2 * std::numbers::pi * point.Y)
								};
								const Vector2 d{fraction.X - xx - point.X, fraction.Y - yy - point.Y};
								const double dist = std::sqrt(NoiseDot(d, d));
								if (dist < nearest) {
									nearest = dist;
									origin = {cell.X + xx + point.X, cell.Y + yy + point.Y};
								}
							}
						origin = {origin.X / cs.X, origin.Y / cs.Y};
					}
					const auto d = source2d::Rotate({p.X - origin.X, p.Y - origin.Y}, dir);
					const double stripe =
						s.Mode == 0
							? std::cos(d.Y * s.RidgeScale)
							: std::abs(NoiseFraction(d.Y / (2 * std::numbers::pi) * s.RidgeScale) - .5) * 4 -
								  1;
					double value = std::max(0., (1 - s.Contrast) + s.Contrast * stripe);
					if (s.Multiply) value *= dis;
					value = (value - s.Level.X) / (s.Level.Y - s.Level.X);
					const double h = (std::pow(2 * base[0] - 1, 3.) + 1) / 2;
					Rgba result{};
					if (!s.Blend) {
						const double ridge = value * h * s.Amplitude;
						result = {base[0] + ridge, base[1] + ridge, base[2] + ridge, (base[3] + 1) * alpha};
					} else {
						const double ridge = h > .5 ? 1 - (1 - 2 * (h - .5)) * (1 - value) : 2 * h * value;
						result = {
							std::max(base[0], ridge),
							std::max(base[1], ridge),
							std::max(base[2], ridge),
							alpha
						};
					}
					if (!WritePixel(target, x, y, result))
						return c.Fail(
							Status::UnsupportedExecution,
							"Ridge source arithmetic produces nonfinite or "
							"unrepresentable samples",
							"surface_out"
						);
				}
			return true;
		}
	} // namespace
	bool SourceRidgeNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.noise.ridge");
		uint32_t width = 0, height = 0;
		if (!NoiseDimensions(c, width, height)) return false;
		const double iterations = DriverRoundHalfEven(c.Scalar("iteration", 1));
		if (iterations > 1022)
			return c.Fail(
				Status::UnsupportedExecution, "Ridge source power normalization overflows", "iteration"
			);
		const uint64_t count = uint64_t(std::max(0., iterations));
		RidgeSettings s;
		s.Position = NoisePosition(c, width, height);
		s.Scale = c.Vec2("scale", {2, 2});
		s.Seed = c.Scalar("seed");
		s.Rotation = c.Scalar("rotation") * std::numbers::pi / 180;
		s.RidgeAngle = c.Scalar("ridge_rotation") * std::numbers::pi / 180;
		s.RidgeScale = c.Scalar("ridge_scale", 32);
		s.CellScale = c.Scalar("cell_scale", 4);
		s.Contrast = c.Scalar("ridge_contrast", .5);
		s.Multiply = c.Boolean("ridge_multiply");
		s.MultiplyFactor = c.Scalar("ridge_multiplier", 16);
		s.Level = c.Vec2("level", {0, 1});
		const double mode = c.SourceChoice("mode"), blend = c.SourceChoice("blend_mode");
		if (mode != 0 && mode != 1)
			return c.Fail(Status::InvalidValue, "Ridge noise mode is invalid", "mode");
		if (blend != 0 && blend != 1)
			return c.Fail(Status::InvalidValue, "Ridge noise blend mode is invalid", "blend_mode");
		s.Mode = int(mode);
		s.Blend = int(blend);
		const auto *heightmap = c.Input("heightmap");
		if ((!heightmap || count) && !c.Find("seed"))
			return c.Fail(Status::InvalidValue, "Ridge noise requires a resolved source seed", "seed");
		if (count && s.Level.X == s.Level.Y)
			return c.Fail(Status::UnsupportedExecution, "Ridge source levels divide by zero", "level");
		const double factor = c.Scalar("itr_factor", 1.5);
		double blur = c.Scalar("blending", 4);
		if (count > 1 && factor == 0)
			return c.Fail(
				Status::UnsupportedExecution, "Ridge source blur evolution is nonfinite", "itr_factor"
			);
		if (c.ProcessorRow == 0) {
			const double batchIterations =
				std::max(iterations, DriverRoundHalfEven(NoiseAllBounds(c, "iteration").Maximum));
			if (batchIterations > 1022)
				return c.Fail(
					Status::LimitExceeded, "Noise entire processor batch exceeds bounded work", "iteration"
				);
			const uint64_t batchCount = uint64_t(std::max(0., batchIterations));
			const auto factors = NoiseAllBounds(c, "itr_factor");
			const double smallestFactor = std::min(std::abs(factor), factors.MinimumPositive);
			if (batchCount > 1 && (factor == 0 || factors.Zero))
				return c.Fail(
					Status::LimitExceeded,
					"Noise entire processor batch has no finite blur work bound",
					"itr_factor"
				);
			uint64_t work = 6 + (NoiseHasMask(c) ? 3 : 0);
			double previewBlur = std::max(std::abs(blur), NoiseAllBounds(c, "blending").Absolute);
			for (uint64_t i = 0; i < batchCount; i++) {
				// Two passes: center + both offsets, writes/copy, and generated/normalized kernel weights.
				const double taps = previewBlur > 1 ? 6 * std::ceil(previewBlur) + 4 : 0;
				if (!std::isfinite(taps) || taps > FRACTAL_NOISE_WORK_LIMIT - 16 ||
					work > FRACTAL_NOISE_WORK_LIMIT - uint64_t(taps) - 16)
					return c.Fail(
						Status::LimitExceeded, "Noise entire processor batch exceeds bounded work", "blending"
					);
				work += uint64_t(taps) + 16;
				previewBlur /= smallestFactor;
			}
			if (!NoiseWork(c, width, height, work)) return false;
		}
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		auto first = MakeSurfaceScratch(c, width, height, *format, "ridge_first"),
			 second = MakeSurfaceScratch(c, width, height, *format, "ridge_second");
		if (!first || !second) return false;
		s.Amplitude = iterations < 1 ? 1 : std::pow(2., iterations) / (std::pow(2., iterations + 1) - 1);
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				Rgba colour;
				if (heightmap) {
					if (!NoiseSample(c, *heightmap, u, v, colour, "heightmap")) return false;
					if (heightmap->Format == SurfaceFormat::R8Unorm ||
						heightmap->Format == SurfaceFormat::R16Float ||
						heightmap->Format == SurfaceFormat::R32Float)
						colour = {colour[0], colour[0], colour[0], 1};
				} else {
					Vector2 uv;
					double alpha;
					if (!NoiseUv(c, u, v, uv, alpha)) return false;
					auto p = source2d::Rotate(
						{uv.X - s.Position.X, uv.Y * double(height) / width - s.Position.Y}, s.Rotation
					);
					const double value = NoiseIq({p.X * s.Scale.X, p.Y * s.Scale.Y}, s.Seed) * s.Amplitude;
					colour = {value, value, value, alpha};
				}
				if (!WritePixel(first->Data, x, y, colour))
					return c.Fail(
						Status::UnsupportedExecution,
						"Ridge initial source height is nonfinite or unrepresentable",
						"heightmap"
					);
			}
		const auto sampler = ReadSampler(c);
		for (uint64_t i = 0; i < count; i++) {
			if (!RidgePass(c, first->Data, second->Data, s)) return false;
			// Both Gaussian safe draws replace the blur shader for R8/R16/R32,
			// producing identity passes.
			if (blur > 1 && !NoiseSingleChannel(second->Data.Format)) {
				GaussianArgs args;
				args.Size = args.SizeHigh = blur;
				args.SampleMode = sampler.Oversample;
				auto blurred = GaussianBlur(c, second->Data, args);
				if (!blurred) return false;
				if (!CopySurfaceSamples(c, blurred->Data, second->Data, "blending")) return false;
			}
			std::swap(first, second);
			s.Scale.X *= factor;
			s.Scale.Y *= factor;
			s.Amplitude /= factor;
			blur /= factor;
			s.RidgeScale /= factor;
		}
		auto *out = c.NewImage("surface_out", width, height, *format);
		if (!out || !CopySurfaceSamples(c, first->Data, *out, "surface_out")) return false;
		return NoiseMask(c, *out);
	}
} // namespace engine::imagegraph::detail
