#pragma once
#include "SourceSeparatedVec2.hpp"

#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
namespace engine::imagegraph::detail {
	inline std::string_view BindingAnimatorPort(const GroupSubtypeBinding &binding) noexcept {
		return binding.AnimatorPort.empty() ? std::string_view(binding.Port)
											: std::string_view(binding.AnimatorPort);
	}
	inline bool BindingReferencesAxes(
		const GroupSubtypeBinding &binding, std::string_view owner, std::string_view port
	) noexcept {
		if (binding.Axes.Storage != GroupAxisStorage::None)
			return binding.Axes.OwnerId == owner && binding.Axes.Port == port;
		return binding.OwnerId == owner && BindingAnimatorPort(binding) == port;
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
	// shared keys keep their writer; an override still owns its getter's X/Y choice.
	inline const Node *SourcePropertyGetterNode(
		const Document &document,
		const Node &node,
		std::string_view port,
		uint64_t *work = nullptr,
		bool *workRefused = nullptr
	) {
		const Node *selected = &node;
		for (size_t hop = 0; hop <= document.Nodes.size(); ++hop) {
			if (work) {
				const uint64_t scan =
					1 + selected->InstanceOverrides.size() + document.Links.size() + document.Nodes.size();
				if (*work > 64'000'000 || scan > 64'000'000 - *work) {
					if (workRefused) *workRefused = true;
					return nullptr;
				}
				*work += scan;
			}
			const bool override =
				std::find(selected->InstanceOverrides.begin(), selected->InstanceOverrides.end(), port) !=
				selected->InstanceOverrides.end();
			const bool linked =
				std::any_of(document.Links.begin(), document.Links.end(), [&](const auto &link) {
					return link.ToNode == selected->Id && link.ToPort == port;
				});
			if (selected->InstanceBase.empty() || override || linked) return selected;
			const auto base =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == selected->InstanceBase;
				});
			if (base == document.Nodes.end()) return nullptr;
			selected = &*base;
		}
		return nullptr;
	}
	inline bool
	SourcePropertyLocallySeparated(const Node &node, std::string_view port, const GroupReplayState *replay) {
		const auto *overlay = replay ? replay->SharedSubtype(node.Id, port) : nullptr;
		const auto *axes =
			overlay && overlay->SeparatedVec2 ? &*overlay->SeparatedVec2 : FindSeparatedVec2(node, port);
		return axes && axes->Separated;
	}
	inline bool SourcePropertyGetterSeparated(
		const Document &document, const Node &node, std::string_view port, const GroupReplayState *replay
	) {
		const auto *selected = SourcePropertyGetterNode(document, node, port);
		return selected && SourcePropertyLocallySeparated(*selected, port, replay);
	}
	// source getters keep the local mode separate from the original animator writer.
	inline std::optional<bool> SourcePropertyGetterAnimated(
		const Document &document, const Node &node, std::string_view port, const GroupReplayState *replay
	) {
		const auto *binding = replay && replay->InstancesBound() ? replay->Binding(node.Id, port) : nullptr;
		if (binding && !InheritedMovedSourceGetter(node, port, binding))
			return binding->Getter == GroupSubtypeAnimator::Animated;
		const Node *flagOwner = SourcePropertyGetterNode(document, node, port);
		if (!flagOwner) return std::nullopt;
		if (std::find(flagOwner->SourceAnimatedInputs.begin(), flagOwner->SourceAnimatedInputs.end(), port) !=
			flagOwner->SourceAnimatedInputs.end())
			return true;
		if (std::find(flagOwner->SourceStaticInputs.begin(), flagOwner->SourceStaticInputs.end(), port) !=
			flagOwner->SourceStaticInputs.end())
			return false;
		return std::nullopt;
	}
}
