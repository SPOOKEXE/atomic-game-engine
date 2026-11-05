#pragma once

#include <engine/gui/Input.hpp>

namespace engine::gui {
	namespace detail {
		inline constexpr size_t MAX_POINTER_ENTRY_TEXT_BYTES = 65536;
	}
	inline constexpr size_t MAX_POINTER_HITS = 4096;
	inline constexpr size_t MAX_POINTER_KEY_BYTES = 65536;

	// Copied pointer-routing decision for delayed interaction validation.
	struct PointerHitWitness {
		ecs::Entity Source, Collector, Collection;
		std::string Key;
		uint32_t Index = UINT32_MAX;
		core::Rect Bounds, Clip;
		core::Vector2 Point;
		float Rotation = 0.0f;
		uint8_t Reach = 0;
		bool operator==(const PointerHitWitness &) const = default;
	};
	struct PointerModalWitness {
		ecs::Entity Collector, Current, Next, FocusBefore, SelectionBefore;
		bool operator==(const PointerModalWitness &) const = default;
	};
	struct PointerScrollWitness {
		core::Vector2 CanvasPosition, CanvasSize, WindowSize, Overshoot, Position, Size;
		core::Rect VerticalThumb, HorizontalThumb;
		float Rotation = 0;
		uint8_t Flags = 0, Direction = 0, Elastic = 0;
		bool operator==(const PointerScrollWitness &) const = default;
	};
	struct PointerDecision {
		Pointer Sample;
		bool Valid = true;
		uint64_t Store = 0;
		ecs::Entity Hovered;
		ecs::Entity VisualBar, VisualDrag, VisualDetector, VisualCanvas, VisualWheel;
		float VisualBarGrab = 0.0f;
		core::Vector2 VisualWheelOrigin, VisualWheelPoint, VisualDragPoint, VisualCanvasOvershoot;
		core::UDim2 VisualDragStart;
		float VisualDragAngle = 0.0f;
		std::array<float, 12> VisualWheelValues{};
		PointerScrollWitness VisualBarState, VisualCanvasState;
		uint8_t VisualWheelFlags = 0;
		bool VisualBarVertical = false, VisualWheelGraph = false;
		int32_t VisualCaret = -1;
		bool VisualEntryClearOnFocus = false, VisualEntryHasLabel = false;
		std::string VisualEntryText;
		bool ModalOverflow = false;
		std::vector<PointerHitWitness> Hits;
		std::vector<PointerModalWitness> Modals;
	};

}
