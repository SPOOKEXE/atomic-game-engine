#pragma once

#include <engine/imagegraph/Document.hpp>

#include <ostream>
#include <span>

namespace engine::imagegraph::detail {
	// Ordinary and scalar-axis records use one source key grammar, including driver metadata.
	bool WriteKeyframeText(std::ostream &, std::span<const Keyframe>, uint32_t version);
}
