// Filter family executors. Each follows its pinned source shader; the comment names the shader file.

#include "../PixelOpsChannelAssembly.hpp"
#include "../SurfaceScratch.hpp"
#include "Blur.hpp"
#include "ColorSpace.hpp"
#include "Curve.hpp"
#include "Families.hpp"
#include "Gradient.hpp"
#include "PosterizeRange.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace engine::imagegraph::detail {
	namespace {
		// Resolved input values remain alive through the complete pixel pass.
		template <class T> const T &BorrowFilterInput(const NodeContext &context, std::string_view id) {
			if (const auto *value = std::get_if<T>(context.Find(id))) return *value;
			static const T EMPTY{};
			return EMPTY;
		}

		// shaders/sh_bw: add brightness, scale by contrast, then threshold Rec. 709 luma at 0.5.
		bool Bw(NodeContext &context) {
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
					const double brightness = MappedScalar(context, "brightness", u, v);
					const double contrast = MappedScalar(context, "contrast", u, v);
					Rgba colour = ReadPixel(source, x, y);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = (colour[channel] + brightness) * contrast;
					const double luma = colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722;
					const double level = luma > 0.5 ? 1.0 : 0.0;
					return Rgba{level, level, level, colour[3]};
				}
			);
		}

		// shaders/sh_greyscale: as BW before the threshold, then Rec. 709 luma multiplied by alpha.
		bool Greyscale(NodeContext &context) {
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
					const double brightness = MappedScalar(context, "brightness", u, v);
					const double contrast = MappedScalar(context, "contrast", u, v);
					Rgba colour = ReadPixel(source, x, y);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = (colour[channel] + brightness) * contrast;
					const double luma =
						(colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722) * colour[3];
					return Rgba{luma, luma, luma, colour[3]};
				}
			);
		}

		// shaders/sh_invert: RGB only, or all four channels with Include Alpha.
		bool Invert(NodeContext &context) {
			const bool alpha = context.Boolean("include_alpha");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const Rgba colour = ReadPixel(source, x, y);
					return Rgba{
						1.0 - colour[0], 1.0 - colour[1], 1.0 - colour[2], alpha ? 1.0 - colour[3] : colour[3]
					};
				}
			);
		}

		// shaders/sh_alpha_cutoff: keep pixels whose alpha reaches Minimum, clear the rest. No Channel input.
		bool AlphaCutoff(NodeContext &context) {
			const double minimum = context.Scalar("minimum");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const Rgba colour = ReadPixel(source, x, y);
					return colour[3] >= minimum ? colour : Rgba{};
				}
			);
		}

		// shaders/sh_alpha_grey: opaque grey of alpha, optionally inverted. The pinned node passes the input
		// surface, not its Curve, to shader_set_curve, so the Curve input has no effect.
		bool AlphaGrey(NodeContext &context) {
			const bool invert = context.Boolean("invert");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const double alpha = ReadPixel(source, x, y)[3];
					const double level = invert ? 1.0 - alpha : alpha;
					return Rgba{level, level, level, 1.0};
				}
			);
		}

		// shaders/sh_flip: Axis bit 1 mirrors X, bit 2 mirrors Y, at the output texel centre.
		bool Flip(NodeContext &context) {
			const int64_t axis = context.Integer("axis");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
					if (axis == 1 || axis == 3) u = 1.0 - u;
					if (axis == 2 || axis == 3) v = 1.0 - v;
					return SampleNearest(source, u, v);
				}
			);
		}

		// shaders/sh_level: per-channel in/out ranges, then the White range over RGB. Ranges are not clamped
		// and an empty input range divides by zero as the shader does.
		bool Level(NodeContext &context) {
			const auto range = [&](std::string_view id) { return context.Vec2(id, Vector2{0.0, 1.0}); };
			const std::array<Vector2, 4> in{
				range("red_in"), range("green_in"), range("blue_in"), range("alpha_in")
			};
			const std::array<Vector2, 4> out{
				range("red_out"), range("green_out"), range("blue_out"), range("alpha_out")
			};
			const Vector2 whiteIn = range("white_in"), whiteOut = range("white_out");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					Rgba colour = ReadPixel(source, x, y);
					for (size_t channel = 0; channel < 4; channel++)
						colour[channel] = (colour[channel] - in[channel].X) /
											  (in[channel].Y - in[channel].X) *
											  (out[channel].Y - out[channel].X) +
										  out[channel].X;
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = (colour[channel] - whiteIn.X) / (whiteIn.Y - whiteIn.X) *
											  (whiteOut.Y - whiteOut.X) +
										  whiteOut.X;
					return colour;
				}
			);
		}

		// shaders/sh_ace: Narkowicz ACES filmic curve on RGB, alpha unchanged.
		bool TonemapAce(NodeContext &context) {
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					Rgba colour = ReadPixel(source, x, y);
					for (size_t channel = 0; channel < 3; channel++) {
						const double value = colour[channel];
						colour[channel] = std::clamp(
							(value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14), 0.0, 1.0
						);
					}
					return colour;
				}
			);
		}

		// shaders/sh_background: source over the Color, then opaque.
		bool Background(NodeContext &context) {
			const Colour colour = context.Get<Colour>("color", Colour{0, 0, 0, 255});
			const Rgba background{
				colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0
			};
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const Rgba foreground = ReadPixel(source, x, y);
					Rgba result{};
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] =
							background[channel] * (1.0 - foreground[3]) + foreground[channel] * foreground[3];
					result[3] = 1.0;
					return result;
				}
			);
		}

		// shaders/sh_offset: wraps the sample by the negated offset rotated by Angle. Mask and Mix only.
		bool Offset(NodeContext &context) {
			const double angle = context.Scalar("angle") * std::acos(-1.0) / 180.0;
			const double cosine = std::cos(angle), sine = std::sin(angle);
			const double scale = 1.0 / std::max(std::abs(cosine), std::abs(sine));
			const double xOffset = context.Scalar("x_offset"), yOffset = context.Scalar("y_offset");
			const double dx = scale * (xOffset * cosine - yOffset * sine);
			const double dy = scale * (xOffset * sine + yOffset * cosine);
			if (!std::isfinite(dx) || !std::isfinite(dy))
				return context.Fail(Status::InvalidValue, "offset coordinates must be finite");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
					const double sampledU = u - dx, sampledV = v - dy;
					return SampleNearest(
						source, sampledU - std::floor(sampledU), sampledV - std::floor(sampledV)
					);
				}
			);
		}

		double Luma601(const Rgba &colour) {
			return colour[0] * 0.299 + colour[1] * 0.587 + colour[2] * 0.114;
		}

		// shaders/sh_FXAA: five-tap luma edge search with filtered reads. Output 0 is the smoothed surface
		// with Mask, Mix and Channel applied; output 1 is the opaque per-channel difference.
		bool Fxaa(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			Image *out =
				context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			Image *difference =
				out ? context.NewImage("mask", source->Width, source->Height, SurfaceFormat::RGBA8Unorm)
					: nullptr;
			if (!difference) return false;
			const double amount = context.Scalar("mix_2", 1.0);
			const double texelU = 1.0 / source->Width, texelV = 1.0 / source->Height;
			const auto sample = [&](double u, double v) { return BilinearClamp(*source, u, v); };
			for (uint32_t y = 0; y < source->Height; y++) {
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
					const double corner = MappedScalar(context, "distance", u, v, true);
					const Rgba centre = sample(u, v);
					const double lumaCC = Luma601(centre);
					const double luma00 = Luma601(sample(u - corner * texelU, v - corner * texelV));
					const double luma10 = Luma601(sample(u + corner * texelU, v - corner * texelV));
					const double luma01 = Luma601(sample(u - corner * texelU, v + corner * texelV));
					const double luma11 = Luma601(sample(u + corner * texelU, v + corner * texelV));
					double dirX = (luma01 + luma11) - (luma00 + luma10);
					double dirY = (luma00 + luma01) - (luma10 + luma11);
					const double reduce =
						std::max((luma00 + luma10 + luma01 + luma11) * (1.0 / 32.0), 1.0 / 128.0);
					const double reciprocal = 1.0 / (std::min(std::abs(dirX), std::abs(dirY)) + reduce);
					dirX = std::clamp(dirX * reciprocal, -8.0, 8.0) * texelU;
					dirY = std::clamp(dirY * reciprocal, -8.0, 8.0) * texelV;
					const Rgba a0 = sample(u - dirX / 6.0, v - dirY / 6.0),
							   a1 = sample(u + dirX / 6.0, v + dirY / 6.0);
					const Rgba b0 = sample(u - dirX * 0.5, v - dirY * 0.5),
							   b1 = sample(u + dirX * 0.5, v + dirY * 0.5);
					Rgba middle{}, outer{};
					for (size_t channel = 0; channel < 4; channel++) {
						middle[channel] = 0.5 * (a0[channel] + a1[channel]);
						outer[channel] = middle[channel] * 0.5 + 0.25 * (b0[channel] + b1[channel]);
					}
					const double lumaMin =
						std::min(lumaCC, std::min(std::min(luma00, luma10), std::min(luma01, luma11)));
					const double lumaMax =
						std::max(lumaCC, std::max(std::max(luma00, luma10), std::max(luma01, luma11)));
					const double lumaB = Luma601(outer);
					const Rgba &chosen = (lumaB < lumaMin || lumaB > lumaMax) ? middle : outer;
					Rgba result{};
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] = centre[channel] + (chosen[channel] - centre[channel]) * amount;
					if (!WritePixel(*out, x, y, result))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
					if (!WritePixel(
							*difference,
							x,
							y,
							Rgba{
								std::abs(centre[0] - result[0]),
								std::abs(centre[1] - result[1]),
								std::abs(centre[2] - result[2]),
								1.0
							}
						))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
				}
			}
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		double Luma709Alpha(const Rgba &colour) {
			return (colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722) * colour[3];
		}

		// shaders/sh_normal: height gradient from Rec. 709 luma times alpha, four or eight taps, filtered
		// reads with the Oversample rule. Mix blends toward the normal; there is no Mask or Channel.
		bool Normal(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			Image *out = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!out) return false;
			const int64_t oversample = ReadSampler(context).Oversample;
			const bool ignore = context.Boolean("trim_flat"), solid = context.Boolean("solid_bg");
			const bool swapX = context.Boolean("flip_x", true), swapY = context.Boolean("flip_y");
			const bool normalize = context.Boolean("normalize", true);
			const double blend = context.Scalar("mix", 1.0);
			const double tx = 1.0 / source->Width, ty = 1.0 / source->Height;
			constexpr double ROOT2 = 1.4142135624;
			for (uint32_t y = 0; y < source->Height; y++) {
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
					const double height = MappedScalar(context, "height", u, v, true);
					const double smooth = MappedScalar(context, "smooth", u, v, true);
					Rgba centre = BilinearClamp(*source, u, v);
					const double size = 1.0 + smooth;
					if (solid) centre[3] = 1.0;
					const double cc = Luma709Alpha(centre);
					if (ignore && (cc == 0.0 || cc == 1.0)) {
						if (!WritePixel(*out, x, y, Rgba{0.5, 0.5, 1.0, centre[3]}))
							return context.Fail(
								Status::InvalidValue,
								"filter sample exceeds numeric surface range",
								"surface_out"
							);
						continue;
					}
					const auto tap = [&](double dx, double dy) {
						return Luma709Alpha(SampleTextureSimple(
							*source, u + tx * dx * size, v + ty * dy * size, oversample, true
						));
					};
					const double h0 = tap(-1, 0), h1 = tap(1, 0), v0 = tap(0, -1), v1 = tap(0, 1);
					double nx = (h1 - cc) + (cc - h0), ny = (v1 - cc) + (cc - v0);
					double wx = 1.0, wy = 1.0;
					if (smooth > 0.0) {
						const double d0 = tap(1, -1), d1 = tap(-1, -1), d2 = tap(-1, 1), d3 = tap(1, 1);
						nx += ((d0 - cc) + (cc - d1) + (cc - d2) + (d3 - cc)) / ROOT2;
						ny += ((cc - d0) + (cc - d1) + (d2 - cc) + (d3 - cc)) / ROOT2;
						wx += 4 * 0.5 * ROOT2;
						wy += 4 * 0.5 * ROOT2;
					}
					nx *= height / wx;
					ny *= height / wy;
					if (swapX) nx = -nx;
					if (swapY) ny = -ny;
					double nz = 1.0;
					if (normalize) {
						const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
						nx /= length;
						ny /= length;
						nz /= length;
					}
					const Rgba normal{0.5 + nx * 0.5, 0.5 + ny * 0.5, 0.5 + nz * 0.5, centre[3]};
					Rgba result{};
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] = centre[channel] + (normal[channel] - centre[channel]) * blend;
					if (!WritePixel(*out, x, y, result))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
				}
			}
			return context.FailureCode == Status::Ok;
		}

		// shaders/sh_gamma_map: RGB raised to 1/2.2, or 2.2 inverted.
		bool GammaMap(NodeContext &context) {
			const bool invert = context.Boolean("invert");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					Rgba colour = ReadPixel(source, x, y);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = std::pow(colour[channel], invert ? 2.2 : 1.0 / 2.2);
					return colour;
				}
			);
		}

		// shaders/sh_grey_alpha: Rec. 709 luma times alpha, through the shader curve, becomes alpha.
		bool GreyAlpha(NodeContext &context) {
			const Curve &curve = BorrowFilterInput<Curve>(context, "curve");
			const bool invert = context.Boolean("invert"), replace = context.Boolean("replace_color", true);
			const Colour tint = context.Get<Colour>("color", Colour{255, 255, 255, 255});
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const Rgba colour = ReadPixel(source, x, y);
					double level = (colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722) * colour[3];
					level = EvalShaderCurve(curve, level);
					if (invert) level = 1.0 - level;
					if (!replace) return Rgba{colour[0], colour[1], colour[2], level};
					return Rgba{
						tint.Red / 255.0, tint.Green / 255.0, tint.Blue / 255.0, level * tint.Alpha / 255.0
					};
				}
			);
		}

		// A channel sample: the colour channel, or mean RGB times alpha.
		double ChannelSample(const Rgba &colour, size_t channel, bool brightness) {
			return brightness ? (colour[0] + colour[1] + colour[2]) / 3.0 * colour[3] : colour[channel];
		}

		// shaders/sh_override_channel over input 0's size.
		bool OverrideChannel(NodeContext &context) {
			const Image *base = context.Input("surface");
			if (!base) return context.Fail(Status::InvalidValue, "image input is missing", "surface");
			const auto format = ResolveProcessorSurfaceFormat(context, base);
			if (!format) return false;
			Image *out = context.NewImage("surface_out", base->Width, base->Height, *format);
			if (!out) return false;
			const std::array<const Image *, 4> channels{
				context.Input("red"), context.Input("green"), context.Input("blue"), context.Input("alpha")
			};
			const bool brightness = context.Integer("sampling_type") == 0;
			for (uint32_t y = 0; y < out->Height; y++)
				for (uint32_t x = 0; x < out->Width; x++) {
					const double u = (x + 0.5) / out->Width, v = (y + 0.5) / out->Height;
					Rgba colour = ReadPixel(*base, x, y);
					for (size_t channel = 0; channel < 4; channel++)
						if (channels[channel])
							colour[channel] =
								ChannelSample(SampleNearest(*channels[channel], u, v), channel, brightness);
					if (!WritePixel(*out, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}

		// shaders/sh_combine_rgb, sized by the first linked of red, green and blue. Array Input needs an
		// image array, which a single-surface executor cannot receive.
		bool CombineRgb(NodeContext &context) {
			if (context.Boolean("array_input"))
				return context.Fail(
					Status::UnsupportedExecution, "array input needs an image array", "array_input"
				);
			const std::array<const Image *, 4> channels{
				context.Input("red"), context.Input("green"), context.Input("blue"), context.Input("alpha")
			};
			const Image *size = channels[0] ? channels[0] : channels[1] ? channels[1] : channels[2];
			if (!size) return context.Fail(Status::InvalidValue, "red, green or blue is required", "red");
			Image *out =
				context.NewImage("surface_out", size->Width, size->Height, SurfaceFormat::RGBA8Unorm);
			if (!out) return false;
			const bool brightness = context.Integer("sampling_type") == 1;
			for (uint32_t y = 0; y < out->Height; y++)
				for (uint32_t x = 0; x < out->Width; x++) {
					const double u = (x + 0.5) / out->Width, v = (y + 0.5) / out->Height;
					const double base = MappedScalar(context, "base_value", u, v);
					Rgba colour{base, base, base, 1.0};
					for (size_t channel = 0; channel < 4; channel++)
						if (channels[channel])
							colour[channel] =
								ChannelSample(SampleNearest(*channels[channel], u, v), channel, brightness);
					if (!WritePixel(*out, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}

		// shaders/sh_combine_hsv: channels are mean RGB times alpha; HSL goes through hsl2rgb.
		bool CombineHsv(NodeContext &context) {
			if (context.Boolean("array_input"))
				return context.Fail(
					Status::UnsupportedExecution, "array input needs an image array", "array_input"
				);
			const std::array<const Image *, 4> channels{
				context.Input("hue"),
				context.Input("saturation"),
				context.Input("value"),
				context.Input("alpha")
			};
			const Image *size = channels[0] ? channels[0] : channels[1] ? channels[1] : channels[2];
			if (!size)
				return context.Fail(Status::InvalidValue, "hue, saturation or value is required", "hue");
			Image *out =
				context.NewImage("surface_out", size->Width, size->Height, SurfaceFormat::RGBA8Unorm);
			if (!out) return false;
			const bool hsl = context.Integer("color_space") == 1;
			for (uint32_t y = 0; y < out->Height; y++)
				for (uint32_t x = 0; x < out->Width; x++) {
					const double u = (x + 0.5) / out->Width, v = (y + 0.5) / out->Height;
					std::array<double, 4> hsva{0, 0, 0, 1};
					for (size_t channel = 0; channel < 4; channel++)
						if (channels[channel])
							hsva[channel] =
								ChannelSample(SampleNearest(*channels[channel], u, v), channel, true);
					Rgba colour{};
					if (hsl) {
						if (hsva[1] == 0.0) {
							colour = {hsva[2], hsva[2], hsva[2], hsva[3]};
						} else {
							const double m2 = hsva[2] <= 0.5 ? hsva[2] * (1.0 + hsva[1])
															 : hsva[2] + hsva[1] - hsva[2] * hsva[1];
							const double m1 = 2.0 * hsva[2] - m2;
							colour = {
								HueToRgb(m1, m2, hsva[0] + 1.0 / 3.0),
								HueToRgb(m1, m2, hsva[0]),
								HueToRgb(m1, m2, hsva[0] - 1.0 / 3.0),
								hsva[3]
							};
						}
					} else {
						const Rgb3 rgb = ShaderHsvToRgb({hsva[0], hsva[1], hsva[2]});
						colour = {rgb[0], rgb[1], rgb[2], hsva[3]};
					}
					if (!WritePixel(*out, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}

		// shaders/sh_curve: per-channel curves, then the Brightness curve rescales RGB by its mean.
		bool CurveNode(NodeContext &context) {
			const Curve &white = BorrowFilterInput<Curve>(context, "brightness");
			const Curve &red = BorrowFilterInput<Curve>(context, "red");
			const Curve &green = BorrowFilterInput<Curve>(context, "green");
			const Curve &blue = BorrowFilterInput<Curve>(context, "blue");
			const Curve &alpha = BorrowFilterInput<Curve>(context, "alpha");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					Rgba colour = ReadPixel(source, x, y);
					colour[0] = EvalShaderCurve(red, colour[0]);
					colour[1] = EvalShaderCurve(green, colour[1]);
					colour[2] = EvalShaderCurve(blue, colour[2]);
					colour[3] = EvalShaderCurve(alpha, colour[3]);
					const double mean = (colour[0] + colour[1] + colour[2]) / 3.0;
					const double target = EvalShaderCurve(white, mean);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = mean == 0.0 ? target : colour[channel] * (target / mean);
					return colour;
				}
			);
		}

		// shaders/sh_curve_hsv: curves on hue, saturation and value.
		bool CurveHsv(NodeContext &context) {
			const Curve &hue = BorrowFilterInput<Curve>(context, "hue");
			const Curve &saturation = BorrowFilterInput<Curve>(context, "saturation");
			const Curve &value = BorrowFilterInput<Curve>(context, "value");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					const Rgba colour = ReadPixel(source, x, y);
					Rgb3 hsv = ShaderRgbToHsv({colour[0], colour[1], colour[2]});
					hsv = {
						EvalShaderCurve(hue, hsv[0]),
						EvalShaderCurve(saturation, hsv[1]),
						EvalShaderCurve(value, hsv[2])
					};
					const Rgb3 rgb = ShaderHsvToRgb(hsv);
					return Rgba{rgb[0], rgb[1], rgb[2], colour[3]};
				}
			);
		}

		// shaders/sh_colorize: luma plus shift, remapped by Color Range, then the gradient.
		bool Colorize(NodeContext &context) {
			const Gradient &gradient = BorrowFilterInput<Gradient>(context, "gradient");
			const GradientSampler sampler = ReadGradient(context, "gradient", gradient);
			const Vector2 range = context.Vec2("color_range", Vector2{0, 1});
			const bool loop = context.Integer("overflow") == 1;
			const bool multiplyAlpha = context.Boolean("multiply_alpha", true),
					   keepAlpha = context.Boolean("keep_alpha", true);
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
					const Rgba colour = ReadPixel(source, x, y);
					double progress = colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722 +
									  MappedScalar(context, "gradient_shift", u, v);
					progress = (progress - range.X) / (range.Y - range.X);
					progress = loop ? ShaderFract(progress) : std::clamp(progress, 0.0, 1.0);
					if (multiplyAlpha) progress *= colour[3];
					if (progress > 1.0)
						progress = progress == std::floor(progress) ? 1.0 : ShaderFract(progress);
					Rgba result = GradientEval(sampler, progress);
					if (keepAlpha) result[3] = colour[3];
					return result;
				}
			);
		}

		Rgb3 ShaderRgbToHsl(const Rgb3 &c) {
			const double low = std::min({c[0], c[1], c[2]}), high = std::max({c[0], c[1], c[2]});
			double hue = 0.0, saturation = 0.0;
			const double lightness = (high + low) / 2.0;
			if (high > low) {
				const double delta = high - low;
				saturation = lightness < 0.5 ? delta / (high + low) : delta / (2.0 - (high + low));
				if (c[0] == high)
					hue = (c[1] - c[2]) / delta;
				else if (c[1] == high)
					hue = 2.0 + (c[2] - c[0]) / delta;
				else
					hue = 4.0 + (c[0] - c[1]) / delta;
				if (hue < 0.0) hue += 6.0;
				hue /= 6.0;
			}
			return {hue, saturation, lightness};
		}

		Rgb3 ShaderRgbToLab(const Rgb3 &c) {
			Rgb3 linear{};
			for (size_t channel = 0; channel < 3; channel++)
				linear[channel] =
					c[channel] > 0.04045 ? std::pow((c[channel] + 0.055) / 1.055, 2.4) : c[channel] / 12.92;
			const Rgb3 xyz{
				100.0 * (linear[0] * 0.4124 + linear[1] * 0.3576 + linear[2] * 0.1805),
				100.0 * (linear[0] * 0.2126 + linear[1] * 0.7152 + linear[2] * 0.0722),
				100.0 * (linear[0] * 0.0193 + linear[1] * 0.1192 + linear[2] * 0.9505)
			};
			const Rgb3 white{95.047, 100.0, 108.883};
			Rgb3 f{};
			for (size_t channel = 0; channel < 3; channel++) {
				const double n = xyz[channel] / white[channel];
				f[channel] = n > 0.008856 ? std::pow(n, 1.0 / 3.0) : 7.787 * n + 16.0 / 116.0;
			}
			const Rgb3 lab{116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2])};
			return {lab[0] / 100.0, 0.5 + 0.5 * (lab[1] / 127.0), 0.5 + 0.5 * (lab[2] / 127.0)};
		}

		double Distance3(const Rgb3 &a, const Rgb3 &b) {
			return std::sqrt(
				(a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])
			);
		}

		// node_posterize.gml: nearest palette colour (sh_posterize_palette), or stepped channels
		// (sh_posterize).
		bool Posterize(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			Image *out = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!out) return false;
			const bool alpha = context.Boolean("posterize_alpha", true);
			if (context.Boolean("use_palette", true)) {
				std::array<Rgba, 256> palette{};
				size_t paletteSize = 0;
				if (const auto *colours = std::get_if<ArrayValue>(context.Find("palette")))
					for (const ElementValue &element : colours->Elements) {
						if (paletteSize == palette.size()) break;
						if (const auto *colour = std::get_if<Colour>(&element))
							palette[paletteSize++] = {
								colour->Red / 255.0,
								colour->Green / 255.0,
								colour->Blue / 255.0,
								colour->Alpha / 255.0
							};
					}
				if (!paletteSize) paletteSize = 1;
				const int64_t space = context.Integer("space");
				const double bias = context.Scalar("hue_bias");
				const Image *reference = context.Input("reference");
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++) {
						const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
						const Rgba original = ReadPixel(*source, x, y);
						Rgba colour = original;
						if (alpha)
							for (double &channel : colour)
								channel *= original[3];
						Rgb3 hsv = ShaderRgbToHsv({colour[0], colour[1], colour[2]});
						if (bias != 0.0) {
							const Rgba sample = reference ? SampleNearest(*reference, u, v) : Rgba{};
							const Rgb3 referenceHsv = ShaderRgbToHsv({sample[0], sample[1], sample[2]});
							hsv[0] += (referenceHsv[0] - hsv[0]) * bias;
							const Rgb3 rgb = ShaderHsvToRgb(hsv);
							colour = {rgb[0], rgb[1], rgb[2], colour[3]};
						}
						size_t closest = 0;
						double best = 999.0;
						for (size_t index = 0; index < paletteSize; index++) {
							const Rgba &entry = palette[index];
							const Rgb3 entryRgb{entry[0], entry[1], entry[2]},
								colourRgb{colour[0], colour[1], colour[2]};
							const Rgb3 entryHsv = ShaderRgbToHsv(entryRgb);
							double hueDifference = std::abs(hsv[0] - entryHsv[0]);
							hueDifference = std::min(hueDifference, 1.0 - hueDifference);
							hueDifference = 0.1 + (1.0 - 0.1) * hueDifference;
							double difference =
								space == 1	 ? Distance3(ShaderRgbToLab(entryRgb), ShaderRgbToLab(colourRgb))
								: space == 2 ? Distance3(ShaderRgbToHsl(entryRgb), ShaderRgbToHsl(colourRgb))
											 : Distance3(entryRgb, colourRgb);
							if (hsv[1] > 0.05) difference *= 1.0 + (hueDifference - 1.0) * bias;
							if (difference < best) {
								best = difference;
								closest = index;
							}
						}
						Rgba result = palette[closest];
						if (!alpha) result[3] = original[3];
						if (!WritePixel(*out, x, y, result))
							return context.Fail(
								Status::InvalidValue,
								"filter sample exceeds numeric surface range",
								"surface_out"
							);
					}
			} else {
				// A global range leaves the unset [1, 1, 1] minimum and [0, 0, 0] maximum, as the source
				// does.
				Rgb3 maximum{0, 0, 0}, minimum{1, 1, 1};
				if (!context.Boolean("use_global_range", true) &&
					!PosterizeLocalRange(context, *source, minimum, maximum))
					return false;
				const double steps = double(context.Integer("steps", 4));
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++) {
						const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
						const double gamma = std::max(MappedScalar(context, "gamma", u, v), 0.0001);
						const Rgba colour = ReadPixel(*source, x, y);
						Rgba result{};
						for (size_t channel = 0; channel < 3; channel++) {
							const double range = maximum[channel] - minimum[channel];
							double c = std::clamp((colour[channel] - minimum[channel]) / range, 0.0, 1.0);
							c = std::pow(c, gamma);
							c = std::floor(c * steps) / (steps - 1.0);
							c = std::pow(c, 1.0 / gamma);
							result[channel] = minimum[channel] + c * range;
						}
						result[3] = alpha ? 1.0 : colour[3];
						if (!WritePixel(*out, x, y, result))
							return context.Fail(
								Status::InvalidValue,
								"filter sample exceeds numeric surface range",
								"surface_out"
							);
					}
			}
			// Mask and Mix only; the node has no Channel input.
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		double ShaderSmoothStep(double edge0, double edge1, double x) {
			const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}

		// shaders/sh_threshold and sh_threshold_adaptive. The adaptive branch weighs a Gaussian of the
		// neighbourhood's luma; its Oversample attribute bounds the neighbourhood samples.
		bool Threshold(NodeContext &context) {
			const bool bright = context.Boolean("brightness"), adaptive = context.Integer("algorithm") == 1;
			const double brightSmooth = context.Scalar("smoothness");
			const Curve &brightCurve = BorrowFilterInput<Curve>(context, "smoothness_curve");
			const bool brightCurved = context.Boolean("smoothness_curved") && !adaptive;
			const bool brightInvert = context.Boolean("invert"), multiply = context.Boolean("multiply");
			const int64_t applyAlpha = context.Integer("apply_to_alpha");
			const bool alpha = context.Boolean("alpha"), alphaInvert = context.Boolean("invert_2");
			const double alphaSmooth = context.Scalar("smoothness_2");
			const Curve &alphaCurve = BorrowFilterInput<Curve>(context, "smoothness_curve_2");
			const bool alphaCurved = context.Boolean("smoothness_2_curved") && !adaptive;
			const double radius = double(std::min<int64_t>(context.Integer("adaptive_radius", 4), 32));
			// Radius is an integer capped at 32; GaussianKernel constructs max(1, radius) weights.
			const uint64_t kernelCount = uint64_t(std::max(1.0, radius));
			auto kernelCharge = context.ReserveWorkspace(kernelCount * sizeof(double), "adaptive_radius");
			if (!kernelCharge) return false;
			const std::vector<double> kernel = GaussianKernel(radius);
			const int64_t oversample = ReadSampler(context).Oversample;
			const auto coefficient = [&](double offset) {
				const size_t index = size_t(std::abs(offset));
				return index < kernel.size() && index < 128 ? kernel[index] : 0.0;
			};
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
					const double brightLevel = MappedScalar(context, "threshold", u, v);
					const double alphaLevel = MappedScalar(context, "threshold_2", u, v);
					const Rgba base = ReadPixel(source, x, y);
					Rgba colour = base;
					if (bright) {
						const double luma = base[0] * 0.2126 + base[1] * 0.7152 + base[2] * 0.0722;
						double level = brightLevel, result = 0.0;
						if (adaptive) {
							double neighbourhood = 0.0;
							for (double j = -radius; j <= radius; j++)
								for (double i = -radius; i <= radius; i++) {
									const Rgba sample = SampleTextureSimple(
										source, u + i / source.Width, v + j / source.Height, oversample, false
									);
									neighbourhood +=
										(sample[0] * 0.2126 + sample[1] * 0.7152 + sample[2] * 0.0722) *
										coefficient(i) * coefficient(j);
								}
							level = neighbourhood - brightLevel;
						}
						result = brightSmooth == 0.0
									 ? (luma < level ? 0.0 : 1.0)
									 : ShaderSmoothStep(level - brightSmooth, level + brightSmooth, luma);
						if (brightCurved) result = EvalShaderCurve(brightCurve, result);
						if (brightInvert) result = 1.0 - result;
						if (applyAlpha == 0)
							colour = {result, result, result, colour[3]};
						else if (applyAlpha == 1)
							colour[3] = result;
						else
							colour = {result, result, result, result};
						if (multiply)
							for (size_t channel = 0; channel < 4; channel++)
								colour[channel] *= base[channel];
					}
					if (alpha) {
						colour[3] = alphaSmooth == 0.0
										? (colour[3] < alphaLevel ? 0.0 : 1.0)
										: ShaderSmoothStep(
											  alphaLevel - alphaSmooth, alphaLevel + alphaSmooth, colour[3]
										  );
						if (alphaCurved) colour[3] = EvalShaderCurve(alphaCurve, colour[3]);
						if (alphaInvert) colour[3] = 1.0 - colour[3];
					}
					return colour;
				}
			);
		}

		// shaders/sh_palette_shift: nearest palette entry by RGB distance, moved Shift places around the
		// palette.
		bool PaletteShift(NodeContext &context) {
			std::array<Rgba, 256> palette{};
			size_t paletteSize = 0;
			if (const auto *colours = std::get_if<ArrayValue>(context.Find("palette")))
				for (const ElementValue &element : colours->Elements) {
					if (const auto *colour = std::get_if<Colour>(&element)) {
						palette[paletteSize++] = {
							colour->Red / 255.0,
							colour->Green / 255.0,
							colour->Blue / 255.0,
							colour->Alpha / 255.0
						};
						if (paletteSize == palette.size()) break;
					}
				}
			const double shift = context.Scalar("shift");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
					if (paletteSize == 0) return Rgba{};
					const Rgba colour = ReadPixel(source, x, y);
					double best = 999.0, index = 0.0;
					for (size_t entry = 0; entry < paletteSize; entry++) {
						const double distance = Distance3(
							{colour[0], colour[1], colour[2]},
							{palette[entry][0], palette[entry][1], palette[entry][2]}
						);
						if (distance < best) {
							best = distance;
							index = double(entry);
						}
					}
					const double amount = double(paletteSize);
					const double shifted = (index + shift) - amount * std::floor((index + shift) / amount);
					if (!std::isfinite(shifted) || shifted < 0.0 || shifted >= amount) {
						context.Fail(
							Status::InvalidValue,
							"palette shift must produce an index within the palette",
							"shift"
						);
						return Rgba{};
					}
					return palette[std::min(size_t(shifted), paletteSize - 1)];
				}
			);
		}

		// node_average.gml: stretch to a power-of-two square, then halve by alpha-weighted 2x2 means in
		// place, quantizing every pass, and read texel (0, 0).
		bool Average(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const uint32_t largest = std::max(source->Width, source->Height);
			const int levels = int(std::ceil(std::log2(double(largest))));
			double side = std::pow(2.0, levels);
			Rgba average{};
			if (side / 2.0 >= 1.0) {
				const auto size = uint32_t(side);
				auto current = MakeSurfaceScratch(context, size, size, SurfaceFormat::RGBA8Unorm, "average");
				if (!current) return false;
				auto next = MakeSurfaceScratch(context, size, size, SurfaceFormat::RGBA8Unorm, "average");
				if (!next) return false;
				for (uint32_t y = 0; y < size; y++)
					for (uint32_t x = 0; x < size; x++)
						if (!WritePixel(
								current->Data,
								x,
								y,
								SampleNearest(*source, (x + 0.5) / size, (y + 0.5) / size)
							))
							return context.Fail(
								Status::InvalidValue,
								"filter sample exceeds numeric surface range",
								"surface_out"
							);
				for (int pass = 0; pass <= levels; pass++) {

					for (uint32_t y = 0; y < size; y++)
						for (uint32_t x = 0; x < size; x++) {
							const double px = (x + 0.5) / size * side, py = (y + 0.5) / size * side;
							const double sx = std::floor(px / 2.0) * 2.0, sy = std::floor(py / 2.0) * 2.0;
							Rgba sum{};
							double alphaSum = 0.0;
							for (const auto &[ox, oy] :
								 {std::pair{0.0, 0.0},
								  std::pair{1.0, 0.0},
								  std::pair{0.0, 1.0},
								  std::pair{1.0, 1.0}}) {
								const Rgba texel =
									SampleNearest(current->Data, (sx + ox) / side, (sy + oy) / side);
								for (size_t channel = 0; channel < 4; channel++)
									sum[channel] += texel[channel];
								alphaSum += texel[3];
							}
							for (double &channel : sum)
								channel = alphaSum == 0.0 ? 0.0 : channel / alphaSum;
							if (!WritePixel(next->Data, x, y, sum))
								return context.Fail(
									Status::InvalidValue,
									"filter sample exceeds numeric surface range",
									"surface_out"
								);
						}
					std::swap(current, next);
					side /= 2.0;
				}
				average = ReadPixel(current->Data, 0, 0);
			} else {
				average = ReadPixel(*source, 0, 0);
			}
			const Colour colour{
				Quantize(average[0]), Quantize(average[1]), Quantize(average[2]), Quantize(average[3])
			};
			average = {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			Image *out = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!out) return false;
			for (uint32_t y = 0; y < out->Height; y++)
				for (uint32_t x = 0; x < out->Width; x++)
					if (!WritePixel(*out, x, y, average))
						return context.Fail(
							Status::InvalidValue, "filter sample exceeds numeric surface range", "surface_out"
						);
			FinishProcessor(context, *source, *out);
			context.SetValue("color", colour);
			return context.FailureCode == Status::Ok;
		}

		// shaders/sh_convolution: the kernel matrix is resized to Size by index prefix; normalization divides
		// by the kernel sum unless it is zero.
		bool Convolution(NodeContext &context) {
			const int64_t size = std::max<int64_t>(3, context.Integer("size", 3));
			if (size > 16)
				return context.Fail(Status::LimitExceeded, "the shader kernel holds 256 weights", "size");
			auto kernelCharge = context.ReserveWorkspace(uint64_t(size * size) * sizeof(double), "kernel");
			if (!kernelCharge) return false;
			std::vector<double> kernel(size_t(size * size), 0.0);
			if (const auto *matrix = std::get_if<MatrixValue>(context.Find("kernel")))
				for (size_t index = 0; index < std::min(kernel.size(), matrix->Values.size()); index++)
					kernel[index] = matrix->Values[index];
			double sum = 1.0;
			if (context.Boolean("normalize")) {
				sum = 0.0;
				for (const double weight : kernel)
					sum += weight;
				if (sum == 0.0) sum = 1.0;
			}
			const int64_t oversample = ReadSampler(context).Oversample;
			const double start = -(double(size) - 1.0) / 2.0;
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
					Rgba result{};
					for (int64_t i = 0; i < size; i++)
						for (int64_t j = 0; j < size; j++) {
							const double weight = kernel[size_t(i * size + j)];
							if (weight == 0.0) continue;
							const Rgba sample = SampleTextureSimple(
								source,
								u + (double(j) + start) / source.Width,
								v + (double(i) + start) / source.Height,
								oversample,
								false
							);
							for (size_t channel = 0; channel < 4; channel++)
								result[channel] += weight * sample[channel] / sum;
						}
					return result;
				}
			);
		}

		// sh_chromatic_aberration (Scale), sh_chromatic_aberration_cont (Continuous, spectral) and
		// sh_chromatic_aberration_grad (Gradient). Mask and Mix only; no Channel.
		bool ChromaticAberration(NodeContext &context) {
			const SamplerSettings sampler = ReadSampler(context);
			const UvMap uv = ReadUvMap(context);
			const bool filtered = Filtered(sampler);
			const int64_t type = context.Integer("type");
			const int64_t iterations = context.Integer("iteration", 1);
			const double resolution = double(context.Integer("resolution", 64));
			const Curve &curve = BorrowFilterInput<Curve>(context, "strength_curve");
			const bool curved = context.Boolean("strength_curved");
			const Gradient &gradient = BorrowFilterInput<Gradient>(context, "gradient");
			const GradientSampler tint{&gradient, nullptr, {}};
			const double gradientShift = context.Scalar("shift");
			if (type != 0 && !(resolution > 0.0))
				return context.Fail(Status::InvalidValue, "resolution must be positive", "resolution");
			return RunPixelProcessor(
				context, [&](const Image &source, uint32_t, uint32_t, double u, double v) {
					const double strength = MappedScalar(context, "strength", u, v, filtered);
					const double intensity = MappedScalar(context, "intensity", u, v, filtered);
					const double offset = MappedScalar(context, "shift_2", u, v, filtered);
					const double scale = MappedScalar(context, "scale", u, v, filtered);
					const Vector2 center = UnitVector(context, "center", source.Width, source.Height);
					const double tx = 1.0 / source.Width, ty = 1.0 / source.Height;
					const double cu = (u - center.X * tx) * 2.0, cv = (v - center.Y * ty) * 2.0;
					const auto premultiplied = [&](double su, double sv, double blend) {
						Rgba sample = SampleTextureUv(source, su, sv, blend, uv, sampler);
						for (size_t channel = 0; channel < 3; channel++)
							sample[channel] *= sample[3];
						return sample;
					};
					Rgba result{};
					if (type == 0) {
						for (int64_t index = 0; index < iterations; index++) {
							const double ratio = double(index + 1) / double(iterations);
							const double dot = cu * cu + cv * cv, amount = strength * ratio;
							const double pu = dot * cu * amount * tx, pv = dot * cv * amount * ty;
							const Rgba red = premultiplied(u - pu, v - pv, 0.5),
									   blue = premultiplied(u + pu, v + pv, 1.0);
							const Rgba centre = premultiplied(u, v, 0.0);
							const Rgba split{red[0], centre[1], blue[2], centre[3] + red[3] + blue[3]};
							for (size_t channel = 0; channel < 4; channel++)
								result[channel] +=
									(centre[channel] + (split[channel] - centre[channel]) * intensity) /
									double(iterations);
						}
						return result;
					}
					const double reach = strength / 16.0 * 0.2;
					const Rgba centre = SampleTextureUv(source, u, v, 0.0, uv, sampler);
					std::array<double, 3> sum{};
					for (float step = 0.0f; step <= 1.0f; step += 1.0f / float(resolution)) {
						const double amount = curved ? EvalShaderCurve(curve, step) : 1.0;
						const Rgba sample = premultiplied(
							u - cu * reach * step * amount, v - cv * reach * step * amount, step
						);
						std::array<double, 3> weight{};
						if (type == 1) {
							const double position = step * scale + offset;
							weight = SpectralZucconi6(position - std::floor(position));
						} else {
							double position = step * scale + offset + gradientShift;
							position = ShaderFract(ShaderFract(position) + 1.0);
							const Rgba colour = GradientEval(tint, position);
							weight = {colour[0], colour[1], colour[2]};
						}
						for (size_t channel = 0; channel < 3; channel++)
							sum[channel] += std::pow(sample[channel], 2.2) * weight[channel];
					}
					constexpr std::array<double, 3> NORMALIZE{0.386, 0.372, 0.23};
					Rgba spectral{0, 0, 0, 1.0};
					for (size_t channel = 0; channel < 3; channel++)
						spectral[channel] =
							std::pow(sum[channel] / (resolution * NORMALIZE[channel]), 1.0 / 2.2);
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] = centre[channel] + (spectral[channel] - centre[channel]) * intensity;
					return result;
				}
			);
		}

		constexpr ExecutorEntry FILTER_EXECUTORS[] = {
			{"pc.alpha_cutoff", AlphaCutoff, true},
			{"pc.alpha_grey", AlphaGrey, true},
			{"pc.average", Average, true},
			{"pc.background", Background, true},
			{"pc.bw", Bw, true},
			{"pc.chromatic_aberration", ChromaticAberration, true},
			{"pc.combine_hsv", CombineHsv, true},
			{"pc.colorize", Colorize, true},
			{"pc.combine_rgb", CombineRgb, true},
			{"pc.convolution", Convolution, true},
			{"pc.curve", CurveNode, true},
			{"pc.curve_hsv", CurveHsv, true},
			{"pc.flip", Flip, true},
			{"pc.fxaa", Fxaa, true},
			{"pc.gamma_map", GammaMap, true},
			{"pc.grey_alpha", GreyAlpha, true},
			{"pc.greyscale", Greyscale, true},
			{"pc.invert", Invert, true},
			{"pc.level", Level, true},
			{"pc.normal", Normal, true},
			{"pc.offset", Offset, true},
			{"pc.override_channel", OverrideChannel, true},
			{"pc.palette_shift", PaletteShift, true},
			{"pc.posterize", Posterize, true},
			{"pc.threshold", Threshold, true},
			{"pc.tonemap_ace", TonemapAce, true},
		};
	}

	std::span<const ExecutorEntry> FilterExecutors() {
		return FILTER_EXECUTORS;
	}
}
