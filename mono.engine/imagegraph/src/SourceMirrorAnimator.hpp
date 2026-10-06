#pragma once

#include "SourceAnimatorIdentity.hpp"

#include <array>
#include <optional>

namespace engine::imagegraph::detail {
	inline constexpr std::array<std::string_view, 5> SourceMirrorVectorPorts = {
		"relative_dimension", "constant_dimension", "position", "center", "scale"
	};
	inline std::optional<size_t> SourceMirrorVectorIndex(std::string_view port) {
		for (size_t i = 0; i < SourceMirrorVectorPorts.size(); ++i)
			if (SourceMirrorVectorPorts[i] == port) return i;
		return std::nullopt;
	}
	// getAnim reads the immediate base property's flag; the Vec2 getter reads its local animator.
	// Absence of source mode metadata preserves ordinary native timeline behavior.
	inline std::optional<bool> SourceMirrorGetterAnimated(
		const Document &document, const Node &node, std::string_view port, const GroupReplayState *replay
	) {
		if (node.Type != "pc.mirror_polar" || !SourceMirrorVectorIndex(port)) return std::nullopt;
		return SourcePropertyGetterAnimated(document, node, port, replay);
	}
}
