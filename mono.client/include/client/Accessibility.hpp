#pragma once

// Native accessibility bridge for one product window. It consumes the shared
// GUI semantic snapshot and returns requests for the UI-owning thread to route.

#include <engine/ecs/Entity.hpp>
#include <engine/gui/Accessibility.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Input.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Window;

namespace client {
	struct AccessibilityAction {
		engine::ecs::Entity Target;
		engine::ecs::Entity Collector;
		uint64_t WorldEpoch = 0;
		engine::gui::SemanticAction Action = engine::gui::SemanticAction::Activate;
	};

	// Rejects callbacks from a replaced store or collector before they reach the
	// ordinary UI router. The draw list is the UI thread's current snapshot.
	inline bool IsCurrentAccessibilityAction(
		const AccessibilityAction &action, uint64_t worldEpoch, const engine::gui::DrawList &list
	) {
		if (action.WorldEpoch != worldEpoch) return false;
		return std::ranges::any_of(list.Commands, [&action](const engine::gui::DrawCommand &command) {
			return command.Source == action.Target && command.Collector == action.Collector;
		});
	}

	// AccessKit consumes product-window coordinates. Screen collectors are mapped
	// through their compile-time transform; spatial collectors have no stable
	// window rectangle and are omitted.
	inline engine::gui::SemanticSnapshot ProjectAccessibilitySnapshot(
		const engine::gui::SemanticSnapshot &snapshot, const engine::gui::DrawList &list
	) {
		std::unordered_map<uint64_t, engine::gui::CollectorTransform> transforms;
		transforms.reserve(list.Transforms.size());
		for (const engine::gui::CollectorTransform &transform : list.Transforms) {
			transforms.emplace(transform.Collector.Id, transform);
		}
		std::unordered_set<uint64_t> spatialCollectors;
		for (const engine::gui::DrawCommand &command : list.Commands) {
			if (command.Spatial) spatialCollectors.insert(command.Collector.Id);
		}

		engine::gui::SemanticSnapshot projected;
		projected.Truncated = snapshot.Truncated;
		projected.Nodes.reserve(snapshot.Nodes.size());
		for (const engine::gui::SemanticNode &node : snapshot.Nodes) {
			if (spatialCollectors.contains(node.Collector.Id)) continue;
			engine::gui::SemanticNode mapped = node;
			if (const auto transform = transforms.find(node.Collector.Id); transform != transforms.end()) {
				mapped.Bounds.Min = transform->second.Origin + mapped.Bounds.Min * transform->second.Scale;
				mapped.Bounds.Max = transform->second.Origin + mapped.Bounds.Max * transform->second.Scale;
			}
			projected.Nodes.push_back(std::move(mapped));
		}
		return projected;
	}

	class AccessibilityAdapter {
	  public:
		struct State;

		explicit AccessibilityAdapter(SDL_Window *window);
		~AccessibilityAdapter();

		AccessibilityAdapter(const AccessibilityAdapter &) = delete;
		AccessibilityAdapter &operator=(const AccessibilityAdapter &) = delete;

		// Replaces the immutable tree observed by the platform adapter. `worldEpoch`
		// is part of every generated node id, so a recycled ECS entity cannot be
		// targeted through an action queued for an earlier world.
		void Update(const engine::gui::SemanticSnapshot &snapshot, uint64_t worldEpoch);
		void SetWindowFocused(bool focused);

		// Returns requests copied by the platform callback. This runs only on the
		// product UI thread, which validates each target through gui::Router.
		std::vector<AccessibilityAction> Drain();

	  private:
		std::unique_ptr<State> Data;
	};
}
