#include "ColorSpace.hpp"
#include "Processor.hpp"

#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		Rgb3 MatrixColour(const std::array<double, 9> &matrix, const Rgb3 &colour) {
			Rgb3 transformed{};
			for (size_t output = 0; output < 3; ++output)
				for (size_t input = 0; input < 3; ++input)
					transformed[output] += matrix[input * 3 + output] * colour[input];
			return transformed;
		}

		Rgb3 QuantizeSpace(Rgb3 colour, int64_t space, bool inverse) {
			if (space == 1) return inverse ? ShaderHsvToRgb(colour) : ShaderRgbToHsv(colour);
			if (space == 2) {
				static constexpr std::array<double, 9> TO_CONE{
					0.4121656120,
					0.2118591070,
					0.0883097947,
					0.5362752080,
					0.6807189584,
					0.2818474174,
					0.0514575653,
					0.1074065790,
					0.6302613616
				};
				static constexpr std::array<double, 9> FROM_CONE{
					4.0767245293,
					-1.2681437731,
					-0.0041119885,
					-3.3072168827,
					2.6093323231,
					-0.7034763098,
					0.2307590544,
					-0.3411344290,
					1.7068625689
				};
				if (!inverse) {
					for (double &channel : colour)
						channel = std::pow(channel, 2.2);
					colour = MatrixColour(TO_CONE, colour);
					for (double &channel : colour)
						channel = std::pow(channel, 1.0 / 3.0);
				} else {
					for (double &channel : colour)
						channel = channel * channel * channel;
					colour = MatrixColour(FROM_CONE, colour);
					for (double &channel : colour)
						channel = std::pow(channel, 1.0 / 2.2);
				}
				return colour;
			}
			if (space == 3) {
				static constexpr std::array<double, 9> TO_YIQ{
					0.30, 0.59, 0.11, 0.599, -0.2773, -0.3217, 0.213, -0.5251, 0.3121
				};
				static constexpr std::array<double, 9> FROM_YIQ{
					1, 0.9496, 0.6236, 1, -0.2748, -0.6357, 1, -1.1000, 1.7000
				};
				return MatrixColour(inverse ? FROM_YIQ : TO_YIQ, colour);
			}
			return colour;
		}

		uint32_t BayerRank(uint32_t x, uint32_t y, uint32_t side) {
			static constexpr std::array<uint32_t, 4> BASE{0, 2, 3, 1};
			if (side == 2) return BASE[(y % 2) * 2 + x % 2];
			const uint32_t half = side / 2;
			return 4 * BayerRank(x % half, y % half, half) + BASE[(y / half) * 2 + x / half];
		}

		double QuantizedChannel(double channel, double steps, bool dithering, double contrast, double rank) {
			if (steps <= 1) return std::floor(channel + 0.5);
			const double quantized = std::floor(channel * steps) / (steps - 1);
			if (std::abs(channel - quantized) < 0.001 || !dithering) return quantized;
			const double interval = 1.0 / (steps - 1), base = std::floor(channel * steps + 0.5) / steps;
			const double difference = channel - base;
			const double ratio = 1.0 - std::min(
										   {std::abs(difference),
											std::abs(channel - base + interval),
											std::abs(channel - base - interval)}
									   ) * steps *
										   2.0 * contrast;
			if (difference > 0) return ratio * 0.5 >= rank ? quantized - interval : quantized;
			return ratio * 0.5 <= rank ? quantized : quantized + interval;
		}
	}

	bool BitReduce(NodeContext &context) {
		const int64_t space = context.Integer("color_space"), pattern = context.Integer("pattern");
		if (space < 0 || space > 3)
			return context.Fail(Status::InvalidValue, "Color Space choice is invalid", "color_space");
		if (pattern < 0 || pattern > 2)
			return context.Fail(Status::InvalidValue, "Dithering pattern is invalid", "pattern");
		const Vector3 steps = context.Get<Vector3>("steps", {4, 4, 4});
		const Rgb3 channelSteps{steps.X, steps.Y, steps.Z};
		const uint32_t side = 2u << uint32_t(pattern);
		const bool dithering = context.Boolean("dithering");
		const double alphaSteps = context.Scalar("alpha_steps", 256);
		return RunPixelProcessor(
			context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
				const Rgba base = ReadPixel(source, x, y);
				Rgb3 colour = QuantizeSpace({base[0], base[1], base[2]}, space, false);
				const double rank = double(BayerRank(x % side, y % side, side)) / (side * side - 1);
				const double contrast = MappedScalar(context, "contrast", u, v);
				for (size_t channel = 0; channel < 3; ++channel)
					colour[channel] =
						QuantizedChannel(colour[channel], channelSteps[channel], dithering, contrast, rank);
				colour = QuantizeSpace(colour, space, true);
				return Rgba{
					colour[0], colour[1], colour[2], std::floor(base[3] * alphaSteps) / (alphaSteps - 1)
				};
			}
		);
	}
}
