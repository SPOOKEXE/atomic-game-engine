#pragma once

// How a viewport panel maps between the world, the texture, and the panel.
//
// **This is the piece `docs/retired/v07v08.md` §4.2 names as most likely to be got
// wrong, and it has two traps in it rather than one.**
//
// ## The texture is last frame's
//
// `mono.studio/AGENTS.md`: the viewport is a `render::SceneTarget` shown with
// `ImGui::Image`, and the image is the *previous* frame's texture, because imgui
// records its draw lists before the renderer runs. Anything drawn over that
// image - a grid, a selection outline, a gizmo - is composited this frame.
//
// So an overlay projected with *this* frame's camera sits over a world drawn
// from *last* frame's, and the two disagree by exactly one frame of camera
// motion. Standing still it looks perfect. Flying, the grid swims against the
// ground and the outline trails the part it is outlining, which reads as a
// renderer fault rather than as a projection using the wrong matrix.
//
// The fix is not a smaller error; it is the right matrix. A panel keeps the
// `ViewProjection` that produced the texture it is currently displaying, and the
// overlay uses that. The one-frame lag is then shared by the world and the
// things drawn on it, so they agree - which is the same trade `world::ViewChannel`
// and `SurfaceView` already make.
//
// ## The image rect is not the panel rect
//
// `render::Renderer` reports how much of a slot's texture the world was actually
// drawn into, and it is not necessarily all of it: targets are allocated in
// rounded blocks and the drawn region is the panel-sized sub-rect inside. Panel
// space therefore maps to **that sub-rect**, not to the panel.
//
// Getting this wrong is worse than getting the matrix wrong, because the error
// is small and constant. A grid a few pixels out looks like a grid. Picking a
// few pixels out selects the wrong part only near an edge, which is the version
// of a bug that gets reported as "sometimes it selects the wrong thing".
//
// ## Why this is not in `render`
//
// `mono.engine/render` has no debug-line facility at all, and adding one at L12
// for an editor's furniture is the wrong place for it. The overlay is drawn into
// the panel's imgui draw list; this header is the arithmetic that makes that
// possible, and it is pure - no imgui, no store, no device - which is why
// `tests/Projection.cpp` can exercise it at all.
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace studio {

	// Where a ray meets a plane.
	//
	// **What a rotate gizmo is made of.** Turning about an axis means dragging
	// around the circle perpendicular to it, and the angle is read from where
	// the cursor's ray crosses that circle's plane.
	//
	// **Refuses a ray parallel to the plane**, and refuses a hit behind the eye.
	// Both produce a point, and both produce the wrong one: a near-parallel ray
	// crosses a plane thousands of metres away, so the angle it reads swings
	// wildly for a pixel of mouse movement - which is a rotation gizmo that
	// spins the selection when you nudge it edge-on.
	//
	// @param origin Any point on the plane.
	// @param normal The plane's normal. Length one.
	// @param ray    The ray under the cursor.
	// @param point  Filled in with where they meet.
	// @return `false` when they do not meaningfully meet in front of the eye.
	bool IntersectRayPlane(
		engine::core::Vector3 origin,
		engine::core::Vector3 normal,
		const engine::core::Ray &ray,
		engine::core::Vector3 &point
	);

	// How far along an axis the point nearest a ray lies.
	//
	// **What a translate gizmo is made of.** Dragging an axis handle means
	// "where along this line is the mouse pointing", and the honest answer is
	// the point on the axis closest to the eye ray - not the axis point that
	// projects nearest the cursor in screen space, which drifts as the camera
	// turns and makes a drag creep after the mouse has stopped.
	//
	// Returns a distance along `axis` from `origin`, signed. The caller
	// subtracts the value it got when the drag began, which is what makes the
	// handle stay under the cursor rather than jumping to it.
	//
	// **Degenerate when the axis points at the eye**, and it says so rather than
	// dividing by nothing: an axis seen end-on has no screen direction to drag
	// along, and every mouse position maps to a wildly different distance. A
	// gizmo that kept dragging there would fling the selection.
	//
	// @param origin Where the axis starts.
	// @param axis   Its direction. Length one.
	// @param ray    The ray under the cursor.
	// @param along  Filled in with the signed distance along `axis`.
	// @return `false` when the axis and the ray are too near parallel to tell.
	bool ClosestPointOnAxis(
		engine::core::Vector3 origin, engine::core::Vector3 axis, const engine::core::Ray &ray, float &along
	);

	// How far an oriented box reaches from its centre along a direction.
	//
	// **The support function, which is what "resting on it" needs.** A box laid
	// on a slope touches it at one corner, and the distance from the centre to
	// that corner along the surface normal is exactly this sum - using half the
	// height instead would sink a tilted part into the ground by however much it
	// is tilted.
	//
	// @param frame     Where the box is and how it is turned.
	// @param half      Its half-extents, in its own axes.
	// @param direction Which way to reach. Length one.
	// @return The distance from the centre to the surface along `direction`.
	// @since v0.13
	float SupportAlong(
		const engine::core::CFrame &frame,
		const engine::core::Vector3 &half,
		const engine::core::Vector3 &direction
	);

	// A rotation with its up along a surface normal, turned as little as it can
	// be.
	//
	// **The old facing is projected onto the new plane rather than discarded.**
	// A part dropped onto a wall has to keep pointing the way its author left it
	// pointing; rebuilding the basis from the normal alone would spin it to
	// whatever the arbitrary second axis happened to be, and every part dropped
	// on that wall would end up facing the same way whatever anybody had done.
	//
	// **`LookVector` is `-Z`**, so the basis's third column is the *back*.
	// Getting that the other way round mirrors every part that is dropped, which
	// reads as the model being wrong rather than the maths - which is most of
	// why this is a named function with a suite rather than eight lines inside a
	// drag.
	//
	// @param was    The rotation to keep as much of as possible.
	// @param normal The surface's outward normal. Need not be unit length.
	// @return The aligned rotation, or `was.Rotation()` when the old facing says
	//         nothing about the new plane.
	// @since v0.13
	glm::quat AlignedTo(const engine::core::CFrame &was, const engine::core::Vector3 &normal);

	// One viewport panel's mapping, for one frame.
	//
	// Built from what the renderer reported for the texture now on screen - see
	// the header note. Every field is in the space its name says: `Matrix` takes
	// world to clip, and the two rect fields are in the panel's own coordinates
	// with the origin at the panel's content top-left.
	//
	// @since v0.7
	struct PanelProjection {
		// `Projection * View`, in the form `scene::CameraMatrices::ViewProjection`
		// gives it.
		//
		// **Rebuilt by a second `scene::ResolveCamera`, not handed over by the
		// renderer.** `Editor::ProjectionFor` resolves the same camera frame,
		// the same lens and the same aspect ratio that `PresentWorld` is about
		// to resolve, so the two agree by construction rather than by being one
		// object. Sharing the renderer's copy would mean the overlay pass
		// reading a matrix produced after it - the frame order the header note
		// describes runs the wrong way for that.
		//
		// The usual objection to computing a thing twice is that the two copies
		// drift. Here they cannot, because the only inputs that could diverge
		// are ones the answer does not use: under a perspective divide `w` is
		// `-z_view`, so where a point lands in x and y comes from the frame, the
		// field of view and the aspect ratio and from neither clipping plane. A
		// near or far plane changed in one place and not the other moves nothing
		// this struct projects - which is why `ProjectionFor` deliberately omits
		// `PresentWorld`'s far-plane stretch rather than copying it. `Near`
		// below is carried explicitly for the one thing that does need a plane,
		// the clip in `ProjectSegment`.
		glm::mat4 Matrix{1.0f};

		// Where the camera was when that matrix was made. Carried rather than
		// extracted from the inverse: it is already to hand at every call site,
		// and inverting a matrix to recover something the caller knows is both
		// slower and less exact.
		engine::core::Vector3 Eye;

		// The panel-space rectangle the world was drawn into. See the header:
		// this is the renderer's reported sub-rect, not the panel.
		glm::vec2 ImageMin{0.0f, 0.0f};

		// How big that rectangle is, in the same space as `ImageMin`.
		glm::vec2 ImageSize{1.0f, 1.0f};

		// The camera's near clipping distance, matching `scene::Camera`.
		//
		// **Used as the clip plane for `ProjectSegment`, and an epsilon will not
		// do.** For a standard perspective matrix clip-space `w` *is* the
		// distance in front of the camera, so clipping at a tiny `w` puts the
		// cut point at the eye itself - where the perspective divide sends it to
		// coordinates in the millions, and imgui rasterises that as a stripe
		// across the whole viewport. Clipping at the near plane puts it where
		// the geometry genuinely stops being visible, which is finite and
		// inside the frustum.
		float Near = 0.1f;

		// Reports whether this describes a rectangle anything could be drawn in.
		//
		// @return `true` when both dimensions are positive.
		bool IsValid() const {
			return ImageSize.x > 0.0f && ImageSize.y > 0.0f;
		}

		// Projects a world point into panel coordinates.
		//
		// **Returns `false` for anything at or behind the eye plane**, rather
		// than a point. A clip-space `w` of zero or less is a point the
		// perspective divide turns into a mirrored ghost in front of the camera
		// - a grid line behind you drawn across the sky - and there is no
		// sensible panel coordinate to hand back instead. Callers clip the
		// segment, or skip it.
		//
		// @param world The point.
		// @param panel Filled in on success, in panel coordinates.
		// @return `false` when the point is not in front of the camera.
		bool WorldToPanel(engine::core::Vector3 world, glm::vec2 &panel) const;

		// Projects a world-space line segment, clipping it against the eye
		// plane.
		//
		// **What a grid line actually needs, and why `WorldToPanel` twice is not
		// enough.** A line crossing behind the camera has one endpoint that
		// cannot be projected at all; dropping the whole segment leaves holes in
		// the grid exactly where it passes the viewer, which is most of the
		// screen when you are standing on it. Clipping keeps the visible part.
		//
		// The clip is exact rather than iterative: clip-space `w` is a dot
		// product with a row of the matrix and therefore linear in world
		// position, so the crossing point is one interpolation.
		//
		// @param from The segment's start.
		// @param to   The segment's end.
		// @param outFrom Filled in with the panel-space start.
		// @param outTo   Filled in with the panel-space end.
		// @return `false` when the whole segment is behind the eye plane.
		bool ProjectSegment(
			engine::core::Vector3 from, engine::core::Vector3 to, glm::vec2 &outFrom, glm::vec2 &outTo
		) const;

		// Builds the world-space ray under a panel point.
		//
		// The ray a click means: it starts at the eye and passes through the
		// point on the near plane the pointer is over. What `spatial::Raycast`
		// takes, and therefore what viewport picking is made of.
		//
		// A point outside the image rect still yields a ray - the arithmetic is
		// the same and refusing would make the caller ask twice. Ask
		// `ContainsPanel` when the distinction matters.
		//
		// @param panel The panel-space point.
		// @return The ray, with a unit direction.
		engine::core::Ray PanelToRay(glm::vec2 panel) const;

		// Whether a panel point is over the drawn world rather than beside it.
		//
		// @param panel The panel-space point.
		// @return `true` when it is inside the image rect.
		bool ContainsPanel(glm::vec2 panel) const;
	};
}
