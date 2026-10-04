#pragma once

#include <engine/imagegraph/SourceFont.hpp>

namespace engine::imagegraph::detail {
	Status MeasureNativeFontData(
		const FontData &,
		std::span<const FontMeasurement>,
		uint64_t maximumOperationBytes,
		uint64_t maximumWorkUnits,
		std::vector<FontMeasurement> &,
		std::string &failure
	);
}
