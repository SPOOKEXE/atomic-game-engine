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
#include <engine/scene/DrawInstance.hpp>

#include <cstddef>
#include <vector>

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
	// **One aim per world, so a host showing one world from several places has
	// to aim between the draws**, and it has to keep the textures apart as well.
	// The aim is world state and is only correct for the eye `ActiveCamera` named
	// when this ran; the *texture* that eye produced outlives the frame, so a
	// caller drawing the world twice out of one set of surface textures gets the
	// last panel's reflection in every panel. `render::Renderer::Render` takes a
	// viewport slot and keeps a surface set per slot for exactly that reason —
	// see its `targetSlot` — and `mono.studio` draws one panel per frame, aiming
	// immediately before each. Nothing here can enforce that: this pass has no
	// idea how many places its answer is about to be drawn from.
	//
	// @param store The world.
	// @return How many surface cameras were placed. Zero is the ordinary case in
	//         a scene with no mirror in it, and is not a failure.
	size_t AimSurfaceCameras(ecs::Store &store);

	// Appends a thin translucent bar lying on each face a surface camera
	// projects off.
	//
	// **Because "which face" is the one thing about a mirror you cannot see.**
	// Everything else this file computes shows up in the image: a camera aimed
	// wrongly reflects the wrong thing, a near plane set wrongly clips. `Face`
	// is different — the wrong one gives a pane that reflects what is *behind*
	// it, which looks exactly like a pane that reflects nothing, and the only
	// way to tell those apart was to read the script. The bar marks the side the
	// projection comes off, so the answer is in the frame.
	//
	// **A draw instance rather than an entity**, and that is what keeps it a
	// marker instead of content. Nothing is added to the world: it does not
	// serialise, it cannot be selected in an explorer, a script cannot find it,
	// and it disappears the moment the caller stops asking. An entity per mirror
	// would be a part every author has to know to ignore, and one somebody would
	// eventually save into a game file.
	//
	// Translucent and un-shadowed for the same reason: it must not change what
	// it is there to show you. **The translucency also keeps it out of the
	// mirrors**, which is load-bearing rather than incidental: the surface pass
	// draws `ScenePlan::Reflected` — the opaque head — so a blended instance
	// never reaches a surface texture, and a bar drawn opaque would appear
	// across the glass in the reflection of every other mirror in the scene.
	//
	// @param store The world.
	// @param out   The draw list to append to. Nothing already in it is touched.
	// @return How many markers were appended, which is one per surface camera
	//         that is parented to a part.
	size_t AppendSurfaceFaceMarkers(ecs::Store &store, std::vector<DrawInstance> &out);
}
