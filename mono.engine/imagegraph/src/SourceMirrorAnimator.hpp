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
		const auto *binding = replay && replay->InstancesBound() ? replay->Binding(node.Id, port) : nullptr;
		if (binding && !InheritedMovedSourceGetter(node, port, binding))
			return binding->Getter == GroupSubtypeAnimator::Animated;
		const Node *flagOwner = &node;
		const bool ownLink = std::any_of(document.Links.begin(), document.Links.end(), [&](const auto &link) {
			return link.ToNode == node.Id && link.ToPort == port;
		});
		const bool override = std::find(node.InstanceOverrides.begin(), node.InstanceOverrides.end(), port) !=
							  node.InstanceOverrides.end();
		if (!node.InstanceBase.empty() && !ownLink && !override) {
			const auto base =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == node.InstanceBase;
				});
			if (base == document.Nodes.end()) return std::nullopt;
			flagOwner = &*base;
		}
		if (std::find(flagOwner->SourceAnimatedInputs.begin(), flagOwner->SourceAnimatedInputs.end(), port) !=
			flagOwner->SourceAnimatedInputs.end())
			return true;
		if (std::find(flagOwner->SourceStaticInputs.begin(), flagOwner->SourceStaticInputs.end(), port) !=
			flagOwner->SourceStaticInputs.end())
			return false;
		return std::nullopt;
	}
}
