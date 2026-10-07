#include "SourcePixelMath.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t PIXEL_MATH_WORK_LIMIT = 64000000;
		struct PixelMathInputs {
			const Image *Source = nullptr, *OperandSurface = nullptr, *Mask = nullptr;
			double Feather = 0;
			std::array<float, 4> Operand{};
			int64_t Operation = 0;
			float Mix = .5f;
			bool Inactive = false;
		};
		bool PixelMathSurface(NodeContext &context, std::string_view port, const Image *surface) {
			if (!surface ||
				!ValidSurfaceLayout(*surface, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Pixel Math requires a valid surface", port);
			if (const auto *value = context.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Pixel Math raw texture binding rejects Atlas", port
				);
			return true;
		}
		bool PreparePixelMath(NodeContext &context, PixelMathInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source ||
				!ValidSurfaceLayout(*inputs.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Pixel Math requires Surface In", "surface_in");
			inputs.Inactive = !context.Boolean("active", true);
			if (context.FailureCode != Status::Ok) return false;
			if (inputs.Inactive) return true;
			if (const auto *value = context.Find("surface_in"))
				if (const auto *atlas = std::get_if<AtlasValue>(value); atlas && !ValidAtlasPayload(*atlas))
					return context.Fail(Status::InvalidValue, "Pixel Math Atlas is malformed", "surface_in");
			inputs.Mask = context.Input("mask");
			if (inputs.Mask && !PixelMathSurface(context, "mask", inputs.Mask)) return false;
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(
					Status::InvalidValue, "Pixel Math mask feather is nonfinite", "mask_feather"
				);
			inputs.Operation = context.Integer("operator");
			const int64_t operandType = context.Integer("operand_type");
			if (context.FailureCode != Status::Ok) return false;
			if (inputs.Operation < 0 || inputs.Operation > 25)
				return context.Fail(
					Status::UnsupportedExecution, "Pixel Math operator is outside its source menu", "operator"
				);
			if (operandType < 0 || operandType > 2)
				return context.Fail(
					Status::UnsupportedExecution, "Pixel Math operand type is undefined", "operand_type"
				);
			const auto value = context.Get<Vector4>("value", {});
			inputs.Operand = {float(value.X), float(value.Y), float(value.Z), float(value.W)};
			if (operandType == 2) {
				const auto colour = context.Get<Colour>("color", {0, 0, 0, 255});
				inputs.Operand = {
					colour.Red / 255.f, colour.Green / 255.f, colour.Blue / 255.f, colour.Alpha / 255.f
				};
			}
			// grug Clamp replaces the uniform even for Surface operand, which still samples its own RG
			// bounds.
			if (inputs.Operation == 15) {
				const auto range = context.Vec2("range");
				inputs.Operand = {float(range.X), float(range.Y), 0, 0};
			}
			if (operandType == 1) {
				inputs.OperandSurface = context.Input("operand_surface");
				if (!PixelMathSurface(context, "operand_surface", inputs.OperandSurface)) return false;
			}
			inputs.Mix = float(context.Scalar("mix_2", .5));
			for (float operand : inputs.Operand)
				if (!std::isfinite(operand))
					return context.Fail(
						Status::InvalidValue, "Pixel Math operand exceeds finite shader range", "value"
					);
			if (!std::isfinite(inputs.Mix))
				return context.Fail(
					Status::InvalidValue, "Pixel Math mix exceeds finite shader range", "mix_2"
				);
			return context.FailureCode == Status::Ok;
		}
		bool QuotePixelMath(NodeContext &context, const PixelMathInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			long double rowWork = pixels * (inputs.Inactive ? 1 : 32);
			if (!inputs.Inactive && inputs.Mask && inputs.Feather > 0) {
				const double radius = std::max(1., std::round(inputs.Feather));
				if (radius > std::numeric_limits<int>::max())
					return context.Fail(
						Status::LimitExceeded,
						"Pixel Math mask feather exceeds supported radius",
						"mask_feather"
					);
				// grug count both full-mask passes, their taps, copies and weight setup.
				rowWork += static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
							   (2 * (2 * radius - 1) * 16 + 2) +
						   radius * 16;
			}
			if (work > PIXEL_MATH_WORK_LIMIT || rowWork > PIXEL_MATH_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "Pixel Math complete batch exceeds work limit", "surface_out"
				);
			work += uint64_t(rowWork);
			return true;
		}
		bool PixelMathChannel(
			NodeContext &context,
			int64_t operation,
			float source,
			float operand,
			float lower,
			float upper,
			float mix,
			float &result
		) {
			if (!std::isfinite(source) || !std::isfinite(operand))
				return context.Fail(Status::InvalidValue, "Pixel Math sample is nonfinite", "surface_in");
			if (((operation == 3 || operation == 5 || operation == 9 || operation == 16) && operand == 0) ||
				((operation == 4 || operation == 5) &&
				 (source < 0 || (source == 0 && (operation == 4 ? operand : 1 / operand) <= 0))) ||
				(operation == 15 && lower > upper))
				return context.Fail(
					Status::UnsupportedExecution, "Pixel Math shader arithmetic is undefined", "operator"
				);
			switch (operation) {
			case 0:
				result = source + operand;
				break;
			case 1:
				result = source - operand;
				break;
			case 2:
				result = source * operand;
				break;
			case 3:
				result = source / operand;
				break;
			case 4:
				result = std::pow(source, operand);
				break;
			case 5:
				result = std::pow(source, 1.f / operand);
				break;
			case 6:
				result = std::sin(source);
				break;
			case 7:
				result = std::cos(source);
				break;
			case 8:
				result = std::tan(source);
				break;
			case 9:
				result = source - operand * std::floor(source / operand);
				break;
			case 10:
				result = std::floor(source);
				break;
			case 11:
				result = std::ceil(source);
				break;
			case 12:
				result = std::floor(source + .5f);
				break;
			case 13:
				result = source * (1 - mix) + operand * mix;
				break;
			case 14:
				result = std::abs(source);
				break;
			case 15:
				result = std::clamp(source, lower, upper);
				break;
			case 16:
				result = std::floor(source / operand) * operand;
				break;
			case 17:
				result = source - std::floor(source);
				break;
			case 18:
				result = source < operand ? 1 : 0;
				break;
			case 19:
				result = source <= operand ? 1 : 0;
				break;
			case 20:
				result = source > operand ? 1 : 0;
				break;
			case 21:
				result = source >= operand ? 1 : 0;
				break;
			default:
				result = source;
				break;
			}
			return std::isfinite(result) ||
				   context.Fail(
					   Status::InvalidValue, "Pixel Math result exceeds finite shader range", "surface_out"
				   );
		}
		bool DrawPixelMath(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.pixel_math");
			PixelMathInputs inputs;
			uint64_t work = 0;
			if (!PreparePixelMath(context, inputs) || !QuotePixelMath(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			Image *output =
				context.NewImage("surface_out", inputs.Source->Width, inputs.Source->Height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < output->Height; ++y)
				for (uint32_t x = 0; x < output->Width; ++x) {
					const auto source = ReadPixel(*inputs.Source, x, y);
					auto operand = inputs.Operand;
					if (inputs.OperandSurface) {
						const auto sample = Texture(
							*inputs.OperandSurface, (x + .5) / output->Width, (y + .5) / output->Height, false
						);
						for (size_t c = 0; c < 4; ++c)
							operand[c] = float(sample[c]);
					}
					Rgba colour{};
					for (size_t c = 0; c < 4; ++c) {
						float result = 0;
						if (inputs.Operation >= 18 && inputs.Operation <= 21 && c == 3)
							result = 1;
						else if (!PixelMathChannel(
									 context,
									 inputs.Operation,
									 float(source[c]),
									 operand[c],
									 operand[0],
									 operand[1],
									 inputs.Mix,
									 result
								 ))
							return false;
						colour[c] = result;
					}
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Pixel Math output storage is nonfinite", "surface_out"
						);
				}
			FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePixelMath(NodeContext &context, uint64_t &batchWork) {
		PixelMathInputs inputs;
		return PreparePixelMath(context, inputs) && QuotePixelMath(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourcePixelMathExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.pixel_math", DrawPixelMath, true}};
		return entries;
	}
}
