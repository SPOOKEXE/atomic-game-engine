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
	// Whether a native request still has a retained displayed-frame origin.
	enum class AccessibilityActionStatus : uint8_t { Ready, Expired, Overflow, Unsupported };

	// One platform accessibility action for the UI thread.
	struct AccessibilityAction {
		engine::ecs::Entity Target;	   // Semantic node to receive the action.
		engine::ecs::Entity Collector; // Collector that exposed the node.
		uint64_t WorldEpoch = 0;	   // World generation that produced the node.
		uint64_t FrameSequence = 0;	   // Displayed frame that supplied the node.
		uint64_t NativeNodeId = 0;	   // Native ID for diagnostics when its origin expired.
		uint32_t NativeAction = 0;	   // Platform action code.
		AccessibilityActionStatus Status = AccessibilityActionStatus::Ready; // Request outcome.
		uint64_t RejectedCount = 0; // Number of requests reported by an overflow marker.
		engine::gui::SemanticAction Action =
			engine::gui::SemanticAction::Activate; // Requested semantic action.
	};

	// Reports a native action that could not be routed to its original target.
	void ReportAccessibilityRejection(const AccessibilityAction &action, const char *reason);

	// Rejects callbacks from a replaced store or collector before they reach the
	// ordinary UI router. The draw list is the UI thread's current snapshot.
	inline bool IsCurrentAccessibilityAction(
		const AccessibilityAction &action, uint64_t worldEpoch, const engine::gui::DrawList &list
	) {
		if (action.Status != AccessibilityActionStatus::Ready || action.WorldEpoch != worldEpoch)
			return false;
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

	// Bridges the current GUI semantic tree to the native accessibility API.
	class AccessibilityAdapter {
	  public:
		struct State;

		// Binds the adapter to one product window.
		explicit AccessibilityAdapter(SDL_Window *window);
		~AccessibilityAdapter();

		AccessibilityAdapter(const AccessibilityAdapter &) = delete;
		AccessibilityAdapter &operator=(const AccessibilityAdapter &) = delete;

		// Replaces the immutable tree observed by the platform adapter. Every node
		// origin retains `worldEpoch`, so actions can reject a replaced world.
		void Update(const engine::gui::SemanticSnapshot &snapshot, uint64_t worldEpoch);
		// Reuse requires the host to prove semantic and input continuity with the
		// previously displayed frame. False leaves the current tree unchanged.
		bool Update(
			const engine::gui::SemanticSnapshot &snapshot,
			uint64_t worldEpoch,
			uint64_t frameSequence,
			bool reuseProven
		);
		// Invalidates the current tree and queued actions without allocating. Native
		// withdrawal is best effort when platform resources cannot be allocated.
		// The window owner thread must serialize Update, Retire and destruction.
		void Retire();
		// Whether the current tree uses direct routing for this world.
		bool HasLegacyTree(uint64_t worldEpoch) const;
		// Updates the native window-focus state.
		void SetWindowFocused(bool focused);

		// Returns requests copied by the platform callback. This runs only on the
		// product UI thread, which validates each target through gui::Router.
		std::vector<AccessibilityAction> Drain();

	  private:
		std::unique_ptr<State> Data;
	};
}
