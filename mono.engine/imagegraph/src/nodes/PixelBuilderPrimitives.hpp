#pragma once

#include "Processor.hpp"

namespace engine::imagegraph::detail {
	// The deterministic CPU profile consumes source vertices. GPU builtin edge coverage is a separate gate.
	bool RasterPixelBuilderPrimitive(NodeContext &context, const std::array<double, 4> &bounds, Image &shape);
}
