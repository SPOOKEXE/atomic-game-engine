#pragma once

// Scalar path of the pinned public Vignette shader. The source declares an
// Exponent input but does not read it in processData or the shader.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class VignetteStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedPower };

	struct VignetteControls {
		Vector2 Center{0.5, 0.5};
		double Roundness = 0.0;
		double Exposure = 15.0;
		double Strength = 1.0;
		double Lighten = 0.0;
		Colour Color{255, 255, 255, 255};
	};

	inline VignetteStatus
	RenderVignetteScalar(const Image &source, Image &output, const VignetteControls &control) {
		const uint64_t bytes = uint64_t(source.Width) * source.Height * 4;
		if (source.Width == 0 || source.Height == 0 || source.Width != output.Width ||
			source.Height != output.Height || source.Pixels.size() != bytes || output.Pixels.size() != bytes)
			return VignetteStatus::InvalidImage;
		if (!std::isfinite(control.Center.X) || !std::isfinite(control.Center.Y) ||
			!std::isfinite(control.Roundness) || !std::isfinite(control.Exposure) ||
			!std::isfinite(control.Strength) || !std::isfinite(control.Lighten))
			return VignetteStatus::InvalidControl;
		const double smooth = control.Roundness / 2.0;
		const double power = 0.25 + smooth;
		const std::array<double, 3> color{
			control.Color.Red / 255.0, control.Color.Green / 255.0, control.Color.Blue / 255.0
		};
		const auto byte = [](double value) {
			return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
		};
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double dx = u - 0.5;
				const double dy = v - 0.5;
				const double distance = dx * dx + dy * dy;
				const double angle = std::atan2(dy, dx);
				const double projectedX = control.Center.X + std::cos(angle) * distance;
				const double projectedY = control.Center.Y + std::sin(angle) * distance;
				const double warpedX = u * (1.0 - smooth) + projectedX * smooth;
				const double warpedY = v * (1.0 - smooth) + projectedY * smooth;
				const double vignetteBase =
					warpedX * (1.0 - warpedY) * warpedY * (1.0 - warpedX) * control.Exposure;
				if ((vignetteBase < 0.0 && std::trunc(power) != power) ||
					(vignetteBase == 0.0 && power <= 0.0) ||
					(vignetteBase > 0.0 && !std::isfinite(std::pow(vignetteBase, power))))
					return VignetteStatus::UndefinedPower;
				const double vignette = std::clamp(std::pow(vignetteBase, power), 0.0, 1.0);
				const double strength = 1.0 - (1.0 - vignette) * control.Strength;
				const double inverse = strength < 0.001 ? 10000.0 : 1.0 / strength;
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				for (size_t channel = 0; channel < 3; channel++) {
					const double sample = source.Pixels[offset + channel] / 255.0;
					const double darkBase = sample * strength;
					const double dark = darkBase * color[channel] * (1.0 - strength) + darkBase * strength;
					const double lightBase = sample * inverse;
					const double light =
						lightBase * (1.0 - color[channel]) * (1.0 - inverse) + lightBase * inverse;
					const double result = dark * (1.0 - control.Lighten) + light * control.Lighten;
					if (!std::isfinite(result)) return VignetteStatus::UndefinedPower;
					output.Pixels[offset + channel] = byte(result);
				}
				output.Pixels[offset + 3] = source.Pixels[offset + 3];
			}
		}
		return VignetteStatus::Ok;
	}
}
