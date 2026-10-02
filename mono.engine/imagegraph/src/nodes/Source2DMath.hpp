#pragma once
#include "Processor.hpp"

#include <cmath>

namespace engine::imagegraph::detail::source2d {

	inline Vector2 Rotate(Vector2 point, double radians) {
		const double cosine = std::cos(radians), sine = std::sin(radians);
		return {point.X * cosine - point.Y * sine, point.X * sine + point.Y * cosine};
	}

	inline Vector2
	PixelPosition(const NodeContext &context, std::string_view port, uint32_t width, uint32_t height) {
		Vector2 position = context.Vec2(port);
		if (!context.IsLinked(port) && context.Integer(std::string(port) + "_unit", 1) == 1) {
			position.X *= width;
			position.Y *= height;
		}
		return {position.X / width, position.Y / height};
	}

}
