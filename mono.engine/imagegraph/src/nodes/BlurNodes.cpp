// Blur family executors built on the shared Gaussian passes.

#include "../PixelOpsGradient.hpp"
#include "Blur.hpp"
#include "ColorSpace.hpp"
#include "Curve.hpp"
#include "Families.hpp"

#include <array>

namespace engine::imagegraph::detail {
	namespace {
		// Mapped Size reads its [low, high] range; the kernel length follows the larger end.
		void ReadMappedSize(const NodeContext &context, std::string_view id, GaussianArgs &args) {
			const std::string base(id);
			const Image *map = context.Input(base + "_map");
			if (map && context.Boolean(base + "_mapped")) {
				const Vector2 range = context.Vec2(base + "_map_range");
				args.Size = range.X;
				args.SizeHigh = range.Y;
				args.SizeMap = map;
			} else {
				args.Size = args.SizeHigh = context.Scalar(id);
			}
		}

		template <class T> const T &BorrowBlurInput(const NodeContext &context, std::string_view id) {
			if (const auto *value = std::get_if<T>(context.Find(id))) return *value;
			static const T EMPTY{};
			return EMPTY;
		}

		// node_blur.gml processData with sh_blur_gaussian.
		bool Blur(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			auto customKernelCharge = context.ReserveWorkspace(0, "size");
			if (!customKernelCharge) return false;
			GaussianArgs args;
			ReadMappedSize(context, "size", args);
			args.SampleMode = ReadSampler(context).Oversample;
			args.Gamma = context.Boolean("gamma_correction");
			if (context.Boolean("override_color")) {
				const Colour colour = context.Get<Colour>("color", Colour{0, 0, 0, 255});
				args.OverrideColour =
					Rgba{colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
			}
			args.Ratio = context.Scalar("aspect_ratio", 1.0);
			args.AngleRadians = context.Scalar("direction") * std::numbers::pi / 180.0;
			args.UvMap = context.Input("uv_map");
			args.UvMix = context.Scalar("uv_mix", 1.0);
			const double largest = std::max(args.Size, args.SizeHigh);
			const long double work =
				static_cast<long double>(source->Width) * source->Height * std::max(1.0, largest) * 4;
			if (!std::isfinite(largest) || work > 256'000'000)
				return context.Fail(Status::LimitExceeded, "blur size exceeds the work budget", "size");
			if (context.Integer("intensity") == 1) {
				auto customWeights = context.ReserveWorkspace(
					uint64_t(std::max(1.0, std::nearbyint(largest))) * sizeof(double), "size"
				);
				if (!customWeights) return false;
				*customKernelCharge = std::move(*customWeights);
				std::vector<double> kernel = GaussianKernel(largest);
				const Curve &curve = BorrowBlurInput<Curve>(context, "intensity_modulation");
				double total = 0.0;
				for (size_t index = 0; index < kernel.size(); index++) {
					kernel[index] = EvalCurveX(
						curve, kernel.size() == 1 ? 0.0 : double(index) / double(kernel.size() - 1)
					);
					total += kernel[index];
				}
				if (total != 0.0)
					for (double &weight : kernel)
						weight /= total;
				args.Kernel = std::move(kernel);
			}
			args.KernelLimit = largest;
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			auto blurred = GaussianBlur(context, *source, args);
			if (!blurred) return false;
			Image *out = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!out || !CopySurfaceSamples(context, blurred->Data, *out, "surface_out")) return false;
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		// sh_outline_only: the largest alpha sampled on rings of radius 1..grow around the shifted pixel,
		// white.
		bool GrowShadow(
			NodeContext &context,
			const Image &source,
			Image &grown,
			double shiftX,
			double shiftY,
			int64_t grow,
			const SamplerSettings &sampler
		) {
			for (uint32_t y = 0; y < source.Height; y++) {
				for (uint32_t x = 0; x < source.Width; x++) {
					const double px = (x + 0.5) - shiftX, py = (y + 0.5) - shiftY;
					double alpha = 0.0;
					for (double radius = 1.0; radius <= double(grow); radius++) {
						double denominator = 1.0, numerator = 0.0;
						for (int step = 0; step <= 64; step++) {
							const double angle = numerator / denominator * 2.0 * std::numbers::pi;
							numerator += 2.0;
							if (numerator >= denominator) {
								numerator = 1.0;
								denominator *= 2.0;
							}
							const Rgba sample = SampleTexture(
								source,
								(px + std::cos(angle) * radius) / source.Width,
								(py + std::sin(angle) * radius) / source.Height,
								sampler
							);
							alpha = std::max(alpha, sample[3]);
						}
					}
					if (!WritePixel(grown, x, y, Rgba{1.0, 1.0, 1.0, alpha}))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
				}
			}
			return context.FailureCode == Status::Ok;
		}

		// node_shadow.gml processData: grow or shift, blur with the shadow colour, then composite.
		bool Shadow(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const double width = source->Width, height = source->Height;
			const int64_t side = context.Integer("side");
			const Colour colour = context.Get<Colour>("color", Colour{0, 0, 0, 255});
			const Rgba shadowColour{
				colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0
			};
			const double strength = context.Scalar("strength", 0.5);
			const int64_t grow = context.Integer("grow", 3), blur = context.Integer("blur", 3);
			if (grow > 256 || blur > 256)
				return context.Fail(Status::LimitExceeded, "shadow grow or blur exceeds the work budget");
			double shiftX = 0.0, shiftY = 0.0;
			if (context.Integer("positioning") == 0) {
				const Vector2 shift = UnitVector(context, "shift", width, height);
				shiftX = shift.X;
				shiftY = shift.Y;
			} else {
				const Vector2 light = UnitVector(context, "light_position", width, height);
				shiftX = width / 2.0 - light.X;
				shiftY = height / 2.0 - light.Y;
			}
			if (!std::isfinite(shiftX) || !std::isfinite(shiftY))
				return context.Fail(Status::InvalidValue, "shadow shift must be finite", "shift");
			if (static_cast<long double>(source->Width) * source->Height *
					(std::max<int64_t>(0, grow) * 65 + std::max<int64_t>(1, blur + 1) * 4) >
				256'000'000)
				return context.Fail(Status::LimitExceeded, "shadow exceeds work budget", "grow");
			const SamplerSettings sampler = ReadSampler(context);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			auto shadow = MakeSurfaceScratch(context, source->Width, source->Height, *format, "shadow_only");
			if (!shadow) return false;
			if (grow > 0) {
				if (!GrowShadow(context, *source, shadow->Data, shiftX, shiftY, grow, sampler)) return false;
			} else {
				for (uint32_t y = 0; y < source->Height; ++y)
					for (uint32_t x = 0; x < source->Width; ++x) {
						const double sx = std::floor(x + 0.5 - shiftX), sy = std::floor(y + 0.5 - shiftY);
						if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
						if (!WritePixel(shadow->Data, x, y, ReadPixel(*source, uint32_t(sx), uint32_t(sy))))
							return context.Fail(
								Status::InvalidValue, "shadow shift exceeds numeric range", "shadow_only"
							);
					}
			}
			auto customKernelCharge = context.ReserveWorkspace(0, "size");
			if (!customKernelCharge) return false;
			GaussianArgs args;
			args.Size = args.SizeHigh = double(blur + 1);
			args.SampleMode = sampler.Oversample;
			args.OverrideColour = shadowColour;
			if (context.Boolean("strength_curved")) {
				auto customWeights = context.ReserveWorkspace(
					uint64_t(std::max(1.0, std::nearbyint(args.Size))) * sizeof(double), "size"
				);
				if (!customWeights) return false;
				*customKernelCharge = std::move(*customWeights);
				std::vector<double> kernel = GaussianKernel(args.Size);
				const Curve &curve = BorrowBlurInput<Curve>(context, "strength_curve");
				for (size_t index = 0; index < kernel.size(); index++)
					kernel[index] *= EvalCurveX(
						curve, kernel.size() == 1 ? 0.0 : double(index) / double(kernel.size() - 1)
					);
				args.Kernel = std::move(kernel);
			}
			auto blurred = GaussianBlur(context, shadow->Data, args);
			if (!blurred) return false;
			shadow.reset();
			std::optional<SurfaceScratch> inner;
			if (side == 1) {
				inner = MakeSurfaceScratch(
					context, source->Width, source->Height, SurfaceFormat::RGBA8Unorm, "shadow_only"
				);
				if (!inner || !CopySurfaceSamples(context, blurred->Data, inner->Data, "shadow_only"))
					return false;
			}
			Image *out = context.NewImage("surface_out", source->Width, source->Height, *format);
			Image *shadowOut =
				out ? context.NewImage("shadow_only", source->Width, source->Height, *format) : nullptr;
			if (!shadowOut) return false;
			const double amount = strength * shadowColour[3];
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					Rgba shade = ReadPixel(blurred->Data, x, y);
					const Rgba base = ReadPixel(*source, x, y);
					Rgba result{};
					if (side == 0) {
						for (double &channel : shade)
							channel *= amount;
						for (size_t channel = 0; channel < 4; channel++)
							result[channel] = shade[channel] * (1.0 - base[3]) + base[channel] * base[3];
					} else if (side == 1) {
						shade[3] = (1.0 - shade[3]) * amount;
						for (size_t channel = 0; channel < 4; channel++)
							result[channel] = base[channel] * (1.0 - shade[3]) + shade[channel] * shade[3];
						result[3] = base[3];
					}
					if (!WritePixel(*out, x, y, result))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
					Rgba only = ReadPixel(inner ? inner->Data : blurred->Data, x, y);
					if (side == 1) {
						// sh_shadow_inner_crop against the original.
						only[3] = (1.0 - only[3]) * base[3];
					} else if (context.Boolean("remove_original", true)) {
						// bm_subtract: destination times one minus the source, on every channel.
						for (size_t channel = 0; channel < 4; channel++)
							only[channel] *= 1.0 - base[channel];
					}
					if (!WritePixel(*shadowOut, x, y, only))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
				}
			FinishProcessor(context, *source, *out);
			auto empty = MakeSurfaceScratch(
				context, source->Width, source->Height, SurfaceFormat::RGBA8Unorm, "shadow_only"
			);
			if (!empty) return false;
			FinishProcessor(context, empty->Data, *shadowOut);
			return context.FailureCode == Status::Ok;
		}

		// A mapped scalar's [low, high] range and map, or its value twice.
		// Unit-bearing scalars scale both ends by the surface width.
		void MappedRange(
			const NodeContext &context,
			std::string_view id,
			double &low,
			double &high,
			const Image *&map,
			double width
		) {
			const std::string base(id);
			map = context.Input(base + "_map");
			if (map && context.Boolean(base + "_mapped")) {
				const Vector2 range = context.Vec2(base + "_map_range");
				low = range.X;
				high = range.Y;
			} else {
				map = nullptr;
				low = high = context.Scalar(id);
			}
			const double scale = UnitScale(context, id, width);
			low *= scale;
			high *= scale;
		}

		// node_blur_directional.gml with surface_apply_blur_directional, and its optional smoothing pass.
		bool BlurDirectional(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			DirectionalArgs args;
			MappedRange(
				context, "strength", args.Strength, args.StrengthHigh, args.StrengthMap, source->Width
			);
			MappedRange(
				context, "direction", args.Direction, args.DirectionHigh, args.DirectionMap, source->Width
			);
			args.Resolution = context.Scalar("resolution", 1.0);
			args.Single = context.Boolean("single_direction");
			args.Fade = context.Boolean("fade_distance");
			args.Gamma = context.Boolean("gamma_correction");
			args.SampleMode = ReadSampler(context).Oversample;
			args.UvMap = context.Input("uv_map");
			args.UvMix = context.Scalar("uv_mix", 1.0);
			const Curve &curve = BorrowBlurInput<Curve>(context, "strength_curve");
			if (context.Boolean("strength_curved")) args.StrengthCurve = &curve;
			args.Spectral = context.Integer("colorize");
			args.SpectralIntensity = context.Scalar("intensity", 1.0);
			args.SpectralShift = context.Scalar("shift");
			args.SpectralScale = context.Scalar("scale", 1.0);
			args.SpectralGradient = &BorrowBlurInput<Gradient>(context, "gradient");
			if (!(args.Resolution > 0.0) || !std::isfinite(args.Resolution) ||
				uint64_t(source->Width) * source->Height * std::max(source->Width, source->Height) * 2 *
						args.Resolution >
					256'000'000)
				return context.Fail(
					Status::LimitExceeded, "directional blur resolution exceeds the work budget", "resolution"
				);
			auto blurred = DirectionalBlur(context, *source, args);
			if (!blurred) return false;
			std::optional<SurfaceScratch> normalized, smoothed;
			const Image *result = &blurred->Data;
			const double smooth = context.Scalar("smooth_blur");
			if (smooth > 0.0) {
				normalized = MakeSurfaceScratch(
					context, source->Width, source->Height, SurfaceFormat::RGBA8Unorm, "smooth_blur"
				);
				if (!normalized || !CopySurfaceSamples(context, *result, normalized->Data, "smooth_blur"))
					return false;
				result = &normalized->Data;
				blurred.reset();
				args.Strength *= smooth;
				args.StrengthHigh *= smooth;
				args.Direction += 90.0;
				args.DirectionHigh += 90.0;
				smoothed = DirectionalBlur(context, normalized->Data, args);
				if (!smoothed) return false;
				result = &smoothed->Data;
			}
			Image *out = context.NewImage("surface_out", source->Width, source->Height, result->Format);
			if (!out || !CopySurfaceSamples(context, *result, *out, "surface_out")) return false;
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		// node_blur_zoom.gml with surface_apply_blur_zoom. The source passes its UV Map input as the strength
		// curve, which the shader ignores, so the Strength Curve never applies.
		bool BlurZoom(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			ZoomArgs args;
			MappedRange(
				context, "strength", args.Strength, args.StrengthHigh, args.StrengthMap, source->Width
			);
			const Vector2 center = UnitVector(context, "center", source->Width, source->Height);
			args.CenterX = center.X;
			args.CenterY = center.Y;
			args.BlurMode = context.Integer("zoom_origin", 1);
			args.SampleMode = ReadSampler(context).Oversample;
			args.Samples = context.Integer("samples", 64);
			args.Gamma = context.Boolean("gamma_correction");
			args.Fade = context.Boolean("fade");
			args.Step = context.Integer("mode") == 1;
			args.Mask = context.Input("blur_mask");
			args.UvMap = context.Input("uv_map");
			args.UvMix = context.Scalar("uv_mix", 1.0);
			args.Spectral = context.Integer("colorize");
			args.SpectralIntensity = context.Scalar("intensity", 1.0);
			args.SpectralShift = context.Scalar("shift");
			args.SpectralScale = context.Scalar("scale", 1.0);
			const Gradient &gradient = BorrowBlurInput<Gradient>(context, "gradient");
			args.SpectralGradient = &gradient;
			if (args.Samples < 1 ||
				static_cast<long double>(source->Width) * source->Height * args.Samples * 2 > 256'000'000)
				return context.Fail(
					Status::LimitExceeded, "zoom blur samples exceed the work budget", "samples"
				);
			auto blurred = ZoomBlur(context, *source, args);
			if (!blurred) return false;
			Image *out = context.NewImage("surface_out", source->Width, source->Height, source->Format);
			if (!out || !CopySurfaceSamples(context, blurred->Data, *out, "surface_out")) return false;
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		// node_bloom.gml processData: bright pass, one of three blurs, saturation and tint, additive
		// composite.
		bool Bloom(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			const double width = source->Width, height = source->Height;
			const Image *bloomMask = context.Input("bloom_mask");
			auto bright = MakeSurfaceScratch(
				context, source->Width, source->Height, SurfaceFormat::RGBA8Unorm, "bloom_mask"
			);
			if (!bright) return false;
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / width, v = (y + 0.5) / height;
					const double tolerance = MappedScalar(context, "tolerance", u, v);
					const Rgba colour = ReadPixel(*source, x, y);
					const double luma = colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722;
					Rgba result = luma > tolerance ? colour : Rgba{0, 0, 0, 1};
					if (bloomMask) {
						const Rgba mask = SampleNearest(*bloomMask, u, v);
						for (size_t channel = 0; channel < 4; channel++)
							result[channel] = colour[channel] * mask[channel];
					}
					if (!WritePixel(bright->Data, x, y, result))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
				}
			const int64_t type = context.Integer("type");
			const int64_t oversample = ReadSampler(context).Oversample;
			const Curve &curve = BorrowBlurInput<Curve>(context, "strength_curve");
			const bool curved = context.Boolean("strength_curved");
			double sizeLow = 0.0, sizeHigh = 0.0;
			const Image *sizeMap = nullptr;
			MappedRange(context, "size", sizeLow, sizeHigh, sizeMap, width);
			if (!std::isfinite(sizeLow) || !std::isfinite(sizeHigh) ||
				static_cast<long double>(width) * height *
						std::max(1.0, std::max(std::abs(sizeLow), std::abs(sizeHigh))) * 4 >
					256'000'000)
				return context.Fail(Status::LimitExceeded, "bloom size exceeds the work budget", "size");
			std::optional<SurfaceScratch> blurred;
			if (type == 0) {
				auto customKernelCharge = context.ReserveWorkspace(0, "size");
				if (!customKernelCharge) return false;
				GaussianArgs args;
				args.Size = sizeLow;
				args.SizeHigh = sizeHigh;
				args.SizeMap = sizeMap;
				args.Ratio = context.Scalar("aspect_ratio", 1.0);
				args.AngleRadians = context.Scalar("direction") * std::numbers::pi / 180.0;
				args.SampleMode = oversample;
				if (curved) {
					auto customWeights = context.ReserveWorkspace(
						uint64_t(
							std::max(1.0, std::nearbyint(std::min(1024.0, std::max(sizeLow, sizeHigh))))
						) * sizeof(double),
						"size"
					);
					if (!customWeights) return false;
					*customKernelCharge = std::move(*customWeights);
					std::vector<double> kernel =
						GaussianKernel(std::min(1024.0, std::max(sizeLow, sizeHigh)));
					for (size_t index = 0; index < kernel.size(); index++)
						kernel[index] *= EvalCurveX(
							curve, kernel.size() == 1 ? 0.0 : double(index) / double(kernel.size() - 1)
						);
					args.Kernel = std::move(kernel);
				}
				blurred = GaussianBlur(context, bright->Data, args);
			} else if (type == 1) {
				ZoomArgs args;
				args.Strength = sizeLow;
				args.StrengthHigh = sizeHigh;
				args.StrengthMap = sizeMap;
				const Vector2 origin = UnitVector(context, "zoom_origin", width, height);
				args.CenterX = origin.X;
				args.CenterY = origin.Y;
				args.BlurMode = 2;
				args.SampleMode = oversample;
				if (curved) args.StrengthCurve = &curve;
				blurred = ZoomBlur(context, bright->Data, args);
			} else {
				DirectionalArgs args;
				args.Strength = sizeLow;
				args.StrengthHigh = sizeHigh;
				args.StrengthMap = sizeMap;
				args.Direction = args.DirectionHigh = context.Scalar("direction");
				args.Fade = true;
				args.SampleMode = oversample;
				if (curved) args.StrengthCurve = &curve;
				blurred = DirectionalBlur(context, bright->Data, args);
			}
			if (!blurred) return false;
			bright.reset();
			const Colour blend = context.Get<Colour>("blend", Colour{255, 255, 255, 255});
			const Rgba tint{blend.Red / 255.0, blend.Green / 255.0, blend.Blue / 255.0, blend.Alpha / 255.0};
			const double saturation = context.Scalar("saturation", 1.0);
			Image *out =
				context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			Image *glow =
				out ? context.NewImage("bloom_mask", source->Width, source->Height, SurfaceFormat::RGBA8Unorm)
					: nullptr;
			if (!glow) return false;
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					Rgba colour = ReadPixel(blurred->Data, x, y);
					Rgb3 hsv = ShaderRgbToHsv({colour[0], colour[1], colour[2]});
					hsv[1] = std::clamp(hsv[1] * saturation, 0.0, 1.0);
					const Rgb3 rgb = ShaderHsvToRgb(hsv);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] = rgb[channel];
					for (size_t channel = 0; channel < 4; channel++)
						colour[channel] *= tint[channel];
					if (!WritePixel(*glow, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
				}
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / width, v = (y + 0.5) / height;
					const double opacity = MappedScalar(context, "strength", u, v);
					const Rgba base = ReadPixel(*source, x, y), fore = ReadPixel(*glow, x, y);
					Rgba result{};
					for (size_t channel = 0; channel < 3; channel++)
						result[channel] = base[channel] + fore[channel] * opacity;
					const double luma = fore[0] * 0.2126 + fore[1] * 0.7152 + fore[2] * 0.0722;
					result[3] = base[3] + luma * opacity;
					if (!WritePixel(*out, x, y, result))
						return context.Fail(
							Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
						);
				}
			FinishProcessor(context, *source, *out);
			return context.FailureCode == Status::Ok;
		}

		constexpr ExecutorEntry BLUR_EXECUTORS[] = {
			{"pc.bloom", Bloom, true},
			{"pc.blur", Blur, true},
			{"pc.blur_directional", BlurDirectional, true},
			{"pc.blur_zoom", BlurZoom, true},
			{"pc.shadow", Shadow, true},
		};
	}

	std::span<const ExecutorEntry> BlurExecutors() {
		return BLUR_EXECUTORS;
	}
}
