#pragma once

// Dear ImGui painter for angview node graph views.
//
// The view decides everything: what a press means, where a node is, what to
// draw. This file only forwards ImGui input to it and turns its draw list into
// ImDrawList calls, so Studio's editors drag exactly like the in-game canvas.
//
// @tier L12 · client

#include <engine/angview/View.hpp>

#include <functional>
#include <imgui.h>

namespace engine::ui {

	// Resolves an `angview::DrawKind::Image` handle to a texture, or a null id.
	using NodeGraphImages = std::function<ImTextureID(uint64_t)>;

	// Draws `list` with its view-space origin at `origin`.
	void PaintNodeGraph(
		const angview::DrawList &list, ImDrawList &out, ImVec2 origin, const NodeGraphImages &images = {}
	);

	// One canvas widget over `size` (zero fills the available region): an
	// invisible button that forwards pointer, wheel, keys and typed text to
	// `view`, then paints it. Returns true when the view consumed input this frame.
	bool NodeGraphCanvas(
		const char *id, angview::View &view, ImVec2 size = {0.0f, 0.0f}, const NodeGraphImages &images = {}
	);

	// The Dark node graph theme recoloured from the editor's current palette,
	// so the canvas follows whatever palette the user picked.
	angview::Theme NodeGraphThemeFromPalette();
}
