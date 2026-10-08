#pragma once

#include "SourceAnimatorIdentity.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace engine::imagegraph::detail {
	inline constexpr std::array<std::string_view, 5> SourceMirrorVectorPorts = {
		"relative_dimension", "constant_dimension", "position", "center", "scale"
	};
	inline constexpr std::array<std::string_view, 7> SourceRepeatVectorPorts = {
		"position", "shift_position", "center", "radius", "anchor", "scale", "shift_scale"
	};
	inline std::span<const std::string_view> SourceConsumerVectorPorts(std::string_view type) {
		if (type == "pc.mirror_polar") return SourceMirrorVectorPorts;
		if (type == "pc.path_repeat") return SourceRepeatVectorPorts;
		return {};
	}
	inline std::optional<size_t> SourceConsumerVectorIndex(std::string_view type, std::string_view port) {
		const auto ports = SourceConsumerVectorPorts(type);
		for (size_t i = 0; i < ports.size(); ++i)
			if (ports[i] == port) return i;
		return std::nullopt;
	}
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
		if (!SourceConsumerVectorIndex(node.Type, port)) return std::nullopt;
		return SourcePropertyGetterAnimated(document, node, port, replay);
	}
}
