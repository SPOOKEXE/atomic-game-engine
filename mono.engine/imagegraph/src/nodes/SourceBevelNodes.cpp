#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceMappedInputs.hpp"
#include "SourceNormalShaderCurve.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		using BevelPixel = std::array<float, 4>;
		BevelPixel BevelSample(const Image &source, float u, float v, bool filtered) {
			const auto c = Texture(source, u, v, filtered);
			return {float(c[0]), float(c[1]), float(c[2]), float(c[3])};
		}
		float BevelBrightness(const BevelPixel &c) {
			return (c[0] + c[1] + c[2]) / 3 * c[3];
		}
		bool BevelWork(NodeContext &context, uint64_t pixels, double steps, uint64_t directions) {
			const uint64_t limit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			if (!std::isfinite(steps) || steps > double(limit / directions) ||
				pixels > limit / (1 + uint64_t(std::max(steps, 0.)) * directions))
				return context.Fail(
					Status::LimitExceeded, "bevel exceeds bounded aggregate sample work", "height"
				);
			return true;
		}
		bool SourceBevel(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source)
				return context.Fail(Status::TypeMismatch, "bevel requires a source surface", "surface_in");
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			const auto sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const bool filtered = Filtered(sampler), high = context.Boolean("highres"),
					   rangeMapped = context.Boolean("height_mapped");
			const bool mapped = rangeMapped && context.Input("height_map");
			Vector2 range;
			if (rangeMapped) {
				// The shared getter rounds only flat Int input; nested rows keep source fractions.
				if (!ReadSourceMappedRange(context, "height", range)) return false;
			} else {
				const double value = context.Scalar("height", 4);
				range = {value, value};
			}
			if (!std::isfinite(float(range.X)) || !std::isfinite(float(range.Y)))
				return context.Fail(
					Status::UnsupportedExecution, "bevel height exceeds finite shader controls", "height"
				);
			const float maximum = float(std::max(range.X, range.Y));
			const double steps = std::ceil(std::max(0.f, maximum) * (high ? 8 : 4));
			if (!BevelWork(context, uint64_t(source->Width) * source->Height, steps, high ? 513 : 65))
				return false;
			const auto scale = context.Vec2("scale", {1, 1}),
					   shift = UnitVector(context, "shift", source->Width, source->Height);
			if (!std::isfinite(float(scale.X)) || !std::isfinite(float(scale.Y)) || scale.X == 0 ||
				scale.Y == 0 || !std::isfinite(float(shift.X)) || !std::isfinite(float(shift.Y)))
				return context.Fail(
					Status::UnsupportedExecution, "source bevel requires finite nonzero scale", "scale"
				);
			const bool shifted = context.Boolean("shift_multiply"), curved = context.Boolean("slope_curved");
			const Curve *curve =
				context.Find("slope_curve") ? std::get_if<Curve>(context.Find("slope_curve")) : nullptr;
			if (curved && (!curve || curve->Anchors.size() > 9 || curve->Header[1] == 0))
				return context.Fail(
					Status::UnsupportedExecution,
					"bevel curve exceeds defined GLSL curve profile",
					"slope_curve"
				);
			const int64_t slope = context.Integer("slope");
			const float shiftX = -float(shift.X / source->Width), shiftY = -float(shift.Y / source->Height),
						angle = std::atan2(shiftY, shiftX), distance = std::hypot(shiftX, shiftY);
			Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < output->Height; y++)
				for (uint32_t x = 0; x < output->Width; x++) {
					const float u = float((x + .5) / output->Width), v = float((y + .5) / output->Height);
					const auto colour = BevelSample(*source, u, v, filtered);
					const float b0 = BevelBrightness(colour);
					float hei = float(range.X);
					if (mapped) {
						const auto map = BevelSample(*context.Input("height_map"), u, v, filtered);
						hei += float(range.Y - range.X) * (map[0] + map[1] + map[2]) / 3;
					}
					float b1 = b0, slopeDistance = hei * b0, maxDistance = hei;
					if (b0 != 0)
						for (float i = 0; i < maximum; i += high ? .125f : .25f) {
							if (i > hei) break;
							float base = 1, top = 0;
							for (int j = 0; j <= (high ? 512 : 64); j++) {
								const float a = top / base * float(2 * std::numbers::pi);
								top += 2;
								if (top >= base) {
									top = 1;
									base *= 2;
								}
								float dx = std::cos(a), dy = std::sin(a);
								if (!shifted) {
									dx += shiftX;
									dy += shiftY;
								}
								const float r = high ? i : std::floor(i + .5f);
								dx *= r;
								dy *= r;
								if (shifted) {
									const float m = 1 + std::cos(std::abs(angle - a)) * distance;
									dx *= m;
									dy *= m;
								}
								dx /= float(scale.X);
								dy /= float(scale.Y);
								const auto sample = SampleTextureSimple(
									*source,
									u + dx / source->Width,
									v + dy / source->Height,
									sampler.Oversample,
									filtered
								);
								const float b = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3 *
												float(sample[3]);
								if (b < b1) {
									slopeDistance = std::min(slopeDistance, r);
									maxDistance = std::min(maxDistance, (b0 - b) * hei);
									b1 = std::min(b1, b);
									i = hei;
									break;
								}
							}
						}
					BevelPixel result = colour;
					if (b0 != 0) {
						float progress = b0;
						if (maxDistance != 0) {
							float m = slopeDistance / maxDistance;
							if (slope == 1)
								m = std::pow(m, 3.f) + 3 * m * m * (1 - m);
							else if (slope == 2)
								m = std::sqrt(1 - std::pow(m - 1, 2.f));
							if (curved) m *= NormalEvalShaderCurve(*curve, m);
							m = std::clamp(m, 0.f, 1.f);
							progress = b1 + (b0 - b1) * m;
						}
						result = {progress, progress, progress, colour[3]};
					}
					if (!WritePixel(*output, x, y, {result[0], result[1], result[2], result[3]}))
						return context.Fail(
							Status::UnsupportedExecution,
							"source bevel produced nonfinite shader math",
							"surface_out"
						);
				}
			FinishProcessor(context, *source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceBevelExecutors() {
		static const ExecutorEntry entries[] = {{"pc.bevel", SourceBevel, true}};
		return entries;
	}
}
