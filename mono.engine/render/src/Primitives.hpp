#pragma once

// The rectangles and quads the passes work in, decided without a device.
//
// **`render/AGENTS.md` states the rule this file exists for: geometry that can
// be asserted without a GPU should live where a test can reach it.**
// `AdornmentGeometry.hpp` was its only example, and the same section pointed at
// a `Primitives.hpp` checked by `tests/Primitives.cpp` that did not exist -
// recorded as a gap in `docs/ARCH_REVIEW.md` B. This is that file.
//
// **What the gap actually was is worth stating, because B named it wrongly.**
// B said the built-in shapes were back inside `src/Renderer.cpp` and
// `src/InterfacePass.cpp` with nothing checking their winding. They are not:
// the shapes are `assets::MakeBuiltin` and their winding is checked by
// `assets/tests/Builtin.cpp`, which asserts every triangle against its declared
// normal. What *was* unchecked in those two files is the arithmetic below - a
// shadow atlas quadrant written out three times, and the quad a spatial canvas
// occupies - and neither of those is a shape.
//
// **Private rather than public, unlike `AdornmentGeometry.hpp`.** That header
// is public because `mono.studio` draws adornments through it; nothing outside
// this module needs a beam quadrant or a canvas quad. A module's own tests may
// reach its `src/`, which is what `mono_add_tests` exists to allow, so a
// private header is still a tested one.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

namespace engine::render {

	// One slot's share of a square atlas, in texels and in the 0..1 window a
	// shader looks the atlas up through.
	//
	// **Both, from one function, because the two used to be written out
	// separately three lines apart.** The portal beam pass set a viewport, a
	// scissor and a `Beams.Region` uniform from the same `index % 2` and
	// `index / 2`, so a change to the packing had to be made in three places and
	// a beam that disagreed with its own lookup samples a neighbour's depth -
	// which reads as a shadow crossing the wrong doorway.
	struct AtlasQuadrant {
		// The rectangle in texels, for the viewport and the scissor.
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;

		// The same rectangle as a texture-coordinate window: `xy` scales a 0..1
		// coordinate into the quadrant and `zw` offsets it there.
		glm::vec4 Window{0.0f, 0.0f, 0.0f, 0.0f};
	};

	// Which quadrant of a square atlas one slot occupies.
	//
	// Two by two in reading order: slot 0 top-left, 1 top-right, 2 bottom-left,
	// 3 bottom-right. An index past the fourth wraps, because the caller has
	// already clamped its count and a quadrant is better than a viewport off the
	// edge of the texture.
	//
	// @param index      Which slot, 0..3.
	// @param resolution The atlas' side in texels.
	// @return The quadrant, in texels and as a lookup window.
	AtlasQuadrant BeamQuadrant(uint32_t index, uint32_t resolution);

	// A flat quad in the world: one corner and the two edges leaving it.
	//
	// The interface shader `interface_spatial.vert` places a collector pixel at
	// `Origin + AxisX * u + AxisY * v` for `u`, `v` in 0..1, so the axes are full
	// spans rather than directions and `Corner` states the same arithmetic where
	// a test can reach it.
	struct SpatialQuad {
		core::Vector3 Origin;
		core::Vector3 AxisX;
		core::Vector3 AxisY;

		// Which way the quad faces, which is **not** `AxisX x AxisY`. A canvas is
		// laid out in interface pixels, so `AxisY` runs *down* the image, and the
		// cross product of the two therefore points away from the viewer. See
		// `tests/Primitives.cpp`, which asserts the sign rather than assuming it.
		core::Vector3 Normal;

		// One corner, in the shader's own coordinates.
		//
		// @param u Across, 0 at the origin edge and 1 at the far one.
		// @param v Down, 0 at the top edge and 1 at the bottom one.
		// @return Where that corner is in the world.
		core::Vector3 Corner(float u, float v) const {
			return Origin + AxisX * u + AxisY * v;
		}

		// The four corners in the order `(0,0)`, `(1,0)`, `(0,1)`, `(1,1)`.
		//
		// @return Top-left, top-right, bottom-left, bottom-right.
		std::array<core::Vector3, 4> Corners() const {
			return {Corner(0.0f, 0.0f), Corner(1.0f, 0.0f), Corner(0.0f, 1.0f), Corner(1.0f, 1.0f)};
		}
	};

	// Whether a canvas fixed to a face is turned towards a viewer.
	//
	// **A canvas seen from behind is skipped rather than drawn mirrored.** Its
	// interface is authored for one side and the pixels have no back.
	//
	// @param normal   Which way the canvas faces.
	// @param toViewer From the canvas to the eye, unnormalised.
	// @return `true` when the eye is in front of it. Exactly edge-on is behind,
	//         because a canvas with no area on screen is not worth a draw.
	bool CanvasFacesViewer(const core::Vector3 &normal, const core::Vector3 &toViewer);

	// How many canvas pixels one stud subtends at a point.
	//
	// **Against the canvas' own height, not the attachment's.** The number this
	// divides is a `UDim2` offset, which is in canvas units - and
	// `ResolveSpatialCanvases` sized the canvas with `PixelsPerStud(...,
	// screen.Height)`, also canvas units. Using the attachment made the two
	// disagree by the display's density, so the pixel half of a billboard's
	// `Size` came out at a fraction of the studs asked for on a high-density
	// display and nowhere else.
	//
	// @param viewProjection What the frame is projecting with.
	// @param anchor         Where the canvas hangs.
	// @param up             One stud up, in world space.
	// @param canvasHeight   The canvas' height in its own pixels.
	// @return Pixels per stud, floored at a hair above zero so a caller can
	//         divide by it. An anchor behind the eye gives a small number rather
	//         than a negative one, because a billboard behind the viewer is
	//         culled by the projection rather than by its size.
	float CanvasPixelsPerStud(
		const glm::mat4 &viewProjection,
		const core::Vector3 &anchor,
		const core::Vector3 &up,
		float canvasHeight
	);

	// The quad a billboard canvas occupies: square to the screen, centred on its
	// anchor, and sized in studs plus pixels.
	//
	// @param anchor         Where the billboard hangs. The quad is centred here.
	// @param right          The camera's right, one stud long.
	// @param up             The camera's up, one stud long.
	// @param toCamera       From the anchor to the eye, unnormalised.
	// @param studs          The size in studs.
	// @param pixels         The size in canvas pixels, converted through
	//                       `pixelsPerStud`.
	// @param pixelsPerStud  What `CanvasPixelsPerStud` answered.
	// @param towardsDefault Which way to face when the eye is exactly on the
	//                       anchor and `toCamera` has no direction. The camera's
	//                       backward vector is what the pass hands over.
	// @return The quad, with `Normal` pointing at the eye.
	SpatialQuad BillboardQuad(
		const core::Vector3 &anchor,
		const core::Vector3 &right,
		const core::Vector3 &up,
		const core::Vector3 &toCamera,
		const core::Vector2 &studs,
		const core::Vector2 &pixels,
		float pixelsPerStud,
		const core::Vector3 &towardsDefault
	);
}
