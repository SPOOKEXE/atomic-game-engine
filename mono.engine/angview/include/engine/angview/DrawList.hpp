#pragma once

// Graph-neutral shapes a painter turns into pixels. All coordinates are in
// view space (pixels from the canvas top-left), already zoomed and panned.
//
// Painters exist for Dear ImGui (Studio) and the gui DrawList (games). A script
// can also take this list and draw it itself.
// @tier L4 · shared

#include <engine/angview/Theme.hpp>
#include <engine/core/types/Vector2.hpp>

#include <string>
#include <vector>

namespace engine::angview {

	enum class DrawKind : uint8_t {
		// Filled and/or stroked rectangle from A (min) to B (max), Radius rounds corners.
		Rect,
		// Line from A to B.
		Line,
		// Cubic bezier A -> B with controls C and D.
		Bezier,
		// Circle at A with Radius.
		Circle,
		// Diamond centred on A, Radius to each point.
		Diamond,
		// Text with its top-left at A. B.X is the clip width, zero for none.
		Text,
		// Host image handle drawn from A to B.
		Image,
		// Painter clip rectangle from A to B until the next PopClip.
		PushClip,
		PopClip,
	};

	struct DrawCommand {
		DrawKind Kind = DrawKind::Rect;
		core::Vector2 A{};
		core::Vector2 B{};
		core::Vector2 C{};
		core::Vector2 D{};
		Rgba Fill{0.0f, 0.0f, 0.0f, 0.0f};
		Rgba Stroke{0.0f, 0.0f, 0.0f, 0.0f};
		float Thickness = 1.0f;
		float Radius = 0.0f;
		float TextSize = 13.0f;
		std::string Text{};
		uint64_t Image = 0;
	};

	using DrawList = std::vector<DrawCommand>;
}
