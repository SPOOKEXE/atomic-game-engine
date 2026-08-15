#pragma once

// The pixel size of a collector that lives in the world rather than on a screen.
//
// **This is `D00022`, and it is the same split `AdornmentGeometry` already
// makes one dimension up.** A `SurfaceGui` sized in pixels-per-stud needs the
// stud extent of the face it is projected onto, which is `scene::Bounds`; a
// `BillboardGui`'s scale is in studs against the screen it lands on, which is a
// fact about a camera and a viewport. `gui` is L7 `shared` and links neither -
// `gui/AGENTS.md` refuses the edge - so it declares `gui::SpatialCanvas` and
// stops there.
//
// This module links `gui` and `scene` both, which is what that entry meant by
// "one multiplication, in the module that has both operands".
//
// ## Why it writes a component instead of returning a number
//
// `gui::Layout` walks every collector itself and decides the canvas per kind.
// Handing it a callback would put a `client` type in a `shared` signature, and
// handing it a map would make the layout pass take an argument that is empty in
// every headless caller. A component the layout *prefers when present* costs a
// lookup per collector - of which a scene has a handful - and leaves the
// headless path exactly as it was.
//
// ## Order
//
// Runs **before** `gui::Layout` in a frame, and after `scene::ResolveActiveCamera`
// so the camera it reads is this frame's. Running it after the layout would draw
// one frame at the previous canvas, which on a billboard the player is walking
// towards is a visible lag on the size of everything inside it.
//
// **Headless.** Bounds, a distance and a tangent - no device, so it is testable
// without one.
//
// @tier L12 · client

#include <engine/gui/Layout.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Writes `gui::SpatialCanvas` on every `SurfaceGui` and `BillboardGui`.
	//
	// **Nothing is written for a collector whose canvas cannot be resolved**,
	// rather than a zero or a guess. A `SurfaceGui` on a `Folder` has no
	// `scene::Bounds` and a billboard in a world with no live camera has no
	// distance - in both cases the authored pixel size is the better answer, and
	// leaving the component off is how `gui::CanvasFor` is told to use it.
	//
	// Stale components are cleared for the same reason: a `SurfaceGui` switched
	// back to `FixedSize` must stop being sized by its adornee on the very next
	// frame, and a resolved canvas nobody refreshed is exactly the sort of thing
	// that keeps working until a part is deleted.
	//
	// @param store  The world. Read for `gui::Surface`, `gui::Billboard`,
	//        `scene::Bounds`, `scene::Transform` and `scene::ActiveCamera`;
	//        written only through `gui::SpatialCanvas`.
	// @param screen The viewport being drawn into. Its *height* in pixels is what
	//        a vertical field of view converts studs against - the width is not
	//        used, because a wider window shows more of the world rather than
	//        making what is in it bigger.
	// @return How many collectors were given a canvas.
	size_t ResolveSpatialCanvases(ecs::Store &store, const gui::Screen &screen);
}
