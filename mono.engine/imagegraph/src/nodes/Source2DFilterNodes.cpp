#include "ColorSpace.hpp"
#include "Sampler.hpp"
#include "Source2DMath.hpp"

namespace engine::imagegraph::detail {
	using source2d::PixelPosition;
	using source2d::Rotate;

	bool MultiplyAlpha(NodeContext &context) {
		const Colour tint = context.Get<Colour>("bg_color", {255, 255, 255, 255});
		const Rgb3 background{tint.Red / 255.0, tint.Green / 255.0, tint.Blue / 255.0};
		return RunPixelProcessor(context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
			Rgba colour = ReadPixel(source, x, y);
			if (colour[3] <= context.Scalar("threshold")) return Rgba{};
			for (size_t channel = 0; channel < 3; ++channel)
				colour[channel] *= colour[3] + background[channel] * (1.0 - colour[3]);
			colour[3] = 1;
			return colour;
		});
	}

	bool ColorBlind(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const int64_t type = context.Integer("type");
		if (type < 0 || type > 8)
			return context.Fail(Status::InvalidValue, "Color Blind type is invalid", "type");
		// Pinned shader matrices are column-major, including its achromatopsia luminance matrix.
		static constexpr std::array<std::array<double, 9>, 9> MATRICES{{
			{1, 0, 0, 0, 1, 0, 0, 0, 1},
			{0.152, 1.053, -0.205, 0.115, 0.786, 0.099, -0.004, -0.048, 1.052},
			{0.817, 0.333, -0.150, 0.333, 0.667, 0, -0.017, 0, 1.017},
			{0.367, 0.861, -0.228, 0.280, 0.673, 0.047, -0.012, 0.043, 0.969},
			{0.800, 0.200, 0, 0.258, 0.742, 0, 0, 0.142, 0.858},
			{1.256, -0.077, -0.179, -0.078, 0.931, 0.148, 0.005, 0.691, 0.304},
			{0.967, 0.033, 0, 0, 0.733, 0.267, 0, 0.183, 0.817},
			{0.299, 0.299, 0.299, 0.587, 0.587, 0.587, 0.114, 0.114, 0.114},
			{0.618, 0.320, 0.062, 0.163, 0.775, 0.062, 0.163, 0.320, 0.516},
		}};
		return RunPixelProcessor(context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
			const Rgba base = ReadPixel(source, x, y);
			Rgba transformed{0, 0, 0, base[3]};
			const auto &matrix = MATRICES[size_t(type)];
			for (size_t output = 0; output < 3; ++output)
				for (size_t input = 0; input < 3; ++input)
					transformed[output] += base[input] * matrix[input * 3 + output];
			return transformed;
		});
	}

	bool ExtractChannels(NodeContext &context) {
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const bool hsv = context.Authored.Type == "pc.hsv_channel", array = context.Boolean("output_array");
		const int64_t outputType = context.Integer("output_type"), space = context.Integer("color_space");
		if (!hsv && (outputType < 0 || outputType > 1))
			return context.Fail(Status::InvalidValue, "Output Type is invalid", "output_type");
		if (hsv && (space < 0 || space > 1))
			return context.Fail(Status::InvalidValue, "Color Space is invalid", "color_space");
		const std::array<std::string_view, 4> ports =
			hsv ? std::array<std::string_view, 4>{"hue", "saturation", "value", "alpha"}
				: std::array<std::string_view, 4>{"red", "green", "blue", "alpha"};
		std::array<Image *, 4> outputs{};
		for (size_t channel = 0; channel < 4; ++channel) {
			outputs[channel] =
				context.NewImage(ports[channel], source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			if (!outputs[channel]) return false;
		}
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const Rgba pixel = ReadPixel(*source, x, y);
				const Rgb3 components =
					hsv ? ShaderRgbToHsv({pixel[0], pixel[1], pixel[2]}) : Rgb3{pixel[0], pixel[1], pixel[2]};
				for (size_t channel = 0; channel < 4; ++channel) {
					Rgba extracted{};
					if (channel == 3)
						extracted = !hsv && outputType == 1 ? Rgba{pixel[3], pixel[3], pixel[3], 1}
															: Rgba{1, 1, 1, pixel[3]};
					else if (hsv) {
						const double level = channel == 2 && space == 1
												 ? (std::max({pixel[0], pixel[1], pixel[2]}) +
													std::min({pixel[0], pixel[1], pixel[2]})) /
													   2.0
												 : components[channel];
						extracted = {level, level, level, pixel[3]};
					} else {
						if (outputType == 1)
							extracted = {components[channel], components[channel], components[channel], 1};
						else {
							extracted[channel] = components[channel];
							extracted[3] = 1;
						}
						if (context.Boolean("keep_alpha")) extracted[3] = pixel[3];
					}
					if (!WritePixel(*outputs[channel], x, y, extracted))
						return context.Fail(
							Status::InvalidValue, "Channel sample exceeds surface range", ports[channel]
						);
				}
			}
		if (array) {
			if (!context.ReserveOutput(4 * (sizeof(Image) + sizeof(ImageArrayItem)), ports[0])) return false;
			ImageArray channels;
			channels.Images.reserve(4);
			channels.Items.reserve(4);
			for (size_t channel = 0; channel < 4; ++channel) {
				channels.Images.push_back(std::move(*outputs[channel]));
				channels.Items.push_back({channel});
			}
			context.OutputImages.clear();
			context.OutputImageArrays.emplace_back(std::string(ports[0]), std::move(channels));
		}
		return true;
	}

	Rgb3 RemoveLab(Rgb3 colour) {
		for (double &channel : colour)
			channel = channel > 0.04045 ? std::pow((channel + 0.055) / 1.055, 2.4) : channel / 12.92;
		Rgb3 xyz{
			100 * (colour[0] * 0.4124 + colour[1] * 0.3576 + colour[2] * 0.1805),
			100 * (colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722),
			100 * (colour[0] * 0.0193 + colour[1] * 0.1192 + colour[2] * 0.9505)
		};
		constexpr Rgb3 WHITE{95.047, 100, 108.883};
		for (size_t channel = 0; channel < 3; ++channel) {
			const double value = xyz[channel] / WHITE[channel];
			xyz[channel] = value > 0.008856 ? std::pow(value, 1.0 / 3.0) : 7.787 * value + 16.0 / 116.0;
		}
		return {
			(116.0 * xyz[1] - 16.0) / 100.0,
			0.5 + 0.5 * 500.0 * (xyz[0] - xyz[1]) / 127.0,
			0.5 + 0.5 * 200.0 * (xyz[1] - xyz[2]) / 127.0
		};
	}

	bool ColorRemove(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const int64_t space = context.Integer("color_space");
		if (space < 0 || space > 1)
			return context.Fail(Status::InvalidValue, "Color Space choice is invalid", "color_space");
		std::array<Rgb3, 256> palette{};
		size_t count = 0;
		const auto *colours = std::get_if<ArrayValue>(context.Find("colors"));
		if (colours)
			for (const auto &element : colours->Elements) {
				const auto *colour = std::get_if<Colour>(&element);
				if (!colour)
					return context.Fail(Status::InvalidValue, "Colors requires colour elements", "colors");
				if (count == palette.size())
					return context.Fail(Status::LimitExceeded, "Source palette limit is 256", "colors");
				const Rgb3 rgb{colour->Red / 255.0, colour->Green / 255.0, colour->Blue / 255.0};
				palette[count++] = space == 0 ? rgb : RemoveLab(rgb);
			}
		return RunPixelProcessor(
			context, [&](const Image &source, uint32_t x, uint32_t y, double u, double v) {
				const Rgba pixel = ReadPixel(source, x, y);
				Rgb3 base{pixel[0], pixel[1], pixel[2]};
				if (space == 1) base = RemoveLab(base);
				double nearest = 99999;
				for (size_t index = 0; index < count; ++index) {
					const auto &match = palette[index];
					nearest = std::min(
						nearest, std::hypot(base[0] - match[0], base[1] - match[1], base[2] - match[2])
					);
				}
				const double threshold = MappedScalar(context, "threshold", u, v);
				return (!context.Boolean("invert") && nearest <= threshold) ||
							   (context.Boolean("invert") && nearest > threshold)
						   ? Rgba{}
						   : pixel;
			}
		);
	}

	bool NormalAdjust(NodeContext &context) {
		const Image *source = context.Input("normal"), *mask = context.Input("mask");
		if (!source) return context.Fail(Status::InvalidValue, "Normal is required", "normal");
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		const Vector3 scale = context.Get<Vector3>("scale", {1, 1, 1});
		const double angle = context.Scalar("rotate") * std::acos(-1.0) / 180.0;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				const double u = (x + 0.5) / output->Width, v = (y + 0.5) / output->Height;
				const Rgba base = ReadPixel(*source, x, y);
				const double intensity = MappedScalar(context, "intensity", u, v);
				const Vector2 xy = Rotate(
					{(base[0] - 0.5) * intensity * scale.X, (base[1] - 0.5) * intensity * scale.Y}, angle
				);
				Rgba normal{xy.X + 0.5, xy.Y + 0.5, base[2] * intensity * scale.Z, base[3]};
				if (mask) {
					const Rgba sample = SampleNearest(*mask, u, v);
					const double amount = (sample[0] + sample[1] + sample[2]) / 3.0 * sample[3];
					for (size_t channel = 0; channel < 3; ++channel)
						normal[channel] = base[channel] + (normal[channel] - base[channel]) * amount;
				}
				if (context.Boolean("normalize")) {
					const double length = std::hypot(normal[0], normal[1], normal[2]);
					for (size_t channel = 0; channel < 3; ++channel)
						normal[channel] /= length;
				}
				if (!WritePixel(*output, x, y, normal))
					return context.Fail(
						Status::InvalidValue, "Adjusted normal exceeds surface range", "surface_out"
					);
			}
		return true;
	}

	bool HighPass(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const SamplerSettings sampler = ReadSampler(context);
		const Vector2 range = context.Vec2("radius_map_range");
		const bool mapped = context.Boolean("radius_mapped") && context.Input("radius_map");
		const double radiusMaximum =
			std::floor(mapped ? std::max(range.X, range.Y) : context.Scalar("radius", 1));
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		constexpr uint64_t MAXIMUM_SAMPLES = 64'000'000;
		const double diameter = std::max(1.0, radiusMaximum * 2.0 + 1.0);
		if (!std::isfinite(radiusMaximum) ||
			diameter * diameter * source->Width * source->Height > MAXIMUM_SAMPLES)
			return context.Fail(Status::LimitExceeded, "High Pass exceeds sample work budget", "radius");
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			const double radius = std::floor(MappedScalar(context, "radius", u, v, true));
			Rgba result{};
			double weightTotal = 0;
			for (double i = -radiusMaximum; i <= radiusMaximum; ++i) {
				if (i < -radius) continue;
				if (i > radius) break;
				for (double j = -radiusMaximum; j <= radiusMaximum; ++j) {
					if (j < -radius) continue;
					if (j > radius) break;
					if (i == 0 && j == 0) continue;
					const double weight = (radius - std::abs(i) - std::abs(j) + 1.0) / radius / 4.0;
					if (weight <= 0) continue;
					const Rgba sample = SampleTextureSimple(
						image, u + i / image.Width, v + j / image.Height, sampler.Oversample, true
					);
					for (size_t channel = 0; channel < 4; ++channel)
						result[channel] -= sample[channel] * weight;
					weightTotal += weight;
				}
			}
			const Rgba centre = SampleTextureSimple(image, u, v, sampler.Oversample, true);
			const double intensity = MappedScalar(context, "intensity", u, v, true);
			for (size_t channel = 0; channel < 4; ++channel)
				result[channel] = (result[channel] + centre[channel] * weightTotal) * intensity +
								  (context.Boolean("blend_original", true) ? centre[channel] : 0.0);
			result[3] = centre[3];
			return result;
		});
	}

}
