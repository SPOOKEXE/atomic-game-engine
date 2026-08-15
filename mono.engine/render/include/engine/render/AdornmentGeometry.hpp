#pragma once

// Adornments, resolved into lines a device can draw.
//
// **This is the half `gui` could not do, and the reason is `D00022`'s.** An
// adornment says what to outline, in what colour and how solid; turning that
// into geometry needs the adornee's `CFrame` and stud extent, which are
// `scene::Transform` and `scene::Bounds`. `gui` is L7 `shared` and links
// neither - `gui/AGENTS.md` refuses the edge - so `gui::EachAdornment` answers
// *which* instance an adornment is about and stops there.
//
// This module links both, which is what `D00022` meant by "whoever draws one
// has both operands". Same split, one dimension up from a `SurfaceGui`'s canvas.
//
// **Lines rather than triangles, and that is what an adornment is.** A
// `SelectionBox` is twelve edges; a handle is a box or a ring. Filling them
// would hide what they are drawn around, which is the one thing a selection box
// exists to leave visible.
//
// **Headless.** Resolving an adornment is a transform and twelve corners, so it
// is testable without a device - the same reason `InterfaceMesh` is a type
// rather than a loop inside a recording function.
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// One line, in world space.
	//
	// @since v0.8
	struct AdornmentLine {
		// The two ends and what colour to draw between them.
		//@{
		core::Vector3 From;
		core::Vector3 To;
		core::Color3 Colour;
		//@}

		// 0 is opaque, Roblox's sense - kept because every other transparency
		// in this engine keeps it and one that flipped would be found by
		// somebody wondering why their selection box was invisible.
		float Transparency = 0.0f;

		// Whether it draws over the world rather than being occluded by it.
		//
		// **On by default for an adornment**, which is the opposite of a `Part`:
		// a selection box a wall hides is a selection box that does not say what
		// is selected, which is the one thing it is for.
		bool AlwaysOnTop = true;
	};

	// Every adornment in a world, as lines.
	//
	// Long-lived: rebuilt each frame into a buffer that keeps its capacity, so a
	// steady selection stops allocating.
	//
	// @since v0.8
	class AdornmentGeometry {
	  public:
		// Resolves every drawable adornment against its adornee.
		//
		// Walks `gui::EachAdornment`, which has already answered which instances
		// are adorned and which of them may be drawn at all - so nothing here
		// re-derives containment or the adornee fallback. What it adds is the
		// transform.
		//
		// **An adornee with no `Transform` produces nothing rather than a box at
		// the origin.** A `Folder` can legally be an `Adornee`, and outlining it
		// at (0,0,0) would draw a box around a place nothing is.
		//
		// @param store The world.
		void Build(ecs::Store &store);

		// The lines, valid until the next `Build`.
		const std::vector<AdornmentLine> &Lines() const {
			return Segments;
		}

	  private:
		void AddBox(const core::CFrame &frame, const core::Vector3 &halfExtent, const AdornmentLine &style);

		std::vector<AdornmentLine> Segments;
	};
}
