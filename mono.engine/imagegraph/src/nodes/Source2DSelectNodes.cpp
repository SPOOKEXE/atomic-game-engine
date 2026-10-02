#include "ColorSpace.hpp"
#include "Processor.hpp"
#include "SourceSafeDraw.hpp"

namespace engine::imagegraph::detail {
	bool ColorSelect(NodeContext &context) {
		const auto range = [&](double value,
							   std::string_view minimum,
							   std::string_view minRadius,
							   std::string_view maximum,
							   std::string_view maxRadius) {
			const double min = context.Scalar(minimum), low = context.Scalar(minRadius),
						 max = context.Scalar(maximum, 1), high = context.Scalar(maxRadius);
			const double minS = min - low, minE = min + low, maxS = max - high, maxE = max + high;
			if (value <= minS || value >= maxE) return 0.0;
			if (value >= minE && value <= maxS) return 1.0;
			if (value < minE) return (value - minS) / (minE - minS);
			if (value > maxS) return 1 - (value - maxS) / (maxE - maxS);
			return 0.0;
		};
		return RunPixelProcessor(context, [&](const Image &source, uint32_t x, uint32_t y, double, double) {
			if (source.Format == SurfaceFormat::R8Unorm || source.Format == SurfaceFormat::R16Float ||
				source.Format == SurfaceFormat::R32Float)
				return SourceSafeDrawPixel(source, x, y);
			const Rgba original = ReadPixel(source, x, y);
			const Rgb3 hsv = ShaderRgbToHsv({original[0], original[1], original[2]});
			const double shiftedHue = hsv[0] - context.Scalar("h_shift");
			// Saturation and value shifts are declared controls but unused by the
			// pinned shader.
			const double selection =
				range(shiftedHue - std::floor(shiftedHue), "h_min", "h_min_range", "h_max", "h_max_range") *
				range(hsv[1], "s_min", "s_min_range", "s_max", "s_max_range") *
				range(hsv[2], "v_min", "v_min_range", "v_max", "v_max_range");
			return Rgba{
				selection, selection, selection, context.Boolean("use_input_alpha") ? original[3] : 1
			};
		});
	}
} // namespace engine::imagegraph::detail
