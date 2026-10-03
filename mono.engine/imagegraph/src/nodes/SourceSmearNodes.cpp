#include "../SourceSafeDraw.hpp"
#include "Curve.hpp"
#include "Sampler.hpp"

#include <engine/core/Profiling.hpp>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t SMEAR_WORK = 64000000;
		struct SmearSettings {
			SamplerSettings Sampler;
			UvMap Uv;
			const Image *Source = nullptr, *TextureImage = nullptr, *StrengthMap = nullptr,
						*DirectionMap = nullptr;
			const Curve *StrengthCurve = nullptr, *SpreadCurve = nullptr;
			Vector2 Strength, Direction;
			Rgba Side{1, 1, 1, 1};
			double Spread = 0;
			uint64_t Steps = 1, Angles = 1;
			int64_t Mode = 0, Modulate = 0, Render = 0, Blend = 0;
			bool Invert = false;
		};
		struct SmearPixel {
			Rgba Colour{};
			double Depth = 0;
			bool DepthDefined = true, ColourDefined = true;
		};
		double Brightness(const Rgba &c) {
			return (c[0] + c[1] + c[2]) / 3 * c[3];
		}
		void Multiply(Rgba &a, const Rgba &b) {
			for (size_t k = 0; k < 4; ++k)
				a[k] *= b[k];
		}
		Rgba UndefinedSmearSample() {
			const double value = std::numeric_limits<double>::quiet_NaN();
			return {value, value, value, value};
		}
		bool SmearFinite(const Rgba &pixel) {
			return std::all_of(pixel.begin(), pixel.end(), [](double channel) {
				return std::isfinite(channel);
			});
		}
		SmearPixel UndefinedSmearPixel() {
			return {UndefinedSmearSample(), std::numeric_limits<double>::quiet_NaN(), false, false};
		}
		Rgba SmearTexture(const Image &image, double u, double v, bool filtered) {
			if (!std::isfinite(u) || !std::isfinite(v)) return UndefinedSmearSample();
			return Texture(image, u, v, filtered);
		}
		// All samplers share the main source dimensions, including the optional final
		// texture.
		Rgba SmearSample(const SmearSettings &s, const Image &image, double u, double v, double blend = 0) {
			if (!std::isfinite(u) || !std::isfinite(v)) return UndefinedSmearSample();
			UvRemap(s.Uv, u, v, blend, false);
			if (!std::isfinite(u) || !std::isfinite(v)) return UndefinedSmearSample();
			if (u < 0 || u > 1 || v < 0 || v > 1) {
				switch (s.Sampler.Oversample) {
				case 2:
					return {0, 0, 0, 1};
				case 3:
					u = std::clamp(u, 0., 1.);
					v = std::clamp(v, 0., 1.);
					break;
				case 4:
					u = Fract(u);
					v = Fract(v);
					break;
				case 6:
					if (v < 0 || v > 1) return {};
					u = Fract(u);
					break;
				case 7:
					if (v < 0 || v > 1) return {0, 0, 0, 1};
					u = Fract(u);
					break;
				case 8:
					u = Fract(u);
					v = std::clamp(v, 0., 1.);
					break;
				case 10:
					if (u < 0 || u > 1) return {};
					v = Fract(v);
					break;
				case 11:
					if (u < 0 || u > 1) return {0, 0, 0, 1};
					v = Fract(v);
					break;
				case 12:
					u = std::clamp(u, 0., 1.);
					v = Fract(v);
					break;
				default:
					return {};
				}
			}
			const Vector2 dimension{double(s.Source->Width), double(s.Source->Height)};
			const bool filtered = &image == s.Source && Filtered(s.Sampler);
			if (s.Sampler.Interpolation == 3) {
				const double x = u * dimension.X + .5, y = v * dimension.Y + .5;
				const double ix = std::floor(x), iy = std::floor(y), fx = x - ix, fy = y - iy;
				return SmearTexture(
					image,
					(ix + fx * fx * (3 - 2 * fx) - .5) / dimension.X,
					(iy + fy * fy * (3 - 2 * fy) - .5) / dimension.Y,
					filtered
				);
			}
			if (s.Sampler.Interpolation == 4) {
				const double cu = u - (Fract(u * dimension.X) - .5) / dimension.X,
							 cv = v - (Fract(v * dimension.Y) - .5) / dimension.Y;
				const double ox = (u - cu) * dimension.X, oy = (v - cv) * dimension.Y;
				Rgba color{};
				double weights = 0;
				for (int x = -1; x <= 1; ++x)
					for (int y = -1; y <= 1; ++y) {
						const double xa = LanczosWeight(x * 2 - 1 - ox, 3), xb = LanczosWeight(x * 2 - ox, 3);
						const double ya = LanczosWeight(y * 2 - 1 - oy, 3), yb = LanczosWeight(y * 2 - oy, 3);
						const double wx = xa + xb, wy = ya + yb, weight = wx * wy;
						const auto pixel = SmearTexture(
							image,
							cu + (x * 2 - .5 + xb / wx) / dimension.X,
							cv + (y * 2 - .5 + yb / wy) / dimension.Y,
							filtered
						);
						for (size_t k = 0; k < 4; ++k)
							color[k] += pixel[k] * weight;
						weights += weight;
					}
				for (double &channel : color)
					channel /= weights;
				return color;
			}
			return SmearTexture(image, u, v, filtered);
		}
		double Mapped(const Image *map, Vector2 range, double u, double v) {
			if (!map) return range.X;
			const Rgba c = Texture(*map, u, v, false);
			return range.X + (range.Y - range.X) * (c[0] + c[1] + c[2]) / 3;
		}
		SmearPixel ShadeSmear(const SmearSettings &s, double u, double v) {
			SmearPixel result;
			const double strength = Mapped(s.StrengthMap, s.Strength, u, v),
						 direction = Mapped(s.DirectionMap, s.Direction, u, v);
			Vector2 position{};
			bool lastPositionDefined = false;
			const float delta = 1.f / float(std::max(s.Source->Width, s.Source->Height)) /
								float(s.Steps / std::max(s.Source->Width, s.Source->Height));
			for (uint64_t a = 0; a < s.Angles; ++a) {
				double angle = -s.Spread + double(a);
				if (s.SpreadCurve) angle *= EvalShaderCurve(*s.SpreadCurve, std::abs(angle / s.Spread));
				const double radians = (direction + 90 + angle) * std::numbers::pi / 180;
				const Vector2 shift{std::sin(radians) * strength, std::cos(radians) * strength};
				Rgba base = SmearSample(s, *s.Source, u, v);
				if (!SmearFinite(base)) return UndefinedSmearPixel();
				for (size_t k = 0; k < 3; ++k)
					base[k] *= base[3];
				double maximum = Brightness(base), depth = 0;
				Rgba colour = s.Invert ? (s.Mode == 0 ? Rgba{0, 0, 0, 1} : Rgba{}) : base;
				bool positionDefined = false;
				for (float progress = 0; progress <= 1.f; progress += delta) {
					const double i = progress;
					const double sign = s.Invert ? 1 : -1;
					const double su = u + sign * shift.X * i, sv = v + sign * shift.Y * i;
					Rgba sample = SmearSample(s, *s.Source, su, sv, i);
					if (!SmearFinite(sample)) return UndefinedSmearPixel();
					if (!s.Invert) {
						for (size_t k = 0; k < 3; ++k)
							sample[k] *= sample[3];
						if (s.Modulate != 2) {
							double attenuation = 1 - i;
							if (s.StrengthCurve)
								attenuation *= EvalShaderCurve(*s.StrengthCurve, attenuation);
							if (s.Mode == 0)
								for (size_t k = 0; k < 3; ++k)
									sample[k] *= attenuation;
							else
								sample[3] *= attenuation;
						}
						const double bright = Brightness(sample);
						if (!(bright > maximum)) continue;
						maximum = bright;
						colour = sample;
						if (i != 0) Multiply(colour, s.Side);
						depth = i / bright;
					} else {
						const double bright = Brightness(sample);
						if (bright == 0 || i > bright) continue;
						if (s.Modulate != 2) {
							if (s.Mode == 0)
								for (size_t k = 0; k < 3; ++k)
									sample[k] *= i;
							else
								sample[3] *= i;
						}
						if (s.Render == 2) {
							colour = sample;
							if (s.StrengthCurve)
								for (double &channel : colour)
									channel *= EvalShaderCurve(*s.StrengthCurve, i);
						} else if (s.Render == 3)
							colour = {1, 1, 1, 1};
						else {
							double intensity = s.Render == 0 ? i : i / bright;
							if (s.StrengthCurve) intensity *= EvalShaderCurve(*s.StrengthCurve, intensity);
							colour = s.Mode == 0 ? Rgba{intensity, intensity, intensity, 1}
												 : Rgba{1, 1, 1, intensity};
						}
						if (std::abs(i - bright) >= delta) Multiply(colour, s.Side);
						depth = i / bright;
					}
					position = {su, sv};
					positionDefined = true;
				}
				if (!s.Invert && s.Modulate == 1) {
					colour = base;
					if (s.Mode == 0)
						for (size_t k = 0; k < 3; ++k)
							colour[k] *= maximum;
					else
						colour[3] *= maximum;
				}
				result.DepthDefined &= positionDefined;
				if (positionDefined) result.Depth = std::max(result.Depth, depth);
				lastPositionDefined = positionDefined;
				for (size_t k = 0; k < 4; ++k) {
					if (s.Blend == 0)
						result.Colour[k] = std::max(result.Colour[k], colour[k]);
					else
						result.Colour[k] += colour[k];
				}
			}
			if (s.TextureImage) {
				result.ColourDefined = lastPositionDefined;
				if (lastPositionDefined)
					Multiply(result.Colour, SmearSample(s, *s.TextureImage, position.X, position.Y));
			}
			return result;
		}
		bool SmearCurve(NodeContext &c, std::string_view toggle, std::string_view port, const Curve *&out) {
			if (!c.Boolean(toggle)) return true;
			const Value *v = c.Find(port);
			out = v ? std::get_if<Curve>(v) : nullptr;
			if (!out || out->Anchors.empty() || out->Anchors.size() > 9 || out->Header[1] == 0)
				return c.Fail(
					Status::UnsupportedExecution, "Smear curve requires a defined bounded GLSL uniform", port
				);
			return true;
		}
		Vector2 SmearRange(
			NodeContext &c, std::string_view port, std::string_view range, bool mapped, double fallback
		) {
			const Value *v = c.Find(port);
			if (mapped && v && (c.IsLinked(port) || !c.IsCatalogueDefault(port).value_or(false))) {
				if (const auto *tuple = std::get_if<Vector2>(v)) return *tuple;
			}
			if (mapped) return c.Vec2(range, {fallback, fallback});
			const double value = c.Scalar(port, fallback);
			return {value, value};
		}
		template <class Leaf> void SmearItems(const std::vector<SourceArrayItem> &items, const Leaf &visit) {
			for (const auto &item : items) {
				if (const auto *element = std::get_if<ElementValue>(&item.Data))
					std::visit([&](const auto &x) { visit(x); }, *element);
				else if (const auto *nested = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					SmearItems(*nested, visit);
			}
		}
		template <class Leaf> void SmearLeaves(const Value &value, const Leaf &visit) {
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				for (const auto &element : array->Elements)
					std::visit([&](const auto &x) { visit(x); }, element);
				SmearItems(array->Items, visit);
				for (const auto &nested : array->Nested)
					for (const auto &element : nested)
						std::visit([&](const auto &x) { visit(x); }, element);
			} else
				std::visit([&](const auto &x) { visit(x); }, value);
		}
		bool SmearBatchWork(NodeContext &c, const Image &source) {
			if (c.ProcessorRow != 0) return true;
			uint64_t width = source.Width, height = source.Height;
			const auto redOnly = [](const Image &image) {
				return image.Format == SurfaceFormat::R8Unorm || image.Format == SurfaceFormat::R16Float ||
					   image.Format == SurfaceFormat::R32Float;
			};
			bool needsShader = !redOnly(source);
			double resolution = 1, spread = 0, feather = 0;
			uint64_t samplerCost = Filtered(ReadSampler(c)) ? 4 : 1;
			if (ReadSampler(c).Interpolation == 4) samplerCost = 36;
			const auto scan = [&](std::string_view port, const Value &value) {
				SmearLeaves(value, [&](const auto &leaf) {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t>) {
						const double number = double(leaf);
						if (port == "resolution") resolution = std::max(resolution, number);
						if (port == "spread") spread = std::max(spread, number);
						if (port == "mask_feather") feather = std::max(feather, number);
						if (port == "interpolate") {
							const double mode = number == 0 ? c.InheritedInterpolation : number;
							samplerCost = std::max<uint64_t>(
								samplerCost, mode == 4 ? 36 : (mode == 2 || mode == 3 ? 4 : 1)
							);
						}
					}
					if constexpr (std::is_same_v<T, EnumValue>)
						if (port == "interpolate") {
							const auto mode = leaf.Value == 0 ? c.InheritedInterpolation : leaf.Value;
							samplerCost = std::max<uint64_t>(
								samplerCost, mode == 4 ? 36 : (mode == 2 || mode == 3 ? 4 : 1)
							);
						}
				});
			};
			for (const auto &[port, value] : c.Values)
				scan(port, value);
			for (const auto &[port, value] : c.ProcessorOriginalValues)
				if (value) scan(port, *value);
			for (const auto &[port, images] : c.ImageArrays)
				if (images && port == "surface_in")
					for (const auto &image : images->Images) {
						needsShader |= !redOnly(image);
						width = std::max<uint64_t>(width, image.Width);
						height = std::max<uint64_t>(height, image.Height);
					}
			// Two shader passes validate undefined out parameters before any output
			// allocation.
			const double cost = !needsShader ? double(width) * double(height) * double(c.ProcessorCount)
											 : double(width) * double(height) *
												   (double(std::max(width, height)) * resolution + 1) *
												   (std::floor(spread * 2) + 1) *
												   double(samplerCost * 6 + 40) * double(c.ProcessorCount);
			if (!std::isfinite(cost) || cost > SMEAR_WORK)
				return c.Fail(
					Status::LimitExceeded,
					"Smear whole processor batch exceeds sample work budget",
					"resolution"
				);
			uint64_t maskWidth = 0, maskHeight = 0;
			for (const auto &[port, image] : c.Images)
				if (port == "mask" && image) {
					maskWidth = std::max<uint64_t>(maskWidth, image->Width);
					maskHeight = std::max<uint64_t>(maskHeight, image->Height);
				}
			for (const auto &[port, images] : c.ImageArrays)
				if (port == "mask" && images)
					for (const auto &image : images->Images) {
						maskWidth = std::max<uint64_t>(maskWidth, image.Width);
						maskHeight = std::max<uint64_t>(maskHeight, image.Height);
					}
			const double featherCost = feather > 0 ? double(maskWidth) * double(maskHeight) *
														 std::max(1., std::round(feather)) * 2 *
														 double(c.ProcessorCount)
												   : 0;
			if (!std::isfinite(featherCost) || featherCost > SMEAR_WORK - cost)
				return c.Fail(
					Status::LimitExceeded,
					"Smear whole processor batch exceeds aggregate sample and feather work budget",
					"mask_feather"
				);
			return true;
		}
	} // namespace
	bool SourceSmear(NodeContext &c) {
		ENGINE_PROFILE("imagegraph source smear");
		bool failed = false;
		if (CopyWhenInactive(c, failed)) {
			if (failed) return false;
			return c.SetOutputDiagnostic(
				"depth_pass",
				Status::UnsupportedExecution,
				"inactive Smear retains prior depth; "
				"caller-owned prior output is required"
			);
		}
		const Image *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Smear requires its input surface", "surface_in");
		if (!SmearBatchWork(c, *source)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		if (source->Format == SurfaceFormat::R8Unorm || source->Format == SurfaceFormat::R16Float ||
			source->Format == SurfaceFormat::R32Float) {
			Image *out = c.NewImage("surface_out", source->Width, source->Height, *format);
			if (!out) return false;
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x)
					if (!WritePixel(*out, x, y, SourceSafeDrawPixel(*source, x, y)))
						return c.Fail(
							Status::InvalidValue,
							"Smear red draw exceeds numeric surface range",
							"surface_out"
						);
			FinishProcessor(c, *source, *out);
			return c.FailureCode == Status::Ok && c.SetOutputDiagnostic(
													  "depth_pass",
													  Status::UnsupportedExecution,
													  "source red-only draw replaces Smear shader "
													  "and does not write depth"
												  );
		}
		SmearSettings s;
		s.Source = source;
		s.Sampler = ReadSampler(c);
		s.Uv = ReadUvMap(c);
		if (!SupportedSampler(c, s.Sampler)) return false;
		const auto resolution = c.Integer("resolution", 1);
		if (resolution <= 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"source Smear resolution produces a nonprogressing or undefined loop",
				"resolution"
			);
		s.Steps = uint64_t(std::max(source->Width, source->Height)) * uint64_t(resolution);
		s.Spread = c.Scalar("spread");
		if (!std::isfinite(s.Spread) || s.Spread > SMEAR_WORK)
			return c.Fail(Status::LimitExceeded, "Smear spread exceeds work budget", "spread");
		s.Angles = s.Spread < 0 ? 0 : uint64_t(std::floor(s.Spread * 2)) + 1;
		s.Mode = c.Integer("mode");
		s.Modulate = c.Integer("modulate_strength");
		s.Render = c.Integer("render_mode");
		s.Blend = c.Integer("blend_mode");
		s.Invert = c.Boolean("invert");
		const bool mappedStrength = c.Boolean("strength_mapped"),
				   mappedDirection = c.Boolean("direction_mapped");
		s.Strength = SmearRange(c, "strength", "strength_map_range", mappedStrength, .2);
		s.Direction = SmearRange(c, "direction", "direction_map_range", mappedDirection, 0);
		s.StrengthMap = mappedStrength ? c.Input("strength_map") : nullptr;
		s.DirectionMap = mappedDirection ? c.Input("direction_map") : nullptr;
		s.TextureImage = c.Input("texture");
		const Colour side = c.Get<Colour>("blend_side", {255, 255, 255, 255});
		s.Side = {side.Red / 255., side.Green / 255., side.Blue / 255., side.Alpha / 255.};
		if (!SmearCurve(c, "strength_curved", "strength_curve", s.StrengthCurve) ||
			!SmearCurve(c, "spread_curved", "spread_curve", s.SpreadCurve))
			return false;
		if (s.SpreadCurve && s.Spread == 0)
			return c.Fail(
				Status::UnsupportedExecution, "source Smear spread curve divides by zero", "spread"
			);
		bool depthDefined = true, colourDefined = true, colourMathUndefined = false,
			 depthMathUndefined = false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const auto pixel = ShadeSmear(s, (x + .5) / source->Width, (y + .5) / source->Height);
				depthDefined &= pixel.DepthDefined;
				colourDefined &= pixel.ColourDefined;
				if (!SmearFinite(pixel.Colour)) {
					colourDefined = false;
					colourMathUndefined = true;
				}
				if (!std::isfinite(pixel.Depth)) {
					depthDefined = false;
					depthMathUndefined = true;
				}
			}
		if (!colourDefined &&
			!c.SetOutputDiagnostic(
				"surface_out",
				Status::UnsupportedExecution,
				colourMathUndefined ? "source Smear sample coordinates or curve math are undefined"
									: "source Smear texture reads an unwritten base position"
			))
			return false;
		if (!depthDefined &&
			!c.SetOutputDiagnostic(
				"depth_pass",
				Status::UnsupportedExecution,
				depthMathUndefined ? "source Smear depth sample math is undefined"
								   : "source Smear sweep leaves its depth out parameter unwritten"
			))
			return false;
		Image *out =
			colourDefined ? c.NewImage("surface_out", source->Width, source->Height, *format) : nullptr;
		if (colourDefined && !out) return false;
		Image *depth =
			depthDefined ? c.NewImage("depth_pass", source->Width, source->Height, *format) : nullptr;
		if (depthDefined && !depth) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const auto pixel = ShadeSmear(s, (x + .5) / source->Width, (y + .5) / source->Height);
				if (out && !WritePixel(*out, x, y, pixel.Colour))
					return c.Fail(
						Status::InvalidValue, "Smear colour exceeds numeric surface range", "surface_out"
					);
				if (depth && !WritePixel(*depth, x, y, {pixel.Depth, pixel.Depth, pixel.Depth, 1}))
					return c.Fail(
						Status::InvalidValue, "Smear depth exceeds numeric surface range", "depth_pass"
					);
			}
		if (out) FinishProcessor(c, *source, *out);
		return c.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
