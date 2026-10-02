#pragma once

#include <engine/imagegraph/Surface.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraphexport::runner {
	bool EncodeStill(
		std::string_view extension,
		const engine::imagegraph::Image &image,
		std::vector<uint8_t> &bytes,
		std::string &failure
	);
}
