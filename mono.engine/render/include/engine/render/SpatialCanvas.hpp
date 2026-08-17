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

#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	struct SpatialPointer {
		ecs::Entity Collector;
		core::Vector2 Position;
	};

	// Writes `gui::SpatialCanvas` on every `SurfaceGui` and `BillboardGui`.
	//
	// **Nothing is written for a collector whose placement cannot be resolved**,
	// rather than a zero or a guess. A `SurfaceGui` on a `Folder` has no
	// `scene::Bounds` and a billboard in a world with no live camera has no
	// projection. A fixed-size surface on a real part is resolved because its
	// authored pixels still need a world plane on which to draw.
	//
	// Stale components are cleared for the same reason: a collector whose
	// adornee disappears must stop using the last plane resolved for it.
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

	// Projects a window pixel onto the foremost interactive spatial collector.
	// Screen interfaces are deliberately not considered; a caller gives those
	// first refusal with `gui::PickScreen`.
	bool ResolveSpatialPointer(
		ecs::Store &store,
		const gui::DrawList &list,
		const gui::Screen &screen,
		const core::Vector2 &point,
		SpatialPointer &out
	);
}
