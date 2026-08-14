#pragma once

// Which camera a world is looked at from, and the matrices that follow from it.
//
// `Camera` is a component because a world may hold several of them - a
// spectator, a cutscene, a security monitor - and under the instance model a
// camera is an instance, which is a row. This is the other half: a resource
// naming the live one, so "where is the camera" is a lookup rather than a walk
// over every camera row asking which is in charge.
//
// **This holds no `render::Camera` and must never hold one.** That type is
// `client` tier: a server-tier host resolves a view for a hosted world and
// cannot name it, and reaching for it would put presentation in the type a
// headless world writes. What crosses is a `CFrame`, three floats and the
// matrices below - all of which are arithmetic, not device state.
//
// The matrices are cached rather than derived at every read because the
// consumers are a culling pass, a draw-list build and an overlay, and three
// passes recomputing one inverse per frame is three chances for two of them to
// disagree about which tick's camera they were looking at.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// A camera's view and projection, already resolved.
	//
	// Column-major and Vulkan-convention: Y-up clip space and depth mapped to
	// `0..1`, which is what `GLM_FORCE_DEPTH_ZERO_TO_ONE` in `core`'s build
	// pins for the whole engine. A projection built under a different setting
	// is a depth buffer that is subtly inverted, and it reads as a z-fighting
	// bug rather than as a matrix mistake.
	//
	// @since v0.4
	struct CameraMatrices {
		// World space to view space: the inverse of the camera's frame.
		glm::mat4 View{1.0f};

		// View space to clip space.
		glm::mat4 Projection{1.0f};

		// `Projection * View`, kept because every consumer wants the product
		// and a second multiply per pass is a second chance to get the order
		// backwards.
		glm::mat4 ViewProjection{1.0f};
	};

	// The world's live camera.
	//
	// A resource: there is one of it, and nothing iterates it.
	//
	// @since v0.4
	struct ActiveCamera {
		// The entity carrying the live `Camera` and `Transform`.
		//
		// A handle rather than a copy of the values, so there is exactly one
		// place a camera's field of view is written and no way for the live
		// camera to disagree with the row it came from.
		ecs::Entity Entity;

		// Width over height of whatever this world is being drawn into.
		//
		// Written by the consumer - a window, an offscreen target, a mirror's
		// texture - because a world has no idea how big anybody's screen is.
		// Kept here rather than passed to `ResolveActiveCamera` so that
		// resolving stays a plain `void(Store &)` the scheduler can register.
		float AspectRatio = 1.0f;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes.
		uint32_t Reserved = 0;

		// What `ResolveActiveCamera` last computed. Identity until it runs.
		//
		// Braced rather than left bare so that "identity until it runs" is what
		// the type says and not only what this comment says. An aggregate
		// initialiser naming the entity and the aspect ratio - which is every
		// caller - leaves this member behind, and a member with no default is
		// one `-Wmissing-field-initializers` is right to call out.
		CameraMatrices Matrices{};
	};

	// Builds the matrices for one camera at one placement.
	//
	// The one place the engine decides what a camera's matrices are. Every
	// consumer that built its own would be a consumer that can disagree about
	// handedness, clip depth or the order of the product.
	//
	// A zero or negative `aspectRatio` yields identity matrices rather than a
	// projection full of infinities: a window reports zero height while it is
	// minimised, and that is an ordinary frame rather than an error.
	//
	// @param frame       The camera's world-space placement.
	// @param camera      Field of view and clipping distances.
	// @param aspectRatio Width over height of the target.
	// @return The resolved view, projection and their product.
	CameraMatrices ResolveCamera(const core::CFrame &frame, const Camera &camera, float aspectRatio);

	// Any projection, with its near plane skewed onto a world-space plane.
	//
	// **Lengyel's oblique frustum, applied to a projection somebody else
	// built.** Everything in front of the plane survives clipping and everything
	// behind it does not, at any angle - which is what a mirror wants for its own
	// pane, so the frame and the back of the glass cannot occlude the reflection,
	// and what a portal cannot work without: its destination is set into a wall,
	// and the wall would otherwise draw across the hole it leads through.
	//
	// **Separate from `SurfaceProjection` because the two callers start from
	// different frustums.** A surface camera skews a frustum fitted to its pane;
	// the recursive portal pass skews the **screen's own** projection, unchanged,
	// which is what makes a pane's clip-space position in the parent view the
	// same coordinate as its position in the sub-render. One skew, stated once.
	//
	// **Written for `0..1` depth, and the difference is not cosmetic.**
	// Lengyel's published derivation maps the near plane to `-1`, so it
	// substitutes `C·2/(C·Q)` and subtracts the `w` row. `GLM_FORCE_DEPTH_ZERO_TO_ONE`
	// is pinned engine-wide in `core`'s build, where near is `0` - so the
	// substitution is `C/(C·Q)` and nothing is subtracted. Using the other form
	// here would put the near plane half a unit into the scene and read as
	// z-fighting rather than as a matrix mistake, which is the trap
	// `CameraMatrices` above already warns about.
	//
	// The camera must be on the near side of the plane: the normal points away
	// from it, into the space being looked at. A camera level with the plane gets
	// the projection back unchanged, because there is no half to keep.
	//
	// @param projection The frustum to skew.
	// @param frame      The camera's world-space placement, which is what takes
	//                   the plane into view space.
	// @param normal     The plane's normal in world space. A zero-length one
	//                   means no plane and no skew.
	// @param distance   `normal · point` for any point on the plane.
	// @return The skewed projection, or `projection` when there is nothing to
	//         skew against.
	// @since v0.15
	glm::mat4 ObliqueProjection(
		const glm::mat4 &projection, const core::CFrame &frame, const core::Vector3 &normal, float distance
	);

	// The projection a surface camera renders through.
	//
	// **Off-axis, so it is a window rather than a cone.** The four extents are
	// independent, so a frustum fitted to a pane from off to one side leans
	// instead of widening symmetrically about the view axis - which is the same
	// coverage at twice the texel density, and what `SurfaceCameras.hpp` named
	// as the change it was waiting for.
	//
	// **Then skewed by `ObliqueProjection`, if the lens carries a clip plane.**
	//
	// @param lens  The fitted extents and the plane, in world space.
	// @param frame The camera's world-space placement, which is what takes the
	//        plane into view space.
	// @return The projection alone. Compose it with `ResolveSurfaceCamera`.
	// @since v0.14
	glm::mat4 SurfaceProjection(const SurfaceLens &lens, const core::CFrame &frame);

	// What moved the pane into the space that projection was fitted to.
	//
	// **`SurfaceLens` holds the map in three pieces and a shader wants one
	// matrix**, and composing them is arithmetic with an order that can be got
	// wrong: the scale is taken about `MappingOrigin` and *then* the rigid part
	// is applied, so the product is `T(pos) · R · T(origin) · S · T(-origin)`.
	// Written once here rather than at each consumer, for the reason
	// `SurfaceProjection` is: a second composition is a second chance to swap
	// two of the four.
	//
	// The identity for a mirror, and for any pair of panes the same size, so a
	// caller that has never heard of a scaled portal gets what it always got.
	//
	// @param lens The lens `AimSurfaceCameras` fitted.
	// @return The matrix to pre-multiply a pane's world position by, which is
	//         what `render::SurfaceView::Mapping` carries.
	// @since v0.15
	glm::mat4 SurfaceMapping(const SurfaceLens &lens);

	// A seam's map as a matrix.
	//
	// **The same product `SeamTransform::Point` applies, written once as four
	// multiplies rather than per point.** A shader cannot call a method, and the
	// beam shadow a hole transports needs the map on the fragment side - see
	// `NON-EUCLIDEAN.md` Part V.3, where a far-side fragment is carried back into
	// the near room before it is looked up.
	//
	// `T(position) · R · T(origin) · S · T(-origin)`, and the order is the part
	// that can be got wrong: the scale is about the *source* pane's centre, which
	// is the point the rigid half already sends to the destination's centre.
	//
	// @param through The map.
	// @return It, as a matrix a shader can multiply by.
	// @since v0.15
	glm::mat4 SeamMatrix(const SeamTransform &through);

	// Builds the matrices for a camera whose projection is already decided.
	//
	// **The surface path's `ResolveCamera`.** A surface camera's frustum is not
	// a field of view - it is fitted to a pane and possibly skewed - so there is
	// nothing for `ResolveCamera` to derive it from. This exists so the one
	// convention that still has to be shared, `View = inverse(frame)` and
	// `ViewProjection = Projection * View`, stays in this file rather than being
	// repeated wherever a projection is handed in.
	//
	// @param frame      The camera's world-space placement.
	// @param projection The projection to use as given.
	// @return The resolved view, projection and their product.
	// @since v0.14
	CameraMatrices ResolveSurfaceCamera(const core::CFrame &frame, const glm::mat4 &projection);

	// Refreshes the world's `ActiveCamera` matrices from the row it names.
	//
	// A `void(Store &)` and nothing else, so it registers as an ordinary
	// system - which is the reason `AspectRatio` is a field above rather than a
	// second argument here.
	//
	// Leaves the matrices exactly as they were when there is no `ActiveCamera`
	// resource, when the entity it names is dead, or when that entity carries
	// no `Camera` or no `Transform`. A stale view is a frame drawn from where
	// the camera was; clearing to identity would be a frame drawn from inside
	// the origin with nothing in it, which looks like a renderer fault and
	// sends the search to the wrong module.
	//
	// @param store The world to resolve in.
	void ResolveActiveCamera(ecs::Store &store);
}
