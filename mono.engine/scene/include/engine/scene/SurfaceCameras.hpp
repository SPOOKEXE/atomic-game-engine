#pragma once

// Where a surface camera stands, worked out from the face it is parented to.
//
// **The reflection used to be the script's arithmetic and is now the engine's,
// and that is the whole point of this file.** `Mirrors-1-world.luau` computed
// its own reflected position — mirror the eye through the plane, look back at
// the pane, push the near plane out to the glass — which is correct and is also
// thirty lines of vector maths every author would have to get right again. Worse,
// it has to be recomputed whenever *either* the eye or the pane moves, so a
// mirror authored that way is only correct for the frame it was written for: the
// example pinned its viewer in place precisely because a moving eye made the
// reflection wrong, and that limitation was mistaken for a broken mirror more
// than once.
//
// A `SurfaceCamera` parented to a `BasePart` with a `Face` is placed here
// instead, every frame, from wherever the viewer actually is.
//
// ## What planar reflection is, in four steps
//
// The face gives a plane: its outward normal rotated into the world, and a point
// on it at the part's half-extent along that axis. Then
//
//   1. the eye is mirrored through the plane — same distance behind, opposite
//      side;
//   2. the camera looks back at the middle of the face, so the projection lines
//      up with the pane rather than sliding across it;
//   3. the near plane is pushed out to the glass, which is the poor-man's
//      oblique clip: everything between the reflected camera and the pane — the
//      frame, the back of the pane, whatever the viewer is standing behind —
//      would otherwise occlude the reflection;
//   4. the part is told which surface it shows, so a mirror is a camera parented
//      to a part and nothing else.
//
// **Step 4 is what makes this an instance rather than a configuration.** Setting
// `Visual::Surface` by hand as well as parenting the camera is one fact recorded
// twice, and the failure mode is a mirror that renders perfectly into a texture
// nothing samples — which looks exactly like a mirror that does not work.
//
// ## What it is not
//
// The projection is **planar**: right for a flat face, wrong for anything
// curved. A general reflection needs a cube map or a screen-space trace and
// neither belongs in a pipeline this size.
//
// The oblique case is approximate. A real planar reflection skews the
// projection's near plane onto the mirror, which handles a viewer looking at the
// pane from the side; this clips at a plane parallel to the face instead, so a
// steep angle clips slightly too much. That is the head-on case done correctly
// and the oblique case done approximately, stated rather than discovered.
//
// **A camera with no `BasePart` parent is left exactly where it was put.** That
// is the script-authored arrangement `Mirrors-1-world.luau` used and it still
// works — this adds a way to place a surface camera, it does not take one away.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Places every surface camera that is parented to a part, and points its
	// part at it.
	//
	// Runs in `PreRender`, beside `SyncRendered`, and for the same reason: it is
	// presentation state derived from the tree, and a world can present without
	// ticking — the studio suspends a world and edits it, so a mirror maintained
	// by the simulation would stop tracking the moment somebody pressed Stop.
	//
	// **Reads the live camera and writes the surface one**, so the order within
	// the phase does not matter: the viewer is never a surface camera, because a
	// surface camera renders into a texture and `ActiveCamera` is what the screen
	// is drawn from. A world that named a surface camera as its active one would
	// be drawing the screen from inside the mirror, which is a scene mistake
	// rather than something to guard here.
	//
	// @param store The world.
	// @return How many surface cameras were placed. Zero is the ordinary case in
	//         a scene with no mirror in it, and is not a failure.
	size_t AimSurfaceCameras(ecs::Store &store);
}
