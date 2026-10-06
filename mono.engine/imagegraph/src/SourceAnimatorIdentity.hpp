#pragma once
#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
#include <optional>
namespace engine::imagegraph::detail {
	inline std::string_view BindingAnimatorPort(const GroupSubtypeBinding &binding) noexcept {
		return binding.AnimatorPort.empty() ? std::string_view(binding.Port)
											: std::string_view(binding.AnimatorPort);
	}
	inline bool MovedSourceAnimator(const GroupSubtypeBinding &binding) noexcept {
		return !binding.AnimatorPort.empty() && binding.AnimatorPort != binding.Port;
	}
	inline bool InheritedMovedSourceGetter(
		const Node &node, std::string_view port, const GroupSubtypeBinding *binding
	) noexcept {
		return binding && MovedSourceAnimator(*binding) &&
			   std::find(node.InstanceOverrides.begin(), node.InstanceOverrides.end(), port) ==
				   node.InstanceOverrides.end();
	}
	inline std::string_view
	SourceGetterPort(const Node &node, std::string_view port, const GroupSubtypeBinding *binding) noexcept {
		return binding && !InheritedMovedSourceGetter(node, port, binding) ? BindingAnimatorPort(*binding)
																		   : port;
	}
	// source getters keep the local mode separate from the original animator writer.
	inline std::optional<bool> SourcePropertyGetterAnimated(
		const Document &document, const Node &node, std::string_view port, const GroupReplayState *replay
	) {
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
