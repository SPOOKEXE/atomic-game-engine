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
//   2. the camera looks back along the face normal, square on to the pane rather
//      than at its centre — see below, because the difference is not decorative;
//   3. the near plane is pushed out to the glass, which is the poor-man's
//      oblique clip: everything between the reflected camera and the pane — the
//      frame, the back of the pane, whatever the viewer is standing behind —
//      would otherwise occlude the reflection;
//   4. the field of view is fitted to the pane's four corners, because no
//      constant one covers it — the camera stands as far behind the glass as the
//      viewer stands in front, so the pane subtends the same angle from the
//      camera as from the viewer, and that grows without bound as somebody walks
//      up to a mirror;
//   5. the part is told which surface it shows, so a mirror is a camera parented
//      to a part and nothing else.
//
// **Steps 2 and 4 are one bug, and it is worth naming because a still frame does
// not show it.** The reflection is projected back onto the pane per fragment and
// `opaque.frag` tests the projected coordinate against the texture's 0..1
// rectangle, falling through to the plain lit pane outside it. So a frustum that
// does not cover the pane does not produce a smaller or a softer image — it
// produces a hard-edged rectangle of reflection floating on a grey wall, which
// moves and resizes as the viewer walks. That reads as a mirror aimed at the
// wrong thing, and it is what a fixed 70° field of view did from anywhere closer
// than a few pane-widths.
//
// Aiming along the normal is what makes the fit possible at all. An aim at the
// pane's centre tilts the view axis by however far the viewer stands to one
// side, and once the viewer is close *and* off-centre the nearest corner of the
// pane falls behind the camera — which no field of view covers. Looking along
// the normal puts every point of the pane at one depth, so the corners are
// always in front and the angle needed is always finite. It costs nothing: the
// image is read back through this camera's own matrix, so its orientation
// decides which texels the pane lands on and never which part of the world it
// shows. The position is what makes it a reflection.
//
// **Step 5 is what makes this an instance rather than a configuration.** Setting
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
// The oblique case is approximate, in two ways worth telling apart.
//
// A real planar reflection skews the projection's near plane onto the mirror,
// which handles a viewer looking at the pane from the side; this clips at a
// plane parallel to the face instead, so a steep angle clips slightly too much.
//
// And the fitted frustum is **symmetric about the face normal**, so a viewer
// off to one side pays for the far edge of the pane on both sides — the texture
// covers twice the width it needs and the mirror is drawn at half the resolution
// it could be. An off-axis frustum fits the same four corners with none of that
// waste and is the right shape here; it needs `render::SurfaceView` to carry the
// pane's rectangle rather than a field of view, which is the change this is
// waiting on. Correctness is not what is being traded — the coverage is exact
// either way — only texels.
//
// Neither is escapable by widening. A pane subtends half a turn from a point on
// its own surface, so walking into the glass asks for a 180° frustum; the fit
// clamps just short of that and the far corners stop being covered. That is the
// geometry rather than the parameterisation, and an off-axis frustum has the
// same limit.
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
	// **A pane the viewer is level with draws nothing at all**, which is a
	// statement about what a surface view means rather than about arithmetic.
	// Which way along its normal a reflected camera looks depends on which side
	// of the plane the viewer is, both answers are right, and no continuous path
	// joins them — so crossing the plane turned the camera 180 degrees between
	// two frames, once per orbit, which is what a mirror flashing is. A pane seen
	// edge-on covers no pixels, so the honest answer is that there is nothing to
	// show; its camera is left where it was and its pane is taken off its slot.
	// `EDGE_ON_MARGIN` in the source carries the width and its limits.
	//
	// @param store The world.
	// @return How many surface cameras produced a reflection. Zero is the
	//         ordinary case in a scene with no mirror in it, and is not a
	//         failure. A pane that was visited and blanked for being edge-on is
	//         not counted, because the caller is asking whether there is a
	//         reflection rather than whether there is a mirror.
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
