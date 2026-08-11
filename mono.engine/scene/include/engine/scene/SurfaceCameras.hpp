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
//   3. the near plane is skewed onto the pane's own plane — a real oblique
//      clip, so everything between the reflected camera and the pane (the
//      frame, the back of the pane, whatever the viewer stands behind) is
//      dropped at any angle rather than approximately;
//   4. an off-axis frustum is fitted to the pane's four corners, because no
//      constant one covers it — the camera stands as far behind the glass as
//      the viewer stands in front, so the pane subtends the same angle from the
//      camera as from the viewer, and that grows without bound as somebody
//      walks up to a mirror;
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
// wrong thing, and it is what a fixed 70 degree field of view did from anywhere
// closer than a few pane-widths.
//
// Aiming along the normal is what makes the fit possible at all. An aim at the
// pane's centre tilts the view axis by however far the viewer stands to one
// side, and once the viewer is close *and* off-centre the nearest corner of the
// pane falls behind the camera — which no frustum covers. Looking along the
// normal puts every point of the pane at one depth, so the corners are always in
// front and the fit is always finite. **Leaning is the frustum's job**, which is
// exactly what an off-axis one is for: the four edges move independently, so a
// viewer off to one side gets the pane and not twice its width. It costs nothing
// in correctness — the image is read back through this camera's own matrix, so
// its orientation decides which texels the pane lands on and never which part of
// the world it shows. The position is what makes it a reflection.
//
// ## A portal is step 1 with a different map
//
// Everything above generalises, and `Portal` is what uses it. Replace "mirror
// the eye through the pane's plane" with "map it through `destination · source⁻¹`,
// with a half-turn so the camera looks out of the far side", and the other four
// steps are unchanged: the same aim along the mapped normal, the same oblique
// clip at the mapped plane, the same off-axis fit to the mapped corners, the
// same slot handed to the same pane.
//
// **The rectangle that is fitted and clipped against is the mapped *source*
// pane, never the destination part.** `opaque.frag` shades a fragment of the
// source pane by projecting it through this camera's matrix, and that only lines
// up because the camera and the rectangle moved together. Fitting to the
// destination would be right exactly when the two panes are the same size, and
// silently wrong — an image sliding across the hole — whenever they are not.
//
// A mirror is the same code with the map's fixed points doing the work: a
// reflection fixes every point of the plane it reflects through, so the mapped
// rectangle *is* the pane and the mapped plane *is* its plane.
//
// **Nothing constrains the two frames to describe one space.** That is not a
// loophole, it is the feature: a destination turned, moved or scaled anywhere
// gives a room bigger on the inside or a corridor that turns through more than
// four right angles, with no second mechanism and no maths past a matrix
// multiply. `docs/NON-EUCLIDEAN.md` is the investigation, `D00112` is what
// remains — traversal, which needs a body to move, and in-frame recursion, which
// is what would remove the one-frame seam on the frame somebody crosses.
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
// **Two approximations that used to be here are gone, and what replaced them is
// worth naming because the old text described them as permanent.** The clip was
// a near plane pushed out *parallel* to the face, which over-clipped at a
// grazing angle; it is a real skew onto the plane now, which a portal cannot do
// without — its destination is set into a wall, and the wall would draw across
// the hole. And the fit was **symmetric about the face normal**, so a viewer off
// to one side paid for the far edge of the pane on both sides and the mirror was
// drawn at half the resolution it could be; it is off-axis now. Correctness was
// never what the second one traded — the coverage was exact either way — only
// texels.
//
// **What no frustum escapes** is the geometry rather than the parameterisation.
// A pane subtends half a turn from a point on its own surface, so walking into
// the glass asks for something no projection covers. `EDGE_ON_MARGIN` is what
// turns that into a surface that stops drawing rather than a matrix full of
// infinities, and an off-axis frustum has exactly the same limit — it simply no
// longer has to clamp on the way there, which is what used to make the fit a
// step function and read as the mirror flashing.
//
// **A portal cannot be walked through.** Seeing through a hole is half the
// feature and moving a body through it is the other; there is no body until the
// character controller lands. And a surface's texture is a frame old, so the
// portal chain resolves over frames rather than within one — invisible on a
// mirror, a seam on the frame somebody crosses. `D00112` carries both.
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

	// Moves anything that walked into a portal out of the pane it leads to.
	//
	// **`D00112`, and the character controller is what it was waiting for.** A
	// portal has drawn correctly since v0.14 and nothing could go through one,
	// because nothing in this engine had a body to move — the deferral says so
	// in as many words. This is the other half, and it is four lines of
	// arithmetic because the hard part was already done: the transform that maps
	// the source pane onto the destination is the *same* one `AimSurfaceCameras`
	// puts the camera through, so a body crossing is multiplied by exactly what
	// the picture was.
	//
	// **Derived here rather than read off `SurfaceLens::Mapping`.** That
	// component is presentation — it is fitted to the local eye and
	// `replication::LocalToTheClient` keeps it off the wire — so a dedicated
	// server, which never aims a surface at all, has no lens to read. Traversal
	// is simulation and must not depend on anything a headless host skips.
	//
	// **A crossing is a segment, not a position**, which is what makes it work
	// at speed. A character walking at sixteen metres a second covers a quarter
	// of a metre a tick, and a test that asked "is it inside the pane now" would
	// miss the tick it was on either side of. `PreviousTransform` is where it
	// was when the tick started, and the test is whether the line between the
	// two changes sign through the pane's plane inside the pane's rectangle.
	//
	// **Velocity is mapped too, and forgetting it is the bug that looks like
	// physics.** The body arrives at the far pane with the speed it had, aimed
	// the way it was aimed — in the old frame. Without the rotation it walks out
	// of the destination sideways, or backwards through the hole it just came
	// out of, which reads as the portal spitting people back.
	//
	// **Runs in `PostSimulation`, after the solver has moved the body**, so what
	// is tested is where the tick actually ended rather than where it was
	// heading. `physics::RegisterCharacterSystems` installs it.
	//
	// @param store The world.
	// @return How many bodies crossed. Zero in every scene with no portal in it.
	// @since v0.14
	size_t CrossPortals(ecs::Store &store);

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
