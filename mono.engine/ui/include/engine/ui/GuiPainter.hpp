#pragma once

// Drawing an engine `gui::DrawList` with Dear ImGui's rasteriser.
//
// **This module and `gui` are still not the same thing, and this file is the
// seam rather than a merge.** `ui/AGENTS.md` says the one thing the two share
// is the glyph atlas: four faces are vendored here, and a second rasteriser
// over the same four files would be two answers to what a glyph looks like. So
// the engine's own widget tree compiles to a flat list at L7, and this draws
// that list through the atlas that already exists.
//
// It takes a `gui::DrawList` and an `ImDrawList` and nothing else - no store,
// no class table, no tree. That is what makes it replaceable: the batched quad
// pipeline the v0.8 plan describes is a *second consumer of the same list*, not
// a second compile, so neither backend can drift from the other about where an
// element is.
//
// ## Why this backend still exists
//
// `mono.client` deliberately does not link this module because a shipped game
// should not carry imgui. It draws the same compiled list through
// `render::InterfacePass`; this painter remains the Studio backend for screen
// collectors inside viewport panels. Spatial collectors use that render pass
// in both hosts so depth, portal and mirror projection have one implementation.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/gui/DrawList.hpp>

#include <functional>
#include <imgui.h>

namespace engine::ui {

	// Where an image lives, once something knows.
	//
	// **A hook rather than a dependency on the asset pipeline.** This module is
	// the editor's toolkit and has no business resolving a game's content
	// names; whoever owns the textures supplies this, and until something does,
	// an `ImageLabel` draws the missing-image marker below rather than nothing.
	//
	// @since v0.8
	struct ImageSource {
		// What a name resolved to.
		//
		// **A struct rather than out-parameters**, because there are four
		// answers now and a signature with three `&`s is one a caller fills in
		// the wrong order exactly once.
		//
		// @since v0.10
		struct Resolved {
			// The texture, or a null id when there is none.
			ImTextureID Texture{};

			// The image's pixel dimensions, which the nine-slice and tile paths
			// need and the stretch path does not.
			ImVec2 Size{0.0f, 0.0f};

			// The sub-rectangle the current animation cell occupies, in texture
			// coordinates.
			//
			// **The whole image for anything that is not a sheet**, so a caller
			// applies it unconditionally. A `.gif` bakes to an ordinary texture
			// carrying a grid of frames - `render::FlipbookCell` - and only the
			// thing that uploaded it knows the grid is there, which is why this
			// comes back with the handle rather than being asked for separately.
			//@{
			ImVec2 CellMin{0.0f, 0.0f};
			ImVec2 CellMax{1.0f, 1.0f};
			//@}
		};

		// The texture for a content name.
		std::function<Resolved(const core::Name &name)> Resolve;

		// The live texture rendered for a `ViewportFrame` element.
		std::function<Resolved(ecs::Entity instance)> ResolveViewport;
	};

	// How a compiled list is placed on screen.
	//
	// @since v0.8
	struct PaintTarget {
		// Where canvas (0, 0) sits, in imgui's screen coordinates.
		//
		// A `ScreenGui` compiled against a viewport panel is laid out from the
		// panel's own origin, so the offset is where that panel is - which is
		// why this is a parameter rather than assumed to be the window corner.
		ImVec2 Origin{0.0f, 0.0f};

		// Multiplied into every coordinate after the origin is applied.
		//
		// For a panel showing a 1600x900 canvas at half size. One knob, for
		// `InterfaceSettings::Scale`'s reason: a UI scaled on one axis and not
		// the other reads as a corrupt font rather than as small.
		float Scale = 1.0f;
	};

	// Draws every command in `list` into `into`.
	//
	// **Does not open or close a window and does not touch imgui state beyond
	// the draw list.** A caller decides where the pixels go - the background
	// draw list for a full-screen overlay, a window's own for a panel - which
	// is the difference between a painter and a panel.
	//
	// Clip rectangles are pushed and popped per command, so the list may be
	// drawn into a target that already has one and it will be intersected
	// rather than replaced.
	//
	// @param list   The compiled list, in paint order.
	// @param into   The imgui draw list to record into.
	// @param target Where the canvas goes and how big.
	// @param images How to resolve content and viewport images, or an empty hook.
	// @return How many imgui primitives were recorded, for a statistics panel.
	size_t PaintGui(
		const gui::DrawList &list, ImDrawList *into, const PaintTarget &target, const ImageSource &images = {}
	);
}
