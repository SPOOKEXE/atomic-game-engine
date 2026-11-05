#pragma once

#include "PointerDecision.hpp"

// Owned facts needed to sample the GUI that was presented for a frame.
// This is private to gui; no ECS pointer survives capture.
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/InputContinuity.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui::detail {

	// Compact paint command. Shaping is populated only for text-entry commands.
	struct PointerFrameCommand {
		DrawKind Kind = DrawKind::Rectangle;
		bool Spatial = false;
		ecs::Entity Source, Collector, Collection;
		std::string Key;
		uint32_t Index = UINT32_MAX;
		core::Rect Bounds, Clip;
		float Rotation = 0.0f;
		int32_t TextSize = 0;
		float LineHeight = 1.0f;
		TextXAlignment XAlignment = TextXAlignment::Center;
		TextYAlignment YAlignment = TextYAlignment::Center;
		ShapedText Shaping;
	};

	// One copied entity and its routing-relevant components. The first enabled
	// detector retains Store child iteration precedence.
	struct PointerFrameNode {
		ecs::Entity Entity, Parent, Collector, Detector;
		uint8_t Reach = 0; // Through, blocked, hit, matching the live selector.
		bool Alive = false;
		bool SpatialCollector = false;
		bool GuiButton = false;
		bool TextEntry = false;
		bool EntryClearOnFocus = false, EntryHasLabel = false;
		bool EntryEditable = false, EntryMultiline = false;
		std::string EntryText;
		bool NodeLink = false;
		std::optional<Element> ElementValue;
		std::optional<DragDetector> DetectorValue;
		std::optional<Selection> SelectionValue;
		std::optional<Scrolling> ScrollingValue;
		std::optional<ScrollState> ScrollValue;
		std::optional<ScrollMotion> MotionValue;
		std::optional<NodeCanvas> CanvasValue;
		std::optional<Resolved> ResolvedValue;
	};

	struct PointerFrameSpatial {
		ecs::Entity Collector;
		SpatialCanvas Canvas;
	};

	struct PointerFrameModal {
		ecs::Entity Collector, Next;
	};

	// A snapshot is valid only when capture finished within all declared bounds.
	// Nodes are sorted by full entity handle. WheelNodes preserves ECS query order.
	struct PointerFrameFacts {
		uint64_t StoreIdentity = 0;
		bool Valid = false;
		bool ModalOverflow = false;
		bool ExpectsScreen = false, ExpectsSpatial = false;
		bool HasScreenCommands = false, HasSpatialCommands = false, LayerClassificationValid = true;
		core::Vector2 CanvasSize;
		std::vector<PointerFrameCommand> Commands;
		std::vector<PointerFrameNode> Nodes;
		std::vector<ecs::Entity> GraphWheelNodes;
		std::vector<ecs::Entity> ScrollWheelNodes;
		std::vector<CollectorTransform> CollectorTransforms;
		std::vector<PointerFrameModal> Modals;
		std::vector<PointerFrameSpatial> SpatialCanvases;
		std::optional<InputContinuity> InitialInput;
		std::optional<RouterCaptureWitness> InitialRouter;

		const PointerFrameNode *Find(ecs::Entity entity) const;
		bool IsDescendant(ecs::Entity entity, ecs::Entity ancestor) const;
		size_t CommandCount() const {
			return Commands.size();
		}
		const PointerFrameCommand &CommandAt(size_t index) const {
			return Commands[index];
		}
		std::span<const CollectorTransform> Transforms() const {
			return CollectorTransforms;
		}
		uint8_t Reach(ecs::Entity entity) const;
		bool IsSpatial(ecs::Entity entity) const;
		bool Allows(ecs::Entity entity, ecs::Entity collector) const;
		bool IsEntry(ecs::Entity entity) const;
		bool IsLink(ecs::Entity entity) const;
		ecs::Entity Collector(ecs::Entity entity) const;
		ecs::Entity Detector(ecs::Entity entity) const;
		const Element *ElementOf(ecs::Entity entity) const;
		const Selection *SelectionOf(ecs::Entity entity) const;
		const Scrolling *ScrollingOf(ecs::Entity entity) const;
		const ScrollState *ScrollStateOf(ecs::Entity entity) const;
		const ScrollMotion *ScrollMotionOf(ecs::Entity entity) const;
		const NodeCanvas *NodeCanvasOf(ecs::Entity entity) const;
		const Resolved *ResolvedOf(ecs::Entity entity) const;
		template <class Callback> void EachGraph(Callback &&callback) const {
			for (const ecs::Entity entity : GraphWheelNodes) {
				const PointerFrameNode *node = Find(entity);
				if (node != nullptr && node->CanvasValue && node->ResolvedValue)
					callback(entity, *node->CanvasValue, *node->ResolvedValue);
			}
		}
		template <class Callback> void EachScrolling(Callback &&callback) const {
			for (const ecs::Entity entity : ScrollWheelNodes) {
				const PointerFrameNode *node = Find(entity);
				if (node != nullptr && node->ScrollingValue && node->ScrollValue && node->ResolvedValue)
					callback(entity, *node->ScrollingValue, *node->ScrollValue, *node->ResolvedValue);
			}
		}
	};
	// A frame sample contains only visual witnesses, not mutable router state.
	PointerDecision SamplePointerFrameFacts(const PointerFrameFacts &facts, const Pointer &pointer);

	// Capture refuses oversized or incomplete frames rather than sampling a partial view.
	bool CapturePointerFrameFacts(
		ecs::Store &store,
		const DrawList &list,
		std::span<const PointerFrameModal> modals,
		bool modalOverflow,
		PointerFrameFacts &out,
		std::span<const ecs::Entity> extraRoots = {}
	);
}
