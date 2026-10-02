#pragma once

#include "Processor.hpp"

namespace engine::imagegraph::detail {
	struct PixelBuilderEffect {
		int64_t Type = 0, Pattern = 0, StrokePosition = 1, StrokeCorner = 0, ShinesAxis = 0;
		Rgba Color{1, 1, 1, 1}, PatternColor{1, 1, 1, 1};
		double Intensity = 1, PatternIntensity = 1, Modify = 4, Thickness = 1, Radius = 1;
		Vector2 PatternScale{1, 1}, PatternPosition{};
		bool MapBounds = false, Subtract = false;
		Vector4 HighlightWidths{};
		std::array<Rgba, 4> HighlightColors{
			Rgba{1, 1, 1, 1}, Rgba{1, 1, 1, 1}, Rgba{1, 1, 1, 1}, Rgba{1, 1, 1, 1}
		};
		double Direction = -90, Progress = 0.5, Slope = 1, Seed = 0;
		std::vector<double> Shines{2, 1, 1};
	};
	bool ReadPixelBuilderEffects(
		NodeContext &context, Vector2 dimension, std::vector<PixelBuilderEffect> &effects
	);
	bool ApplyPixelBuilderEffects(
		NodeContext &context,
		const Image &shape,
		const std::array<double, 4> &bounds,
		std::span<const PixelBuilderEffect> effects,
		Image &output,
		Vector2 canvasDimension
	);
}
