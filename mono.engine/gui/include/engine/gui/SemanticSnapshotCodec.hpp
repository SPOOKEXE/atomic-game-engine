#pragma once

#include <engine/gui/Accessibility.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace engine::gui {
	// Encode a bounded prefix for retention with a displayed frame. Sets the
	// decoded snapshot's Truncated flag when the complete tree does not fit.
	// On failure, output remains unchanged.
	bool
	EncodeSemanticSnapshot(const SemanticSnapshot &snapshot, size_t limit, std::vector<std::byte> &output);

	// Decode within a conservative capacity and allocation-overhead estimate.
	// On failure, output remains unchanged.
	bool DecodeSemanticSnapshot(std::span<const std::byte> bytes, size_t budget, SemanticSnapshot &output);
}
