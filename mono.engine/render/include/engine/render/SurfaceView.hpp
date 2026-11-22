#pragma once

// Surface and portal views rendered into textures, and the recursion depth caps.

#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>
#include <engine/scene/environment/Sunlight.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <vector>

namespace engine::render {
	struct SceneLight;

	// Planner outcome recorded with a portal view for capture diagnostics.
	enum class PortalDemandStatus { Ready, Hidden, Invalid, Unsupported };

	// Distinguishes the two geometric refusals folded into PortalDemandStatus::Hidden.
	enum class PortalDemandHiddenReason { None, Frustum, ClipPlane };

	// A second view, rendered into a texture instead of the swapchain.
	//
	// Surface textures are double-buffered, so what a pane shows the screen is
	// the frame before. **What a pane shows *another pane* is not**, and stopped
	// being so in v0.15: the levels below the first are rendered inside the frame
	// by a recursion, from cameras derived level by level. See `PaneNormal` for
	// what that needs and why iterating could not supply it.
	//
	// **Mirrors and cross-world windows, and since v0.15 nothing else.** A
	// same-world portal used to be one of these and is a `PortalView` now, for
	// the reason that type states in full: the two derive a sub-camera by
	// different rules, and only one of them is a reflection.
	//
	// @since v0.6
	struct SurfaceView {
		// Which surface index this renders, matching `scene::Visual::Surface` on
		// whatever samples it.
		//
		// **The pairing is by number and nothing else**, which is what lets a
		// replica reflect at all: the wire carries components and not the tree,
		// so "the camera belonging to this pane" cannot be a parent link on the
		// far end. `scene::AimSurfaceCameras` is what makes the two numbers
		// agree, by writing the camera's index onto the pane it is parented to.
		//
		// At or above `scene::MAX_SURFACES` the view is dropped with a line in
		// the log, rather than silently rendering nothing.
		int16_t Index = 0;

		// Where the surface camera is, in world space.
		core::CFrame Frame;

		// The pane this camera projects off, when it is a mirror parented to one.
		//
		// **What lets the surface pass descend, and the whole of what it was
		// missing.** `Frame` and `Projection` are a camera *already placed from
		// the eye*, so a pane appearing inside another pane's picture was drawn
		// and sampled from a viewpoint nobody was looking from - the coordinate
		// leaves the texture's rectangle and `opaque.frag` falls back to the flat
		// lit pane, which is what "a mirror in a mirror shows a blank slab" was.
		//
		// A mirror's camera is a function of the pane and the viewer -
		// `scene::ReflectCamera` - so a pass holding the *rectangle* can place
		// that camera for a viewer that is not the eye, which is exactly what the
		// level below a mirror needs. `PortalView` carries the same four vectors
		// for the same reason, and neither type reads them off a component,
		// because measuring a face is `scene::GatherSurfacePanes`' job.
		//
		// **A zero `PaneNormal` means "not a pane this pass may descend into"**,
		// and it is the ordinary case rather than an error: a camera parented to
		// the world has no face to reflect through, and one showing another
		// world's instances has no local geometry behind it. Both keep the single
		// eye-derived image they have always had.
		//
		// `Centre ± First ± Second` is the four corners, as everywhere else.
		//
		// @since v0.15
		//@{
		core::Vector3 PaneCentre;
		core::Vector3 PaneNormal;
		core::Vector3 PaneFirst;
		core::Vector3 PaneSecond;
		//@}

		// The lens the pane's author gave the camera, which is what a camera
		// placed for a deeper viewer has to be fitted at.
		//
		// **Carried rather than read back out of `Projection`.** The near plane is
		// recoverable from a projection matrix and the far plane is recoverable
		// badly, and both are already known where the pane was measured - so a
		// recursion that recovered them would be deriving, once per level, two
		// numbers that were handed over for free.
		//
		// @since v0.15
		//@{
		float PaneNear = 0.1f;
		float PaneFar = 500.0f;
		//@}

		// What it renders through, already built.
		//
		// **A projection handed in rather than a field of view derived here**,
		// and the change is what a portal rests on. A surface frustum is fitted
		// to a pane's four corners and is therefore **off-axis** - the four
		// edges lean independently, so a viewer standing to one side gets
		// exactly the pane instead of twice its width; and for a portal it is
		// also **obliquely clipped**, with the near plane skewed onto the
		// destination's plane so the wall the hole leads through cannot draw
		// across it. Neither is expressible as an angle and two distances,
		// which is what this field used to be.
		//
		// Built by `scene::SurfaceProjection` from the `scene::SurfaceLens` that
		// `scene::AimSurfaceCameras` fits, so the renderer neither measures a
		// pane nor knows what a portal is.
		//
		// @since v0.14
		glm::mat4 Projection{1.0f};

		// What moved the pane into the space this view was fitted to.
		//
		// **A pane samples its image by projecting its own world position, so a
		// camera that was fitted somewhere else needs the pane taken there
		// too.** The surface is rendered with the view's own matrix and read
		// back with that matrix times this one; `scene::SurfaceLens::Mapping` is
		// where the two are argued out.
		//
		// Identity is a mirror, and is what a host that has never heard of a
		// portal leaves it as - including one filling this struct by hand.
		//
		// @since v0.14
		glm::mat4 Mapping{1.0f};

		// How wide the texture is. Square is not required; a wide mirror wants a
		// wide target, and giving it a square one wastes half the texels.
		uint32_t Width = 1024;

		// How tall the texture is.
		uint32_t Height = 1024;

		// How opaque the projected image is, 0 transparent to 1 solid.
		//
		// **The image's own opacity and not the part's**, which is the whole
		// distinction `scene::SurfaceCamera::ImageTransparency` exists to make:
		// how much of the world shows through the glass and how much of the glass
		// shows through the reflection are two facts, and writing the image with
		// the part's alpha meant fading a mirror faded its reflection to nothing.
		//
		// Carried as opacity rather than as transparency because that is what the
		// shader multiplies by; the flip happens once, where the component is
		// read, rather than in a shader nobody can put a breakpoint in.
		float ImageOpacity = 1.0f;

		// What the image is put through before a pane shows it.
		//
		// **A grade at the sampling site rather than a second pass.** The
		// surface texture holds an ordinary picture of the world whatever this
		// says; `opaque.frag` applies the effect where the pane reads it. So an
		// effect costs no render target, no extra pipeline and no bind, and the
		// surfaces that ask for none pay for none.
		//
		// From `scene::SurfaceCamera::Effect`.
		//
		// @since v0.13
		scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

		// Which tags an instance must carry to be drawn into this surface, or
		// zero for all of them.
		//
		// From `scene::SurfaceCamera::TagFilter`. **Applied per instance in the
		// draw loop rather than by re-ordering the draw list**, because the
		// order is shared by every view and a filter is per view: partitioning
		// it for one surface would be partitioning it for all of them, and the
		// screen pass would then draw the group instead of the world.
		uint32_t TagFilter = 0;

		// How many times a second this surface may redraw, or zero for every
		// frame.
		//
		// From `scene::SurfaceCamera::FPS`, and honoured beside the content
		// signature rather than instead of it: a surface redraws when its image
		// has changed **and** it is visible **and** its interval has elapsed.
		// The three are independent reasons to skip, and a surface that fails
		// any of them keeps the texture it has along with the matrices that
		// drew it.
		//
		// **Needs `Renderer::SetAnimationTime`**, which is the frame clock this
		// class already has. A host that never sets one leaves it at zero, and
		// a surface capped against a clock that never advances would render once
		// and freeze - so a non-advancing clock is treated as uncapped.
		//
		// @since v0.15
		float FPS = 0.0f;

		// Which instances this surface draws, when they are not this world's.
		//
		// **What makes a portal able to show another world**, and it is a range
		// rather than a second draw list because the renderer uploads one
		// instance buffer per frame. A host that wants world B seen through
		// world A's pane puts B's instances in `Render`'s `foreign` argument and
		// names that range here; the surface pass then draws exactly it.
		//
		// **An offset into `foreign`, not into the world's own list**, and the
		// two are separate arguments for the reason that split exists: joined,
		// the far world would be culled, ordered and drawn as part of this one.
		// The renderer moves the range into buffer space itself, after the
		// ordering that would otherwise have invalidated it.
		//
		// **`Count == 0` means "this world", which is every mirror and every
		// same-world portal.** Those draw the `scene::ScenePlan`'s own runs -
		// the world minus the pane being rendered, plus every other pane - which
		// is the arrangement that makes a mirror of a mirror work and is not
		// something a foreign range can participate in: the plan describes this
		// world's partitioning and knows nothing about the appended tail.
		//
		// So a foreign surface is drawn **plainly and in one run**. A pane in
		// the far world shows as a flat pane rather than as a recursive image,
		// which is the same fallback a surface with no frame yet already gets
		// and is right for the same reason - a hole through a hole into a third
		// world is a feature nobody has asked for and would need a plan per
		// world per frame to serve.
		//
		// @since v0.14
		//@{
		uint32_t InstanceFirst = 0;
		uint32_t InstanceCount = 0;
		//@}

		// Cross-world captures use the destination world's lighting and local
		// lights. Mirrors and same-world captures leave this disabled and inherit
		// the view's current lighting.
		//@{
		scene::WorldLighting Lighting;
		std::vector<SceneLight> Lights;
		bool OverrideLighting = false;
		//@}
	};

	// One same-world portal, as the recursive pass needs it.
	//
	// **Beside `surfaces` and never one of them, because a hole and a mirror are
	// not the same map.** Both passes are recursions now and both derive each
	// level's camera from the level above - that much stopped being a difference
	// in v0.15, and the machinery for opening a pass and drawing the world into
	// it is shared between them. What is left is the rule itself, and it is not
	// close to the same: a mirror reflects the viewer through one plane, and a
	// hole carries it through `destination · half-turn · source⁻¹` onto a second
	// pane that may be turned, moved and resized anywhere at all.
	//
	// **And what the pane reads back differs with it.** A hole's sub-render is
	// the screen's own frustum skewed onto the far plane, so its pane samples by
	// screen position and lines up texel for texel. A mirror's is fitted to its
	// own rectangle, so its pane samples by projecting its world position through
	// the matrix that drew the level. Merging the two entries would mean one of
	// them carrying the other's fields empty.
	//
	// `docs/retired/NON-EUCLIDEAN.md`'s Part III is the whole argument for the hole, and
	// CodeParade's `Portal.cpp` is the model.
	//
	// Local portals derive recursive views. External portals sample an owned
	// image reply using its fitted capture mapping and never render local geometry
	// as a substitute for an unavailable destination.
	//
	// @since v0.15
	struct PortalView {
		// Which surface slot the pane's draw instances carry, from
		// `scene::DrawInstance::Surface`.
		//
		// **How the pass finds the pane's geometry, and it draws no geometry of
		// its own.** The quad is the pane part already in the draw list, which
		// `scene::PartitionSurfaces` has already grouped into a run of its own -
		// so there is no second mesh, no vertex buffer, and nothing coplanar with
		// the pane to fight it for depth.
		int16_t Index = 0;

		// ImportedImage is a renderer-local generation handle. ImagePortal names
		// the authored entrance. ExternalImage stays true while an image is absent.
		bool ExternalImage = false;
		// Planner outcome for this aperture. It is diagnostic metadata and does
		// not change the renderer's external image mode.
		PortalDemandStatus ImageDemandStatus = PortalDemandStatus::Invalid;
		// Geometric reason a hidden image demand did not produce a capture.
		PortalDemandHiddenReason ImageHiddenReason = PortalDemandHiddenReason::None;
		// Whether the aperture intersects the demand culling frustum.
		bool ImageFrustumVisible = false;
		// Culling frame used for ImageDemandStatus. It can differ from the raster
		// camera while a presentation route retains its source eye.
		core::CFrame ImageDemandCamera;
		// Revision of the camera used to classify this demand.
		uint64_t ImageCameraRevision = 0;
		// Generation handle of the accepted imported image, or zero.
		uint64_t ImportedImage = 0;
		// Authored portal key used to resolve that imported image.
		core::Name ImagePortal;
		// Imported HDR radiance sampled only by deferred seam spill, or zero.
		uint64_t ImportedLightImage = 0;
		// Capture key and source-world side paired with ImportedLightImage.
		core::Name LightImagePortal;
		core::Vector3 LightOutward; // Source portal's outward light direction.

		// The slot of the hole at the far end of this one, or -1 for none.
		//
		// **Skipped by the level this one opens**, which is CodeParade's
		// `skipPortal` argument and is the same saving. A sub-camera stands at
		// the far pane looking away from it, so the far pane is at the level's
		// own clip plane: rendering through it produces a scene that is then
		// entirely clipped away, at the cost of a full sub-render. Cheaper to
		// name it than to draw it and throw it away.
		int16_t Partner = -1;

		// The pane's plane and rectangle in world space, exactly as
		// `scene::PortalSeam` states them: `Centre ± First ± Second` is the four
		// corners and `Normal` is the face's own normal.
		//
		// **The rectangle and not the pane's box**, because what has to be culled
		// per level is the hole rather than the slab it is cut into, and because
		// the sub-camera is derived by mapping *this* rectangle rather than
		// anything the draw list holds.
		//@{
		core::Vector3 Centre;
		core::Vector3 Normal;
		core::Vector3 First;
		core::Vector3 Second;
		//@}

		// The map to the far side.
		//
		// **One map and not one per side, which is the point.** It carries this
		// pane's front hemisphere to the far pane's back one and its back to the
		// far pane's front, so it is right at the top level and right at every
		// level below where the sub-camera has stepped through - and it is the
		// exact map `scene::CrossPortals` moves a body by, so what the hole shows
		// and where walking into it puts you cannot come apart. CodeParade's
		// `Portal::Warp::delta`, which is likewise one matrix for both sides.
		scene::SeamTransform Warp;

		// Which tags an instance must carry to be drawn through this hole, or
		// zero for all of them. `SurfaceView::TagFilter`'s argument, unchanged.
		uint32_t TagFilter = 0;
	};

	// What a portal can show this frame. The degraded states are named so a
	// frame missing destination content says why, instead of passing as
	// continuity or failing without a reason.
	enum class PortalPresentation {
		// Drawn from the local world, or from an accepted destination image.
		Current,
		// The planner did not demand an image: hidden, invalid or unsupported.
		// PortalView::ImageDemandStatus and ImageHiddenReason say which.
		Unrequested,
		// Demanded, but no destination image has been accepted yet, so nothing
		// beyond the aperture can be drawn, including a body already past it.
		AwaitingImage,
	};

	// Classifies one portal view from its demand outcome and accepted image.
	inline PortalPresentation PresentationOf(const PortalView &view) {
		if (!view.ExternalImage) return PortalPresentation::Current;
		if (view.ImageDemandStatus != PortalDemandStatus::Ready) return PortalPresentation::Unrequested;
		return view.ImportedImage != 0 ? PortalPresentation::Current : PortalPresentation::AwaitingImage;
	}

	// How many levels of portal recursion a renderer will go to.
	//
	// **A ceiling rather than a budget**, because the pool is allocated per level
	// per slot at viewport resolution: four levels of a sixteen-slot scene would
	// be sixty-four full-screen colour and depth pairs, which is video memory
	// nobody asked for. Anything a caller asks for above this is clamped, with
	// the frame drawn rather than refused.
	//
	// @since v0.15
	inline constexpr uint32_t MAX_PORTAL_DEPTH = 4;

	// How many levels of mirror-in-mirror a renderer will resolve.
	//
	// **A ceiling for the same reason `MAX_PORTAL_DEPTH` is one, and it arrived
	// with the same change.** While the surface pass resolved a chain by running
	// itself again, depth cost one extra pass per visible pane per level and an
	// ambitious number merely wasted time. It is a recursion now - each level
	// descends into every *other* pane it can see - so the passes go as
	// `panes × (panes - 1) ^ (depth - 1)` before the per-level visibility test
	// takes them back down, and a scene that asked for eight would ask the device
	// for something it cannot finish inside a frame.
	//
	// Three rather than four, because a mirror is not a hole: what you see at
	// the third bounce is a few pixels of a few pixels and the fourth is not
	// distinguishable from the ambient it fades into, whereas a corridor of
	// portals is the whole subject of the scene it appears in.
	//
	// Anything above this is clamped, with the frame drawn rather than refused.
	//
	// **It is also what the automatic depth is bounded by**, and that is the
	// half worth stating: `SetSurfaceBounces(0)` lets each viewport measure its
	// own depth from the frame it just drew, and a corridor of facing panes says
	// "one deeper" at every level for ever. This is what stops it, and it is why
	// `scene::NextSurfaceBounces` takes a ceiling rather than knowing one.
	//
	// @since v0.15
	inline constexpr uint32_t MAX_SURFACE_DEPTH = 3;

}
