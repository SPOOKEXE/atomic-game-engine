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
// construction. `NON_EUCLID.md`'s final section is the whole argument and
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
// multiply. `docs/NON-EUCLIDEAN.md` is the investigation.
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
		// rather than one, and it is the difference `docs/NON-EUCLIDEAN.md`
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

	// Builds it for one side of one seam.
	//
	// @param seam The portal.
	// @param side Which side the thing being mapped is on: positive in front of
	//             `Normal`, negative behind it. Zero counts as behind.
	// @return The map to put a placement, a velocity or a direction through.
	// @since v0.15
	SeamTransform SeamMapping(const PortalSeam &seam, float side);

	// How far the seam's plane is from a point, signed along `Normal`.
	//
	// @param seam The portal.
	// @param at   The point.
	// @return Positive in front of the pane, negative behind it.
	// @since v0.15
	float SeamOffset(const PortalSeam &seam, const core::Vector3 &at);

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

	// Every linked portal in the world.
	//
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
	};

	// The same crossing, described rather than just mapped.
	//
	// **The nearest pane wins**, which the plainer form did not have to decide
	// because nothing could tell its answers apart.
	//
	// @param hop Filled when the answer is true, untouched otherwise.
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

	// Appends a copy of every body standing in a portal, on the far side of it.
	//
	// **Half a character is the artefact this removes.** A pane is a hole and a
	// body may straddle it — `OpenPortals` exists to allow exactly that — but
	// the body is one set of parts in one place, so the far room draws nothing
	// and the near room draws all of it. Standing in the seam, you are whole on
	// the side you came from and absent on the side you are walking into, which
	// is the one thing a picture of a hole and a hole must not share.
	//
	// A clone is the standard answer and it is cheap here: the map onto the far
	// side is `SeamMapping`, the same product the camera and a crossing body go
	// through, and a `DrawInstance` is a frame and a box.
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
	// **Cross-world panes are skipped.** `Portal::DestinationWorld` says why:
	// their `Destination` is a camera stand-in in *this* world, so a clone
	// through one would appear a metre behind the pane the body is walking into
	// rather than in the world it is walking to. `client::AttachForeignSurfaces`
	// is where that half is answered.
	//
	// @param store The world.
	// @param out   The draw list to append to. Nothing already in it is touched.
	// @return How many clones were appended.
	// @since v0.15
	size_t AppendPortalClones(ecs::Store &store, std::vector<DrawInstance> &out);

	// The same, through one named pane, whether or not it crosses worlds.
	//
	// **What a host calls once it has the far world's draw list in its hands.**
	// A cross-world pane is skipped by the overload above because its clone does
	// not belong in this world's list — it belongs beside the *other* world's,
	// which only something holding the universe can assemble.
	// `client::AttachForeignSurfaces` is that caller, and it appends the clone
	// straight after the far world's rows so the two are one range.
	//
	// @param store   The world the body is standing in.
	// @param surface Which surface slot the pane samples.
	// @param out     The list to append to. Nothing already in it is touched.
	// @return How many clones were appended.
	// @since v0.15
	size_t AppendPortalClones(ecs::Store &store, int8_t surface, std::vector<DrawInstance> &out);

	// Appends the far half of anything standing in a portal.
	//
	// **A body in a hole is in two places, and the renderer only knew about
	// one.** `CrossPortals` moves a body when its step changes side, so for the
	// ticks it takes to walk through, the body is a single object sitting across
	// the plane — near half in this room, far half nowhere. What a player sees
	// is themselves sliced off at the seam: the near half drawn, and the far
	// half missing from the picture in the pane, because the picture is of a
	// room the body is not in yet.
	//
	// This is the other copy. Every instance straddling a pane's rectangle is
	// appended again at `destination · half-turn · source⁻¹` — the same map the
	// camera and the body go through — so the two halves meet at the plane.
	// Neither copy needs clipping: the pane's own image covers the near half's
	// overhang, and the surface camera's oblique clip takes the ghost's.
	//
	// **A draw instance rather than an entity**, exactly as
	// `AppendSurfaceFaceMarkers` is one and for the same reason: nothing is
	// added to the world, so it does not serialise, cannot be selected, and no
	// script can find a second copy of a character it did not make.
	//
	// **Reads the list rather than the world**, so it works on a replica with no
	// simulation in it, and so a ghost is built from the *interpolated* frame a
	// client actually drew rather than from the tick position — a ghost half a
	// frame behind its own body is a seam that opens and closes as you walk.
	//
	// Same-world pairs only. A `Portal::DestinationWorld` ghost belongs in the
	// far world's draw list, which is a host's to append and not a store's.
	//
	// @param store The world.
	// @param out   The draw list to read and append to.
	// @return How many ghosts were appended. Zero in every scene where nobody is
	//         standing in a hole, which is nearly every frame.
	// @since v0.15
	size_t AppendPortalGhosts(ecs::Store &store, std::vector<DrawInstance> &out);

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
