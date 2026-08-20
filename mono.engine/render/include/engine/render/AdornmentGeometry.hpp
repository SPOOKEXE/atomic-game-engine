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
// **Lines *and* faces as of v0.17, and the faces are opt-in.** A `SelectionBox`
// is twelve edges and, when somebody asks for one, six faces behind them.
// Filling by default would hide what the box is drawn around, which is the one
// thing a selection box exists to leave visible - so `SurfaceTransparency`
// defaults to fully invisible and no face is emitted at all until it moves.
// That is Roblox's default and its reason.
//
// **What this had for two versions was no consumer.** `Build` and `Lines` were
// called by nothing, and no pass in the engine drew a world-space line - so a
// `SelectionBox` drew nothing whatever its properties said, and the three
// properties `docs/DEFERRED.md` D00129 held back were held back behind a
// missing renderer rather than a missing triangle path. The studio draws them
// now, through the overlay `Editor::DrawColliderOutlines` already uses: project
// the world points into the panel and hand imgui the segments. No pipeline, no
// shader, no target.
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

		// How wide the line is, in studs.
		//
		// **Studs rather than pixels**, which is `SelectionBox.LineThickness`'s
		// unit and the one that behaves: a box outlined in pixels keeps the same
		// weight as it recedes and ends up a solid blob at a distance. A drawer
		// converts, because only a drawer knows how far away the camera is.
		//
		// @since v0.17
		float Thickness = 0.05f;

		// Whether it draws over the world rather than being occluded by it.
		//
		// **On by default for an adornment**, which is the opposite of a `Part`:
		// a selection box a wall hides is a selection box that does not say what
		// is selected, which is the one thing it is for.
		bool AlwaysOnTop = true;
	};

	// One filled face, in world space.
	//
	// **A quad rather than a triangle**, because every face this module emits is
	// one: a box has six and a handle is a box. Handing a drawer two triangles
	// with a shared edge would make it draw the seam twice under a transparent
	// colour, which shows as a brighter diagonal across every face.
	//
	// @since v0.17
	struct AdornmentFace {
		// The four corners, wound so consecutive pairs are edges.
		core::Vector3 Corners[4];

		// What colour to fill it, and how solid. `SelectionBox.SurfaceColor3`
		// and `.SurfaceTransparency`.
		//@{
		core::Color3 Colour;
		float Transparency = 0.0f;
		//@}

		// Whether it draws over the world rather than being occluded by it.
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

		// The filled faces, valid until the next `Build`.
		//
		// **Empty unless something asked for one**, which is the ordinary case:
		// a `SelectionBox` at its default `SurfaceTransparency` of 1 fills
		// nothing, so a drawer that only wants outlines pays one empty vector.
		//
		// @since v0.17
		const std::vector<AdornmentFace> &Faces() const {
			return Fills;
		}

	  private:
		void AddBox(
			const core::CFrame &frame,
			const core::Vector3 &halfExtent,
			const AdornmentLine &style,
			const AdornmentFace *fill
		);

		std::vector<AdornmentLine> Segments;
		std::vector<AdornmentFace> Fills;
	};
}
