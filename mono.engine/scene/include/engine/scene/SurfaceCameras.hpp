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
// ## A *cross-world* portal is step 1 with a different map
//
// Everything above generalises, and `Portal` is what uses it. Replace "mirror
// the eye through the pane's plane" with "map it through `destination · source⁻¹`,
// with a half-turn so the camera looks out of the far side", and the other four
// steps are unchanged: the same aim along the mapped normal, the same oblique
// clip at the mapped plane, the same off-axis fit to the mapped corners, the
// same slot handed to the same pane.
//
// **A same-world portal stopped being one of these at v0.15, and the reason is
// worth stating here because this file argued the opposite at length.** Every
// camera placed by this pass is a function of *the eye*. That is exactly right
// for a reflection and it is what a hole cannot survive: when one surface pass
// draws another surface's pane it projects that pane's image with the second
// camera's own matrix, which was placed from the eye rather than from the camera
// the pass is rendering from. A portal seen through a portal is therefore drawn
// from the wrong **viewpoint**, not merely from a frame-old texture, and no
// number of bounces touches it.
//
// So a hole in *this* space is `render::PortalView` and a recursive render pass,
// whose sub-camera is derived from whichever camera the recursion is currently
// at — the warp applied to that camera's own frame, that camera's own projection
// skewed onto the mapped pane. Warps then compose down the recursion by
// construction. `NON-EUCLIDEAN.md`'s Part III is the whole argument and
// `temp/NonEuclidean`'s `Portal.cpp` is the model.
//
// **What is left on this path is the cross-world pane**, and deliberately: a
// `Portal::DestinationWorld` is a *window onto a second simulation* rather than a
// hole in one space, so a warp into it is a stated frame and not a derived one.
// It keeps its camera, its fit and its slot, and it does not recurse.
//
// This pass still places a camera for a same-world portal and still hands it a
// slot, because the slot is how a pane is named and `client::CollectPortalViews`
// reads `PortalSeam::Surface` to find the pane's draw run. What it no longer does
// is reach a screen: `client::CollectSurfaceViews` drops any camera whose slot a
// portal claimed.
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
// multiply. `NON-EUCLIDEAN.md` is the investigation.
//
// **"Scaled" is `SeamTransform`, and it was the one word that document oversold
// for a version.** The map used to be a `CFrame` — a position and a rotation —
// so a pair of panes of different sizes drew a source-sized window onto a
// full-sized room and a body walked out of it the size it went in. The map
// carries the ratio of the two panes now, and the camera, the pane's sampling
// matrix, a crossing body, a clone, a ghost, the camera arm and a
// portal-crossing ray all go through it.
//
// `D00112` closed at v0.15 on the bounce loop, and the seam it left behind — a
// hole seen through a hole, drawn from the eye rather than from the camera the
// pass is rendering from — is the recursive portal pass'. See the section above.
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
// the glass asks for something no projection covers. For a mirror
// `EDGE_ON_MARGIN` is what turns that into a surface that stops drawing rather
// than a matrix full of infinities; for a portal, which is walked through on
// purpose, the three floors that keep the matrix finite — `MINIMUM_DEPTH`,
// `FIT_MINIMUM_SPAN` and `SurfaceProjection`'s refusal to skew against a plane
// the camera is on — are what carry it across. An off-axis frustum has exactly
// the same limit either way; it simply no longer has to clamp on the way there,
// which is what used to make the fit a step function and read as the mirror
// flashing.
//
// **A portal can be walked through.** Traversal is `CrossPortals`, the body's
// half, with the eye carried by `PortalCrossing` and the yaw by `PortalTransit`
// — and none of it knows what a `SurfaceCamera` is, which is why none of it
// changed when the picture moved to a pass of its own.
//
// **A cross-world pane is still a frame behind, and now it is the only thing
// that is.** Its texture is written by the surface pass and read on the next
// one; a same-world hole resolves inside the frame, deepest first.
//
// **A camera with no `BasePart` parent is left exactly where it was put.** That
// is the script-authored arrangement `Mirrors-1-world.luau` used and it still
// works — this adds a way to place a surface camera, it does not take one away.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstddef>
#include <span>
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
	// **A *mirror* the viewer is level with draws nothing at all**, which is a
	// statement about what a surface view means rather than about arithmetic.
	// Which way along its normal a reflected camera looks depends on which side
	// of the plane the viewer is, both answers are right, and no continuous path
	// joins them — so crossing the plane turned the camera 180 degrees between
	// two frames, once per orbit, which is what a mirror flashing is. A pane seen
	// edge-on covers no pixels, so the honest answer is that there is nothing to
	// show; its camera is left where it was and its pane is taken off its slot.
	// `EDGE_ON_MARGIN` in the source carries the width and its limits.
	//
	// **A linked portal is exempt, and the exemption is not a tolerance.** The
	// flip cancels for a hole: the frame the viewer's side changes is the frame
	// the viewer is carried through the pane, so `through · eye` is continuous
	// and there is no discontinuity to blank. Blanking it anyway put a dark
	// frame exactly in the doorway, on the one move a portal is for.
	//
	// @param store The world.
	// @return How many surface cameras produced a reflection. Zero is the
	//         ordinary case in a scene with no mirror in it, and is not a
	//         failure. A pane that was visited and blanked for being edge-on is
	//         not counted, because the caller is asking whether there is a
	//         reflection rather than whether there is a mirror.
	size_t AimSurfaceCameras(ecs::Store &store);

	// One mirror pane, as a rectangle in the world.
	//
	// **`PortalSeam`'s twin, and the two are deliberately not one type.** A seam
	// describes a hole and carries what only a hole has — where the far end is,
	// how much bigger it is, whether it crosses worlds — and a reflection has
	// none of that, because its map has no far end at all. What the two share is
	// the rectangle, and sharing exactly that is what stops a mirror acquiring a
	// destination field that is always null.
	//
	// **Gathered rather than read off a component**, for the reason a seam is: a
	// pane is a `SurfaceCamera`, plus the part it is parented to, plus the face
	// named on it — so a pass that wanted one would otherwise walk the tree and
	// re-derive `ReachOf` and the face's two axes for itself. This file already
	// paid for that mistake one level down, where a marker drawn on a face the
	// camera was not projecting off is a debugging aid that lies.
	//
	// @since v0.15
	struct SurfacePane {
		// The pane's plane: the middle of the face, and the face's own unit
		// normal.
		//
		// **The face's normal, not "the one pointing at the viewer".** Which
		// side is outward is a question about the viewer, exactly as it is about
		// the crosser in `PortalSeam` — a pane can be looked at from either side
		// and both answers are right. `ReflectCamera` takes the side from the
		// viewer it is handed, which is what lets one pane answer differently at
		// two levels of a recursion.
		core::Vector3 Centre;
		core::Vector3 Normal;

		// The pane's half-axes in the world, so `Centre ± First ± Second` is the
		// rectangle. Vectors rather than extents, because that is the form the
		// fit and the clip both want.
		core::Vector3 First;
		core::Vector3 Second;

		// The part the face is on, and the surface camera that projects off it.
		//
		// **Both ends, because the pairing is written from one number.** The
		// camera carries the slot it renders into and the part carries the slot
		// it samples; a pass holding one of them could set only half of that,
		// and half of it is a camera rendering perfectly into a texture nothing
		// samples.
		ecs::Entity Part = ecs::NULL_ENTITY;
		ecs::Entity Camera = ecs::NULL_ENTITY;

		// Which surface slot the pane samples, from `SurfaceCamera::Surface`.
		// What lets a caller name one mirror out of several.
		int8_t Surface = 0;

		// Which tags an instance must carry to appear in this pane, or zero for
		// all of them, from `SurfaceCamera::TagFilter`.
		//
		// **Carried for `PortalSeam::TagFilter`'s reason.** A recursive pass has
		// no surface camera in its hand at the levels below the first — it has
		// the pane it is descending into — so a filter authored on the camera
		// would be silently dropped for exactly the mirrors the recursion is
		// there to draw.
		uint32_t TagFilter = 0;

		// The lens the author gave the camera. `ReflectCamera` measures its
		// extents at `NearPlane` and hands both back untouched.
		//
		// **The author's and not the engine's, which is a v0.14 change this must
		// not quietly undo.** The near plane used to be shoved out to the glass
		// as a poor man's oblique clip, so a script that set it had it taken back
		// on the next frame; there is a real clip now and these are read rather
		// than written.
		float NearPlane = 0.1f;
		float FarPlane = 500.0f;
	};

	// The camera one mirror pane must be rendered from, for one viewer.
	//
	// @since v0.15
	struct MirrorEye {
		// Where it stands and which way it looks, ready for a `Transform`.
		core::CFrame Frame;

		// The frustum fitted to the pane, the oblique clip on the pane's own
		// plane, and the identity map a mirror samples through — ready for a
		// `SurfaceLens`.
		SurfaceLens Lens;

		// Whether there is a reflection to draw at all.
		//
		// **False when the viewer is inside the edge-on band**, where there is
		// no continuous orientation to aim for: which way along the normal a
		// reflected camera looks depends on which side of the plane the viewer
		// is, both answers are right, and no path joins them — so a viewer
		// crossing turns the camera half a turn between two frames, which is
		// what a mirror flashing is. A pane seen edge-on covers no pixels, so
		// the honest answer is that there is nothing to show.
		//
		// Carries the same meaning `AimSurfaceCameras`' own aim does: the frame
		// and the lens are left at their defaults and a caller that renders
		// anyway is drawing from a camera nothing placed.
		bool Renders = false;
	};

	// The four corner directions of an ordinary perspective camera's frustum, in
	// world space.
	//
	// **What the fit is intersected with, and the whole of a close pane's
	// sharpness.** A surface has to cover the part of its pane *the viewer can
	// see*, and up against the glass those are wildly different things: a pane
	// subtends nearly half a turn from a point on its own surface and a screen
	// subtends seventy degrees, so fitting the whole pane there spends almost
	// every texel outside the frame and the image goes blocky exactly when it is
	// largest.
	//
	// **One unit deep, which is a convention the two overloads share and must.**
	// The fit divides each direction by its own depth along the view axis, so a
	// corner's length cancels everywhere except against the floor that keeps a
	// corner swinging past the camera's plane finite — and two overloads
	// disagreeing about scale would disagree only there, which is the one place
	// nobody would look.
	//
	// **A little wider than the screen exactly needs**, for the same reason the
	// fit itself is: the edge of a frustum is not a safe place to sample, and a
	// clamp landing exactly on the screen edge puts the pane's visible boundary
	// on the texture's boundary.
	//
	// @param frame               The viewer's placement.
	// @param fieldOfViewRadians  Its vertical field of view.
	// @param aspect              Width over height of what it draws into. Zero
	//                            or less is taken as square, which is what a
	//                            minimised window reports.
	// @param out                 The four directions.
	// @return How many were written, which is four. A count so a caller can hand
	//         it straight to `ReflectCamera` as a span, and so a viewer with no
	//         frustum to speak of is expressible as zero.
	// @since v0.15
	size_t FrustumCorners(
		const core::CFrame &frame, float fieldOfViewRadians, float aspect, core::Vector3 (&out)[4]
	);

	// The same, for a camera whose frustum is an already-fitted off-axis lens.
	//
	// **Which is what every level of a mirror recursion past the first has.** A
	// reflected camera has no field of view — its extents were fitted to a pane
	// and possibly skewed — so the perspective overload has nothing to be handed,
	// and a recursion that fell back to "no corners" would drop the clamp at
	// precisely the levels where the pane is nearest and the texels scarcest.
	//
	// **The lens' own margin is not applied again.** These extents already
	// carry the widening the fit put on them, and widening a widened frustum
	// compounds once per level of the recursion.
	//
	// @param frame The camera's placement.
	// @param lens  Its fitted extents, measured at `SurfaceLens::NearPlane`.
	// @param out   The four directions, one unit deep as above.
	// @return Four.
	// @since v0.15
	size_t FrustumCorners(const core::CFrame &frame, const SurfaceLens &lens, core::Vector3 (&out)[4]);

	// Where a mirror pane's camera stands when the pane is looked at from
	// `viewer`, and what it sees through.
	//
	// **A function of the pane and the viewer and nothing else, which is the
	// whole point.** A mirror seen *inside* another mirror is looked at from
	// that mirror's camera rather than from the eye, so the rule has to compose
	// — and it can only compose if it is a function, rather than a walk over the
	// world's one active camera. While it was the latter, a pane appearing in
	// another pane's picture was placed and sampled from the eye at every depth,
	// which is `ROADMAP.md` v0.15's "mirror-in-mirror-in-mirror draws the inner
	// panes as flat tint": the coordinate leaves 0..1 and `opaque.frag` falls
	// back to the plain lit pane.
	//
	// **One statement of what a mirror does to a camera**, for the reason
	// `SeamMapping` is one statement of what a hole does to what goes through
	// it. `AimSurfaceCameras` calls this for its own mirrors, so a second
	// derivation cannot drift from it by a sign.
	//
	// The four steps are the ones this file's header describes: the viewer is
	// mirrored through the pane's plane, the camera looks back along the face
	// normal rather than at the pane's centre, an off-axis frustum is fitted to
	// the pane's four corners, and the near plane is skewed onto the pane's own
	// plane.
	//
	// @param pane          The rectangle and the lens to fit.
	// @param viewer        Where the pane is being looked at from. Only its
	//                      position decides the reflection — a mirror does not
	//                      care which way the viewer faces — but the whole frame
	//                      is taken because `viewerCorners` is measured in it
	//                      and a caller holding one holds the other.
	// @param viewerCorners The viewer's own frustum, as four world-space
	//                      directions from `FrustumCorners`. Empty leaves the
	//                      fit unclamped, which is the right answer for a viewer
	//                      that has no frustum and the wrong one for a viewer
	//                      that has one and did not pass it — the image is
	//                      correct either way and a close pane is drawn at a
	//                      fraction of the resolution it could be. Anything
	//                      other than four is treated as none, because a partial
	//                      frustum is not a frustum.
	// @return Where to put the camera and what to render it with, or an answer
	//         with `Renders` false in the edge-on band.
	// @since v0.15
	MirrorEye ReflectCamera(
		const SurfacePane &pane, const core::CFrame &viewer, std::span<const core::Vector3> viewerCorners
	);

	// Every mirror pane in the world: a `SurfaceCamera` parented to a `BasePart`
	// whose `Portal` does not name a live destination.
	//
	// **A linked portal is not one of these.** Its camera is a warp rather than
	// a reflection — `SeamMapping` maps the viewer through the pair instead of
	// mirroring it through one plane — and gathering it here would hand a
	// recursion two descriptions of the same pane that disagree about where the
	// camera goes. `GatherPortalSeams` is its half, and an *unlinked* portal is
	// a mirror by the same rule `AimSurfaceCameras` applies: a hole leading
	// nowhere is a wall.
	//
	// **Walk order, which is archetype order, and deliberately not sorted.**
	// `SurfacePane::Surface` is what names a pane, and it is stable across a
	// frame where the position in this list is not. Sorting here would be a
	// second ordering to keep in step with the one `AimSurfaceCameras` hands the
	// slots out in.
	//
	// @param store The world.
	// @param panes Cleared, then filled.
	// @return How many there are. Zero in every scene with no mirror in it.
	// @since v0.15
	size_t GatherSurfacePanes(ecs::Store &store, std::vector<SurfacePane> &panes);

	// One portal, as the rectangle a crossing is tested against.
	//
	// **One description of a hole, for the three passes that need one.** A body
	// walking through (`CrossPortals`), a body *half* through and therefore
	// drawn on both sides (`client::CollectInstances`), and a body half through
	// and therefore standing on both floors (`physics::GhostPortalBodies`) are
	// the same rectangle asked three questions. They disagreed the moment there
	// were three copies of it, and what that looks like is a character that
	// falls through a floor it is visibly standing on.
	//
	// @since v0.15
	struct PortalSeam {
		// The pane's plane: a point on it and its face's unit normal.
		//
		// **The face's normal, not "the outward one".** Which side is outward is
		// a question about the *crosser*, exactly as it is a question about the
		// viewer in `AimSurfaceCameras` — a pane can be walked into from either
		// side and both answers are right. `SeamMapping` takes the side.
		core::Vector3 Centre;
		core::Vector3 Normal;

		// The pane's half-axes in the world, so `Centre ± First ± Second` is the
		// rectangle. Vectors rather than extents because that is what the inside
		// test wants.
		core::Vector3 First;
		core::Vector3 Second;

		// The far pane's face frame, looking out of itself. The half of the
		// mapping that does not depend on who is crossing.
		core::CFrame Destination;

		// The pane this seam is the face of, and the part the far end is on.
		//
		// **Kept so a clone pass can skip them both.** A pane straddles its own
		// plane by definition, so cloning it through itself is a portal inside a
		// portal — which recurses in the picture and, worse, gives the solver a
		// second copy of the very surface it is deciding a crossing against.
		ecs::Entity Pane = ecs::NULL_ENTITY;
		ecs::Entity Far = ecs::NULL_ENTITY;

		// How much bigger the far pane is than this one.
		//
		// **A hole between two panes of different sizes is a change of scale,
		// and treating it as a rigid map is what makes a room bigger on the
		// inside a painting.** The map that puts the camera at the far end also
		// says how large the far end is; if it carries only a rotation and a
		// translation then a doorway twice the size shows the same room at the
		// same size through a bigger frame, and a body that walks through comes
		// out the size it went in. That is a picture of an impossible space
		// rather than one, and it is the difference `NON-EUCLIDEAN.md`
		// claimed and the implementation did not have.
		//
		// **The square root of the area ratio, which is the one definition that
		// does not have to pick an axis.** Two rectangles of the same shape give
		// exactly the ratio of their sides, which is what anybody would expect.
		// Two of *different* shapes are an ill-posed pair — there is no single
		// number that maps a tall rectangle onto a wide one — and this answers
		// with the one that is symmetric in the two axes rather than with
		// whichever the face's first axis happened to be. A pair like that
		// renders and traverses; it simply does not line up at the edges, and no
		// scalar could make it.
		//
		// One, exactly, for the ordinary pair of matching panes and for every
		// mirror. `SeamTransform` is where it is applied.
		//
		// @since v0.15
		float Scale = 1.0f;

		// Which tags an instance must carry to be drawn through this hole, or
		// zero for all of them, from `SurfaceCamera::TagFilter`.
		//
		// **Carried on the seam because the recursive portal pass has no camera
		// to read it off.** A same-world hole is drawn by `render::PortalView`
		// rather than by a surface camera, and the seam is the only description
		// of it that reaches the host — so a filter authored on the pane would
		// otherwise be silently dropped for exactly the portals that stopped
		// being surfaces.
		//
		// @since v0.15
		uint32_t TagFilter = 0;

		// Which surface slot the pane samples, from `SurfaceCamera::Surface`.
		// What lets a host name one portal out of several.
		int8_t Surface = 0;

		// Whether this portal names another world.
		//
		// **A cross-world portal's `Destination` is a camera stand-in and not a
		// place**, which is exactly the distinction `Portal::DestinationWorld`
		// documents. Cloning or moving a body through it would put them at the
		// stand-in — in *this* world, a metre behind the pane they were walking
		// into — instead of handing them to whoever owns the crossing.
		bool Crosses = false;
	};

	// The map from one side of a seam to the far side.
	//
	// **A similarity and not a rigid motion, which is the one thing that had to
	// change for a hole to be able to lie about size.** The rotation and the
	// translation are `destination · half-turn · source⁻¹`, exactly as before;
	// the scale is `PortalSeam::Scale` and it is taken about the *source* pane's
	// centre, which is the point the rigid map already sends to the destination's
	// centre. Scaling there rather than at the far end means the two halves
	// compose in either order and neither needs the other's centre.
	//
	// **Four ways to apply it, because four things go through a hole and they
	// are not the same kind of thing.** A position moves and scales; a velocity
	// or an offset rotates and scales; a unit direction only rotates, and
	// scaling it would quietly stop it being unit; a placement is a position and
	// a rotation. Every caller that got one of these wrong produced the same
	// symptom — a portal that works and leads somewhere slightly wrong — so they
	// are named rather than left to a multiply.
	//
	// @since v0.15
	struct SeamTransform {
		// The rigid half: `destination · half-turn · source⁻¹`. The half-turn is
		// what makes it a hole rather than a window onto a copy.
		core::CFrame Frame;

		// The point the scale is about, which is the source pane's centre.
		core::Vector3 Origin;

		// `PortalSeam::Scale`, or 1 for a mirror and for any matched pair.
		float Scale = 1.0f;

		// Where a point in this space lands in the far one.
		core::Vector3 Point(const core::Vector3 &at) const {
			return Frame.PointToWorldSpace(Origin + (at - Origin) * Scale);
		}

		// Where a direction points in the far space, keeping its length.
		//
		// **For anything that must stay unit** — a ray's direction, a normal.
		core::Vector3 Rotate(const core::Vector3 &of) const {
			return Frame.VectorToWorldSpace(of);
		}

		// The same, taking the scale with it.
		//
		// **For anything that is a length** — a velocity, an offset, a
		// half-extent. A body that comes out of a hole half the size it went in
		// and keeps its old speed is a body that crosses the far room in half
		// the time, which reads as the portal firing you out of it.
		core::Vector3 Carry(const core::Vector3 &of) const {
			return Frame.VectorToWorldSpace(of * Scale);
		}

		// Where a placement ends up: its position mapped, its rotation turned.
		core::CFrame Place(const core::CFrame &frame) const {
			return core::CFrame{Point(frame.Position), Frame.Rotation() * frame.Rotation()};
		}

		// How long a distance measured in this space is in the far one.
		float Length(float of) const {
			return of * Scale;
		}
	};

	// Builds the map through one seam.
	//
	// **One map for both sides, which is what makes it a hole.** It carries this
	// pane's front hemisphere to the far pane's back one and its back to the far
	// pane's front, and the far pane's own map is its exact inverse — so a round
	// trip is the identity whichever side it started from. Picking the map by
	// the crosser's side, which this used to do, gives two maps that both land
	// on the same side of the far pane and are therefore not inverses.
	//
	// @param seam The portal.
	// @return The map to put a placement, a velocity or a direction through.
	// @since v0.15
	SeamTransform SeamMapping(const PortalSeam &seam);

	// How far the seam's plane is from a point, signed along `Normal`.
	//
	// @param seam The portal.
	// @param at   The point.
	// @return Positive in front of the pane, negative behind it.
	// @since v0.15
	float SeamOffset(const PortalSeam &seam, const core::Vector3 &at);

	// How far a point is from a rectangle, as a solid rather than as a plane.
	//
	// **The rectangle and not its plane, which is the whole point.** A pane
	// stretched across a doorway is a metre from somebody standing beside the
	// doorway and a hair from somebody standing in it, and the plane cannot tell
	// those apart — it says zero for both. Everything downstream of this number
	// (`PortalNearPlane`, `PortalClipBias`) is trading precision away as the eye
	// closes on a hole, so answering "zero" for an eye that is nowhere near one
	// spends that precision on nothing.
	//
	// The closest point of the rectangle, found by clamping the projection onto
	// each half-axis into `-1..1` and measuring to what is left. CodeParade's
	// `Portal::DistTo`.
	//
	// @param centre The middle of the rectangle.
	// @param first  One half-axis, as a vector, so `centre ± first` is an edge.
	// @param second The other.
	// @param at     The point.
	// @return The distance, never negative.
	// @since v0.15
	float RectangleDistance(
		const core::Vector3 &centre,
		const core::Vector3 &first,
		const core::Vector3 &second,
		const core::Vector3 &at
	);

	// The same, for a gathered seam.
	//
	// @param seam The portal.
	// @param at   The point.
	// @return The distance from the hole itself, never negative.
	// @since v0.15
	float SeamDistance(const PortalSeam &seam, const core::Vector3 &at);

	// How far the nearest hole in the world is from a point.
	//
	// @param store The world.
	// @param at    The point, normally the eye.
	// @return The distance, or infinity in a world with no portals in it.
	// @since v0.15
	float NearestSeamDistance(ecs::Store &store, const core::Vector3 &at);

	// Smallest near plane a camera is allowed, in studs.
	//
	// **Depth precision has to be spent somewhere and this is where.** A near
	// plane is a floor on how close geometry can be drawn, so the pane of a hole
	// you are walking into is sliced open by it — you see through the wall for
	// the last hand's width of the approach, which is the one moment the whole
	// feature is judged on.
	constexpr float PORTAL_NEAR_MIN = 0.003f;

	// The near plane to actually draw with, given how close a hole is.
	//
	// **Half the distance, so the pane is never within the near plane and the
	// depth range is never cut further than it has to be.** CodeParade's
	// `GH_CLAMP(NearestPortalDist() * 0.5f, GH_NEAR_MIN, GH_NEAR_MAX)`, with the
	// authored value standing in for `GH_NEAR_MAX` — a camera the scene wanted
	// far-sighted keeps that until a hole is close enough to need otherwise, and
	// gets it straight back on the way out.
	//
	// **Derived rather than stored, and that is deliberate.** Writing this back
	// into `Camera::NearPlane` would destroy the authored value on the first
	// frame near a portal and there would be nothing left to return to. Every
	// caller passes the authored number in and gets the drawing number out, so
	// the component keeps meaning what the scene said.
	//
	// @param authored    `Camera::NearPlane`, the value with no hole nearby.
	// @param nearestSeam What `NearestSeamDistance` said, or infinity.
	// @return The near plane to build the projection with.
	// @since v0.15
	float PortalNearPlane(float authored, float nearestSeam);

	// How far a hole's oblique clip plane is moved back towards its camera.
	//
	// **Towards, so the plane keeps a little more and not a little less.** The
	// oblique substitution makes that plane the near plane, and the far room's
	// own geometry meets the mapped pane exactly — a floor that runs up to the
	// doorway, the wall the pane is set into — so after two matrix products some
	// of it lands a float on the wrong side and is thrown away. What that looks
	// like is a hairline of background around the inside of every hole with
	// parts poking through it. Keeping a sliver too much costs nothing anybody
	// can see; keeping a sliver too little is the artefact.
	//
	// **Pushing the plane the other way is what cuts a body in half.** Anything
	// standing in the seam is within a hair of the plane on both sides, so a
	// slab taken off the far room takes the far half of whoever is walking
	// through with it.
	//
	// Shrinks with the distance for the reason the near plane does: standing
	// with your nose against the pane, a fixed slab would be most of what the
	// hole shows. CodeParade's `extra_clip`.
	//
	// @param nearestSeam What `NearestSeamDistance` said, or infinity.
	// @return The pull-back, in studs.
	// @since v0.15
	float PortalClipBias(float nearestSeam);

	// Whether a point has gone through the hole.
	//
	// **A point rather than a body, which is what makes it a different question
	// from `SeamStraddled`.** A body has a size, straddles a plane and is cut by
	// it; a point is on one side or the other and belongs wholly to whichever
	// space that is. A particle is the case this exists for — a torch's flame
	// carried into a doorway has some of its sparks on this side of the plane
	// and some past it, and the ones past it are in the far room and are drawn
	// there.
	//
	// **Behind the pane's face *and* inside its rectangle**, with no widening.
	// The widening `SeamStraddled` applies is slack for a body's reach, and a
	// point has none — a spark a stud to the side of a doorway has not gone
	// through the doorway, it is beside it.
	//
	// @param seam The portal.
	// @param at   The point.
	// @return Whether it is on the far side of the hole.
	// @since v0.15
	bool SeamCarries(const PortalSeam &seam, const core::Vector3 &at);

	// Whether a box centred on `at` reaches across the seam inside its rectangle.
	//
	// **The straddle test, which is not the crossing test.** `CrossPortals` asks
	// whether a *segment* changed sides between two ticks; this asks whether a
	// body is in the pane *right now*, which is the state a clone exists for. A
	// body can be in the seam for a hundred ticks without ever crossing.
	//
	// @param seam  The portal.
	// @param at    The body's centre.
	// @param reach How far the body extends from `at`. A radius, so it is
	//              conservative for a box — a clone drawn a little early is
	//              invisible and one drawn a little late is a body cut in half.
	// @return Whether the body occupies the pane.
	// @since v0.15
	bool SeamStraddled(const PortalSeam &seam, const core::Vector3 &at, float reach);

	// Where a body standing in a seam is cut, and whether it may be cut at all.
	//
	// **One body cut at the plane, rather than two bodies drawn whole.** A
	// straddler is copied to the far side of its hole so the far room has
	// something to show; without a cut both copies are complete, and the extra
	// halves are drawn where the hole is not. With a pane set into a thick wall
	// the wall hides them both — which is why this went unnoticed through three
	// scenes — and with a free-standing pane it is visibly two crates in a
	// doorway, which is what `examples/PortalShadow.luau` shows.
	//
	// The two half-spaces are complementary through the map, so their union is
	// exactly the body and their intersection is empty. What fills the half each
	// copy has lost is the picture in the hole: the near half's missing part is
	// the far copy, seen through the pane, and the sub-camera's oblique clip
	// takes the far copy's.
	//
	// @since v0.15
	struct SeamCut {
		// The plane the *original* keeps, which is the front of the pane it is
		// standing in: `dot(p, NearNormal) >= NearOffset`.
		core::Vector3 NearNormal;
		float NearOffset = 0.0f;

		// The plane the *copy* keeps, which is the front of the far pane.
		core::Vector3 FarNormal;
		float FarOffset = 0.0f;

		// Whether the body fits through the hole's own footprint.
		//
		// **A body wider than the hole must not be cut, and that is the one
		// rule here that is not arithmetic.** The cut is a single plane, so it
		// is exact only for the part of the body that is inside the pane's
		// rectangle; anything hanging past the rim would be sliced by a plane
		// that continues where the hole does not, and what that looks like is a
		// crate with a flat face for no visible reason. A body that does not fit
		// is drawn whole on both sides, exactly as it was before this existed —
		// its overhang is the old artefact and is not made worse, and a hole it
		// cannot fit through is not a hole it is walking into.
		//
		// Measured with the body's **oriented box** against the pane's own
		// half-axes rather than with the bounding sphere `SeamStraddled` uses. A
		// sphere is what refused every character the last time size was made a
		// rule here: a five-stud figure has a radius of three and a doorway a
		// half-axis of two, so nothing would ever fit. A limb is a flat box and
		// its reach *across* a doorway is a fraction of its reach along itself.
		bool Fits = false;
	};

	// Works out where a straddling body is cut by one seam.
	//
	// @param seam       The portal it is standing in.
	// @param through    `SeamMapping(seam)`, taken by the caller because it
	//                   already has it and building it twice is two answers to
	//                   one question.
	// @param frame      Where the body is, interpolated for the frame being
	//                   drawn rather than at the tick boundary.
	// @param halfExtent Its half-extent on its own axes.
	// @return The two planes, and whether they may be applied at all.
	// @since v0.15
	SeamCut CutOfSeam(
		const PortalSeam &seam,
		const SeamTransform &through,
		const core::CFrame &frame,
		const core::Vector3 &halfExtent
	);

	// Every linked portal in the world.
	//
	// @param store The world to walk.
	// @param seams Cleared, then filled.
	// @return How many there are.
	// @since v0.15
	size_t GatherPortalSeams(ecs::Store &store, std::vector<PortalSeam> &seams);

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
	// **And the body is resized when the two panes are not the same size**,
	// which is what makes a room bigger on the inside something the simulation
	// agrees with rather than something the pane draws. A crosser's `Bounds`,
	// its speed, and — for a character — its humanoid's height, radius, walk and
	// jump speeds, ground tolerance and every limb's box and rest offset are all
	// multiplied by `PortalSeam::Scale`. Go through the small end and you are
	// small; come back the other way and the reciprocal puts you back, exactly,
	// because the scale of a pair is the ratio of two measurements that have not
	// changed.
	//
	// **Gravity is not scaled, and that is a decision rather than an
	// oversight.** CodeParade's demo multiplies it by the crosser's accumulated
	// scale so that shrinking is imperceptible, which is right for a gag about a
	// tunnel and wrong here: there is one world, `scene::Gravity` is its
	// property, and a small thing in it should fall the way a small thing falls.
	// What that looks like is the correct thing — walk into the large end and
	// the room becomes vast and your jumps become small, which is the whole
	// point of having gone through.
	//
	// **So is the camera's yaw, when the body crossing is the one it follows**,
	// and that is the same bug a third time. A player's view direction is
	// `CameraController::Angles` rather than any transform this pass moves, so
	// a pair of panes that turns a corner used to leave the eye pointing the way
	// it came in while the body walked the other way — the view snapping to a
	// wall on the frame you cross, and W walking you sideways afterwards,
	// because `ReadMoveIntent` is relative to that same yaw. Only the yaw is
	// turned, and only for `CameraController::Subject`; a headless host has no
	// controller and does nothing.
	//
	// **Runs in `PostSimulation`, after the solver has moved the body**, so what
	// is tested is where the tick actually ended rather than where it was
	// heading. `physics::RegisterCharacterSystems` installs it.
	//
	// @param store The world.
	// @return How many bodies crossed. Zero in every scene with no portal in it.
	// @since v0.14
	size_t CrossPortals(ecs::Store &store);

	// Where a straight line ends up when a portal is in the way of it.
	//
	// **The segment test `CrossPortals` runs on a body, offered to anything
	// else that travels in a straight line.** The one caller today is
	// `PlaceCamera`, and the bug it exists to close is worth naming because it
	// makes a working portal look broken: a third-person camera sits several
	// metres behind its subject, so the frame a character walks through a hole
	// is the frame the *body* is on one side and the *eye* is still on the
	// other. What that looks like is the character teleporting away from the
	// camera and turning as it goes — a hole that spits people out sideways
	// rather than one you walk through. Putting the arm through the same map
	// carries the eye with it, and the view that results is the one a player
	// expects for a different reason as well: standing at a hole in third
	// person, you see yourself in it, from the far side.
	//
	// **One hop, and deliberately.** A camera arm that crossed two holes in one
	// segment would need the map composed in order and the rectangle tested in
	// the mapped frame; a hole close enough behind another to do that is a hole
	// the arm is already inside. The first crossing found wins.
	//
	// @param store   The world.
	// @param from    Where the line starts — for a camera, the head.
	// @param to      Where it would end without a portal in the way.
	// @param through The map from this side to the far side, written only when
	//                the answer is true. Apply it to *both* the far end and the
	//                direction, exactly as a body's placement and velocity are —
	//                and mind which of `SeamTransform`'s four applications each
	//                of those is, because a hole may change size as well as
	//                place.
	// @return Whether the segment went through a hole rather than past one.
	// @since v0.15
	bool PortalCrossing(
		ecs::Store &store, const core::Vector3 &from, const core::Vector3 &to, SeamTransform &through
	);

	// One segment's meeting with one pane, in full.
	//
	// **What a ray needs and a body does not.** A body is teleported whole and
	// wants only the map; a ray has a length it must not exceed, has to know how
	// much of it was spent reaching the glass, and has to know *which* pane so
	// it can decline to stop on it — see `physics::RaycastThroughPortals`.
	//
	// @since v0.15
	struct PortalHop {
		// The map from this side to the far side. `CrossPortals`' `through`.
		SeamTransform Through;

		// Where along `from`→`to` the plane was met, as a fraction.
		float Share = 1.0f;

		// The pane that was met.
		//
		// **Because a portal's own pane is the one thing a ray through it must
		// look past.** `OpenPortals` leaves the collider in place as a trigger —
		// contacts are still reported and `Raycast` still answers with it — so
		// the nearest thing in front of every hole is the hole.
		ecs::Entity Pane = ecs::NULL_ENTITY;

		// The pane at the far end, which the same rule applies to.
		//
		// **A ray comes out of a hole standing on the far pane's glass**, at
		// exactly zero distance from it, because the map takes the near pane's
		// plane onto the far one's. Continuing without ignoring it reports the
		// destination pane as the first thing beyond the hole, every time, which
		// reads as a portal you cannot see or shoot through at all.
		ecs::Entity Far = ecs::NULL_ENTITY;
	};

	// The same crossing, described rather than just mapped.
	//
	// **The nearest pane wins**, which the plainer form did not have to decide
	// because nothing could tell its answers apart.
	//
	// @param store The world holding the panes.
	// @param from  Where the segment starts, in world space.
	// @param to    Where it ends.
	// @param hop   Filled when the answer is true, untouched otherwise.
	// @since v0.15
	bool
	PortalCrossing(ecs::Store &store, const core::Vector3 &from, const core::Vector3 &to, PortalHop &hop);

	// Pushes a point out of any portal pane it is standing in the plane of.
	//
	// **A viewpoint may be on either side of a hole and never in it**, which is
	// the rule a body already follows and an eye did not. `CrossPortals` puts a
	// crosser down a stated distance clear of the plane it crossed — see the
	// landing clearance in the source — so nothing can come to rest in the seam.
	// A camera had no such rule: a third-person arm swung into a pane, or a
	// first-person eye walked into one, could land *within* the pane's own
	// thickness.
	//
	// What that looks like is worth naming because it does not read as a camera
	// bug. The surface camera's oblique clip has no half-space left to keep, the
	// fit's extents run away, and the pane fills the screen with a vertical
	// smear of stretched texels — which looks like a corrupt texture or a broken
	// projection rather than like an eye standing somewhere it should not.
	//
	// **Pushed to the nearer side rather than always outward.** Which side a
	// viewpoint belongs on is the same question `SeamMapping` asks of a crosser
	// and `AimSurfaceCameras` asks of a viewer: barely inside from the front, it
	// belongs in front; past the middle, it has effectively arrived and belongs
	// behind. Either answer is a place a camera can render from, and the band
	// between them is the only place it cannot.
	//
	// **One pane, for `CrossPortals`' reason.** A point inside two panes at once
	// is at the line where two holes meet, and pushing it out of both would be
	// two answers to one question.
	//
	// @param store The world.
	// @param at    The point, moved only when it is inside a pane.
	// @return Whether it was moved.
	// @since v0.15
	bool ClearOfPanes(ecs::Store &store, core::Vector3 &at);

	// Stops a portal's pane from solving contacts, so a body can be inside it.
	//
	// **A hole you cannot stand in is a picture of a hole.** The pane is an
	// ordinary `Part` and an ordinary part collides, so the solver stopped a
	// character dead on the surface of every portal in the engine. `CrossPortals`
	// then never fired: it tests whether the segment between where a body started
	// the tick and where it finished changes sign through the pane, and a body
	// the solver parked *on* the plane never changes sign. Traversal was
	// implemented, tested and unreachable — the wall in front of it was the pane
	// itself.
	//
	// What that looks like is the thing to recognise: walking into a portal
	// stops you at the picture, and the far room stays a painting on a wall. An
	// immersive portal has to let the body straddle the plane — half in each
	// space, which is the frame everybody screenshots.
	//
	// **`Collider::Trigger` rather than removing the collider**, which is
	// `CanCollide = false` and exactly what it is for: contacts are still
	// reported, so a script can know somebody is in the hole, and no impulse is
	// solved, so nothing pushes them out of it. Removing the collider outright
	// would also take the pane out of `Raycast`, and `physics::GroundCharacters`
	// casts through the world — a portal that stopped answering queries is a
	// portal you fall through the floor beside.
	//
	// **A rule and not an authoring note.** It would be one line in each scene
	// that builds a portal, and every scene would have to know it; a hole that
	// collides is never what anybody meant. Idempotent, so a settled world pays
	// one component read per portal per tick and writes nothing.
	//
	// @param store The world.
	// @return How many panes were opened. Zero once every portal in the world
	//         has been, which is every tick after the first.
	// @since v0.14
	size_t OpenPortals(ecs::Store &store);

	// Cuts everything standing in a portal at the plane, and appends its far
	// half on the other side.
	//
	// **Half a character is the artefact this removes, and two whole characters
	// is the artefact the first attempt at it left behind.** Appending a copy
	// fixed the first half and introduced the second: both copies were drawn
	// *whole*, so the original hung out of the back of the pane into the room it
	// was walking into and the copy hung out of the far pane back into the room
	// it came from. A pane set into a thick wall hides both overhangs, which is
	// why that survived three scenes; a free-standing pane is visibly two crates
	// in a doorway. `NON-EUCLIDEAN.md` Part V.1.
	//
	// So: one body, cut at the plane. Each half keeps the front of its own pane
	// — `CutOfSeam` is where the two planes come from and why they are
	// complementary by construction — and what fills the half each has lost is
	// the picture in the hole.
	//
	// **What may be cut is what fits through the hole.** There is no test here
	// for whether a thing can move, and both rules that preceded it were wrong:
	// against the pane's shorter half-axis every character was refused, because
	// a person is very nearly as big as the doorway they walk through; against
	// `Motion` and `CharacterLimb` an anchored crate resting in a seam showed
	// nothing on the far side, though it is as much a thing standing in the hole
	// as anything that walked there. `CutOfSeam::Fits` is the physical statement
	// instead — a body wider than the hole is not standing in the hole — and it
	// excludes the room the pane is cut into without having to know that is what
	// it is doing.
	//
	// **One pass rather than two, which is what this used to be.** An entity
	// walk and a draw-list walk both produced this copy, and calling both put
	// two of them on the far side, z-fighting; worse, the list walk read the
	// list the entity walk had just appended to and ghosted the ghosts. Only a
	// list walk can cut, because only it holds the row the original is in, so
	// the list walk is the one that survived — and it reads the *interpolated*
	// frame a client actually drew rather than a tick position, which matters
	// because a far half a frame behind its own body is a seam that opens and
	// closes as you walk.
	//
	// A pane is a hole and a
	// body may straddle it — `OpenPortals` exists to allow exactly that — but
	// the body is one set of parts in one place, so the far room draws nothing
	// and the near room draws all of it. Standing in the seam, you are whole on
	// the side you came from and absent on the side you are walking into, which
	// is the one thing a picture of a hole and a hole must not share.
	//
	// The map onto the far side is `SeamMapping`, the same product the camera
	// and a crossing body go through, and a `DrawInstance` is a frame and a box.
	//
	// **A draw instance rather than an entity**, for `AppendSurfaceFaceMarkers`'
	// reason and more of it: a clone lives for one frame, must never be
	// selected, saved, found by a script or simulated, and there is one per body
	// per pane per frame. `physics::GhostPortalBodies` is the collision half and
	// is deliberately a different mechanism — a picture and a contact have
	// nothing to share but the seam.
	//
	// **Drawn by every pass, which is right rather than tolerated.** A clone is
	// visible through the pane, and it is also visible to somebody standing in
	// the far room looking at the far pane — which is what a body sticking out
	// of a portal looks like, and is what the original does at the near pane.
	//
	// **A cross-world pane gets the cut and not the copy.** Its
	// `Destination` is a camera stand-in in *this* world, so a clone through one
	// would appear a metre behind the pane the body is walking into rather than
	// in the world it is walking to — that copy is `AppendPortalClones`' job,
	// from a host holding both worlds. The *cut* is this pass's either way: the
	// body poking out of the back of the glass is a row right here, and leaving
	// it whole draws the body twice over, whole in the room it is leaving and
	// whole again in the room it is entering, joined nowhere.
	//
	// @param store The world.
	// @param out   The draw list. Read and appended to, and the rows of
	//              straddling bodies already in it are cut in place.
	// @return How many far halves were appended, which is how many things are
	//         standing in a hole. Zero on nearly every frame.
	// @since v0.15
	size_t CutAndCloneSeams(ecs::Store &store, std::vector<DrawInstance> &out);

	// Appends the far half of everything standing in one named pane, cut to the
	// far side of it, whether or not that pane crosses worlds.
	//
	// **What a host calls once it has the far world's draw list in its hands.**
	// A cross-world pane's copy does not belong in the list its body is drawn
	// from — it belongs beside the *other* world's, which only something holding
	// the universe can assemble. `client::AttachForeignSurfaces` is that caller,
	// and it appends the copy straight after the far world's rows so the two are
	// one range.
	//
	// **A draw list in and a draw list out**, which is what keeps this and
	// `CutAndCloneSeams` answering the same question. It was an entity walk over
	// bodies carrying `Motion` or `CharacterLimb` — "what goes through a portal
	// is what can move" — and that rule was already retired on the same-world
	// side, where an anchored crate resting in a seam is as much a thing
	// standing in the hole as anything that walked there. Reading rows also
	// takes the interpolated frame the frame is actually drawn with, rather than
	// re-deriving it and landing a half-body a tick away from its other half.
	//
	// **And the copy is cut.** A cross-world pane used to be argued as a window
	// rather than a hole — nothing straddles a window — which drew a body in the
	// doorway whole on this side and whole again on the far one, joined nowhere.
	// The two halves now get the same complementary planes a same-world pair
	// gets. The *original* is cut by `CutAndCloneSeams`, in the list it lives
	// in, which does that for a crossing seam as well.
	//
	// @param store   The world the body is standing in.
	// @param surface Which surface slot the pane samples.
	// @param source  That world's rows, as they will be drawn.
	// @param out     The list to append to. Nothing already in it is touched.
	// @return How many far halves were appended.
	// @since v0.15
	size_t AppendPortalClones(
		ecs::Store &store,
		int8_t surface,
		std::span<const DrawInstance> source,
		std::vector<DrawInstance> &out
	);

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
