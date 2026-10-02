// CPU gradient objects retain all 128 keys. The shader sampler has a separate 64-slot contract.

#include "Families.hpp"
#include "Gradient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace engine::imagegraph::detail {
	namespace {
		uint8_t Byte(double value) {
			value = std::clamp(value, 0.0, 255.0);
			const double lower = std::floor(value), fraction = value - lower;
			return static_cast<uint8_t>(
				lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0))
			);
		}

		bool ReserveGradientOutput(NodeContext &context, size_t count, size_t stride, std::string_view port) {
			const uint64_t nameBytes = std::max<uint64_t>(port.size(), std::string{}.capacity());
			if (count > Limits::MaximumArrayElements || nameBytes > Limits::MaximumArrayBytes ||
				(stride && count > (Limits::MaximumArrayBytes - nameBytes) / stride))
				return context.Fail(Status::LimitExceeded, "gradient exceeds evaluation byte budget", port);
			return context.ReserveOutput(uint64_t(count) * stride + nameBytes, port);
		}

		const Gradient *ReadKeys(NodeContext &context) {
			const Value *value = context.Find("gradient");
			const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
			if (!gradient || gradient->Mode > 6) {
				context.Fail(
					Status::InvalidValue, "gradient requires a valid interpolation mode", "gradient"
				);
				return nullptr;
			}
			if (gradient->Keys.size() > Limits::MaximumGradientKeys) {
				context.Fail(Status::LimitExceeded, "gradient exceeds 128 keys", "gradient");
				return nullptr;
			}
			for (const engine::imagegraph::GradientKey &key : gradient->Keys)
				if (!std::isfinite(key.Time)) {
					context.Fail(Status::InvalidValue, "gradient key times must be finite", "gradient");
					return nullptr;
				}
			return gradient;
		}

		bool
		ReadArray(NodeContext &context, std::string_view port, ValueType type, const ArrayValue *&array) {
			const Value *value = context.Find(port);
			array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array || array->ElementType != type || !array->Nested.empty())
				return context.Fail(Status::InvalidValue, "gradient requires a flat typed array", port);
			if (array->Elements.size() > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "gradient input exceeds array budget", port);
			for (const ElementValue &element : array->Elements) {
				if (type == ValueType::Colour && !std::holds_alternative<Colour>(element))
					return context.Fail(Status::InvalidValue, "palette requires colours", port);
				if (type == ValueType::Scalar) {
					const auto *number = std::get_if<double>(&element);
					if (!number || !std::isfinite(*number))
						return context.Fail(Status::InvalidValue, "positions require finite scalars", port);
				}
			}
			return true;
		}

		Rgb3 CpuOklabMix(const Rgb3 &a, const Rgb3 &b, double amount) {
			constexpr std::array<Rgb3, 3> TO{
				{{.4121656120, .2118591070, .0883097947},
				 {.5362752080, .6807189584, .2818474174},
				 {.0514575653, .1074065790, .6302613616}}
			};
			constexpr std::array<Rgb3, 3> FROM{
				{{4.0767245293, -1.2681437731, -.0041119885},
				 {-3.3072168827, 2.6093323231, -.7034763098},
				 {.2307590544, -.3411344290, 1.7068625689}}
			};
			Rgb3 mixed{}, result{};
			for (size_t row = 0; row < 3; row++) {
				double x = 0, y = 0;
				for (size_t channel = 0; channel < 3; channel++) {
					x += TO[row][channel] * std::pow(a[channel], 2.2);
					y += TO[row][channel] * std::pow(b[channel], 2.2);
				}
				mixed[row] = std::pow(x, 1.0 / 3) + (std::pow(y, 1.0 / 3) - std::pow(x, 1.0 / 3)) * amount;
			}
			for (size_t row = 0; row < 3; row++) {
				double linear = 0;
				for (size_t channel = 0; channel < 3; channel++)
					linear += FROM[row][channel] * std::pow(mixed[channel], 3);
				result[row] = std::pow(std::max(0.0, linear), 1.0 / 2.2);
			}
			return result;
		}

		bool Sample(NodeContext &context, const Gradient &gradient, double position, Colour &output) {
			if (!std::isfinite(position))
				return context.Fail(Status::InvalidValue, "gradient sample must be finite", "sample");
			const auto &keys = gradient.Keys;
			if (keys.empty()) {
				output = {0, 0, 0, 0};
				return true;
			}
			if (keys.size() == 1 || position <= keys.front().Time) {
				output = keys.front().Color;
				return true;
			}
			if (position >= keys.back().Time) {
				output = keys.back().Color;
				return true;
			}
			for (size_t index = 1; index < keys.size(); index++) {
				const auto &before = keys[index - 1], &after = keys[index];
				if (after.Time < position) continue;
				if (after.Time == position) {
					output = after.Color;
					return true;
				}
				if (gradient.Mode == 1) {
					output = before.Color;
					return true;
				}
				const double amount = (position - before.Time) / (after.Time - before.Time);
				if (!std::isfinite(amount))
					return context.Fail(
						Status::InvalidValue, "gradient interpolation is not finite", "gradient"
					);
				const Colour &a = before.Color, &b = after.Color;
				Rgb3 c0{a.Red / 255.0, a.Green / 255.0, a.Blue / 255.0};
				Rgb3 c1{b.Red / 255.0, b.Green / 255.0, b.Blue / 255.0};
				Rgb3 mixed;
				if (gradient.Mode == 2 || gradient.Mode == 5) {
					// GML hue/saturation/value getters quantize to bytes before interpolation.
					auto h0 = ShaderRgbToHsv(c0), h1 = ShaderRgbToHsv(c1);
					for (size_t channel = 0; channel < 3; channel++) {
						h0[channel] = Byte(h0[channel] * 255) / 255.0;
						h1[channel] = Byte(h1[channel] * 255) / 255.0;
					}
					// GML frac preserves the sign of its input, unlike GLSL fract.
					const double delta = std::fmod(h1[0] - h0[0], 1.0);
					double distance = std::fmod(2 * delta, 1.0) - delta;
					if (gradient.Mode == 5) distance -= (distance > 0) - (distance < 0);
					mixed = ShaderHsvToRgb(
						{Byte((h0[0] + distance * amount) * 255) / 255.0,
						 Byte((h0[1] + (h1[1] - h0[1]) * amount) * 255) / 255.0,
						 Byte((h0[2] + (h1[2] - h0[2]) * amount) * 255) / 255.0}
					);
				} else if (gradient.Mode == 3)
					mixed = CpuOklabMix(c0, c1, amount);
				else
					mixed = GradientMix(c0, c1, amount, gradient.Mode);
				for (double value : mixed)
					if (!std::isfinite(value))
						return context.Fail(
							Status::InvalidValue, "gradient colour blend is not finite", "gradient"
						);
				uint8_t alpha = Byte(a.Alpha + (b.Alpha - a.Alpha) * amount);
				// The pinned Oklab and CMYK helpers multiply an already-byte alpha by 255.
				if (gradient.Mode == 3 || gradient.Mode == 6)
					alpha = static_cast<uint8_t>(uint32_t(alpha) * 255);
				output = {Byte(mixed[0] * 255), Byte(mixed[1] * 255), Byte(mixed[2] * 255), alpha};
				return true;
			}
			output = keys.back().Color;
			return true;
		}

		bool GradientOut(NodeContext &context) {
			const Gradient *gradient = ReadKeys(context);
			if (!gradient) return false;
			Colour colour;
			if (!Sample(context, *gradient, context.Scalar("sample"), colour)) return false;
			if (!ReserveGradientOutput(context, gradient->Keys.size(), sizeof(engine::imagegraph::GradientKey), "gradient") ||
				!ReserveGradientOutput(context, 0, 0, "color"))
				return false;
			context.SetValue("gradient", *gradient);
			context.SetValue("color", colour);
			return true;
		}

		bool GradientExtract(NodeContext &context) {
			const Gradient *gradient = ReadKeys(context);
			if (!gradient ||
				!ReserveGradientOutput(context, gradient->Keys.size(), 2 * sizeof(ElementValue), "colors") ||
				!ReserveGradientOutput(context, 0, 0, "positions") ||
				!ReserveGradientOutput(context, 0, 0, "type"))
				return false;
			ArrayValue colours{ValueType::Colour, {}}, positions{ValueType::Scalar, {}};
			colours.Elements.reserve(gradient->Keys.size());
			positions.Elements.reserve(gradient->Keys.size());
			for (const engine::imagegraph::GradientKey &key : gradient->Keys) {
				colours.Elements.emplace_back(key.Color);
				positions.Elements.emplace_back(key.Time);
			}
			context.SetValue("colors", std::move(colours));
			context.SetValue("positions", std::move(positions));
			context.SetValue("type", int64_t(gradient->Mode));
			return true;
		}

		bool GradientPalette(NodeContext &context) {
			const ArrayValue *palette = nullptr, *positions = nullptr;
			if (!ReadArray(context, "palette", ValueType::Colour, palette)) return false;
			const bool custom = context.Boolean("custom_positions");
			if (custom && !ReadArray(context, "positions", ValueType::Scalar, positions)) return false;
			const double mode = context.SourceChoice("interpolation", 1);
			if (context.FailureCode != Status::Ok) return false;
			const size_t count = std::min(palette->Elements.size(), Limits::MaximumGradientKeys);
			if (!ReserveGradientOutput(context, count, sizeof(engine::imagegraph::GradientKey), "gradient"))
				return false;
			Gradient gradient;
			gradient.Mode = mode == 0 ? 1 : mode == 1 ? 0 : mode == 2 ? 2 : mode == 3 ? 3 : mode == 4 ? 4 : 0;
			gradient.Keys.reserve(count);
			// The source checks the incoming enum before remapping it, so RGB (1) uses n-1.
			const double step = mode == 1 ? (count > 1 ? 1.0 / (count - 1) : 0) : (count ? 1.0 / count : 0);
			for (size_t index = 0; index < count; index++) {
				const double position = custom ? (index < positions->Elements.size()
													  ? std::get<double>(positions->Elements[index])
													  : 0)
											   : index * step;
				gradient.Keys.push_back({position, std::get<Colour>(palette->Elements[index])});
			}
			if (!custom && mode == 1 && count == 1)
				return context.Fail(
					Status::InvalidValue,
					"source singleton RGB palette yields a non-finite key time",
					"palette"
				);
			context.SetValue("gradient", std::move(gradient));
			return true;
		}

		bool GradientSample(NodeContext &context) {
			const Gradient *gradient = ReadKeys(context);
			if (!gradient) return false;
			const double shift = context.Scalar("shift");
			const double mode = context.SourceChoice("type");
			if (context.FailureCode != Status::Ok) return false;
			if (mode != 0 && mode != 1)
				return context.Fail(
					Status::UnsupportedExecution,
					"fractional sample choice retains source history not represented natively",
					"type"
				);
			if (!std::isfinite(shift) || mode < 0 || mode > 1)
				return context.Fail(
					Status::InvalidValue, "gradient sample mode or shift is invalid", "shift"
				);
			const ArrayValue *ratios = nullptr;
			double ratio = 0;
			size_t count;
			if (mode == 0) {
				const int64_t steps = context.Integer("step", 16);
				if (steps < 0 || uint64_t(steps) > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "gradient sample step exceeds array budget", "step"
					);
				count = static_cast<size_t>(steps);
			} else if (const Value *value = context.Find("ratio");
					   value && std::holds_alternative<ArrayValue>(*value)) {
				if (!ReadArray(context, "ratio", ValueType::Scalar, ratios)) return false;
				count = ratios->Elements.size();
			} else {
				ratio = context.Scalar("ratio");
				if (!std::isfinite(ratio))
					return context.Fail(Status::InvalidValue, "gradient ratio must be finite", "ratio");
				count = 1;
			}
			if (!ReserveGradientOutput(context, count, sizeof(ElementValue), "colors")) return false;
			ArrayValue colours{ValueType::Colour, {}};
			colours.Elements.reserve(count);
			for (size_t index = 0; index < count; index++) {
				const double progress = mode == 0 ? double(index) / count
										: ratios  ? std::get<double>(ratios->Elements[index])
												  : ratio;
				Colour colour;
				if (!Sample(context, *gradient, std::fmod(shift + progress, 1.0), colour)) return false;
				colours.Elements.emplace_back(colour);
			}
			context.SetValue("colors", std::move(colours));
			return true;
		}

		bool GradientShift(NodeContext &context) {
			const Gradient *source = ReadKeys(context);
			if (!source) return false;
			const double shift = context.Scalar("shift"), scale = context.Scalar("scale", 1);
			if (!std::isfinite(shift) || !std::isfinite(scale))
				return context.Fail(Status::InvalidValue, "gradient shift and scale must be finite", "shift");
			if (!ReserveGradientOutput(context, source->Keys.size(), sizeof(engine::imagegraph::GradientKey), "gradient"))
				return false;
			Gradient result;
			result.Mode = source->Mode;
			result.Keys.reserve(source->Keys.size());
			for (const engine::imagegraph::GradientKey &key : source->Keys) {
				double time = .5 + (key.Time - .5) * scale + shift;
				if (!std::isfinite(time))
					return context.Fail(
						Status::InvalidValue, "gradient transformed time is not finite", "scale"
					);
				if (context.Boolean("wrap")) {
					time = std::fmod(time, 1.0);
					if (time < 0) time += 1;
				}
				const auto found = std::lower_bound(
					result.Keys.begin(),
					result.Keys.end(),
					time,
					[](const engine::imagegraph::GradientKey &item, double target) {
						return item.Time < target;
					}
				);
				if (found != result.Keys.end() && found->Time == time)
					found->Color = key.Color;
				else
					result.Keys.insert(found, {time, key.Color});
			}
			context.SetValue("gradient", std::move(result));
			return true;
		}

		bool GradientReplace(NodeContext &context) {
			const Gradient *gradient = ReadKeys(context);
			const ArrayValue *from = nullptr, *to = nullptr;
			if (!gradient || !ReadArray(context, "color_from", ValueType::Colour, from) ||
				!ReadArray(context, "color_to", ValueType::Colour, to))
				return false;
			const double threshold = context.Scalar("threshold", .1);
			if (!std::isfinite(threshold))
				return context.Fail(Status::InvalidValue, "gradient threshold must be finite", "threshold");
			const size_t outputCount = std::max<size_t>(gradient->Keys.size(), 1);
			if (!ReserveGradientOutput(context, outputCount, sizeof(engine::imagegraph::GradientKey), "gradient"))
				return false;
			Gradient result = *gradient;
			if (result.Keys.empty()) {
				result.Keys.push_back({0, {0, 0, 0, 255}});
				context.SetValue("gradient", std::move(result));
				return true;
			}
			for (engine::imagegraph::GradientKey &key : result.Keys) {
				double closest = 999;
				size_t match = from->Elements.size();
				for (size_t index = 0; index < from->Elements.size(); index++) {
					const Colour &candidate = std::get<Colour>(from->Elements[index]);
					const double red = (double(key.Color.Red) - candidate.Red) / 255;
					const double green = (double(key.Color.Green) - candidate.Green) / 255;
					const double blue = (double(key.Color.Blue) - candidate.Blue) / 255;
					const double distance = std::sqrt(red * red + green * green + blue * blue);
					if (distance <= threshold && distance < closest) {
						closest = distance;
						match = index;
					}
				}
				if (match < from->Elements.size() && !to->Elements.empty())
					key.Color = std::get<Colour>(to->Elements[match % to->Elements.size()]);
			}
			context.SetValue("gradient", std::move(result));
			return true;
		}
	}

	std::span<const ExecutorEntry> GradientExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.gradient_out", GradientOut},
			ExecutorEntry{"pc.gradient_palette", GradientPalette},
			ExecutorEntry{"pc.gradient_extract", GradientExtract},
			ExecutorEntry{"pc.gradient_sample", GradientSample},
			ExecutorEntry{"pc.gradient_shift", GradientShift},
			ExecutorEntry{"pc.gradient_replace_color", GradientReplace}
		};
		return ENTRIES;
	}
}
