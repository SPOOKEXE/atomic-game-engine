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
// Runs **before** `gui::Layout` in a frame, and after whatever placed the eye,
// so the camera row it reads is this frame's. Running it after the layout would
// draw one frame at the previous canvas, which on a billboard the player is
// walking towards is a visible lag on the size of everything inside it.
//
// **Headless.** Bounds, a distance and a tangent - no device, so it is testable
// without one.
//
// @tier L12 · client

#include <engine/core/types/Vector3.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>

#include <cstddef>
#include <optional>

namespace engine::scene {
	struct Camera;
}
namespace engine::core {
	struct CFrame;
}

namespace engine::ecs {
	class Store;
}

namespace engine::gui {
	class Router;
	class PointerFrame;
	struct Pointer;
}

namespace engine::render {

	struct SpatialDepthFrameInfo;

	// A copied world-space collector placement, independent of later camera preparation.
	struct SpatialCollector {
		// Entity owning the world-space interface collector.
		ecs::Entity Collector;
		// Resolved canvas size and placement for that entity.
		gui::SpatialCanvas Canvas;
	};

	// Where a world-space pointer landed on a collector's canvas.
	//
	// @since v0.18
	struct SpatialPointer {
		// The collector the pointer hit, or an invalid entity when it hit
		// nothing.
		ecs::Entity Collector;

		// Where it landed, in that canvas's own pixels rather than in studs or
		// on the screen - which is the coordinate space every interface hit test
		// above this already works in.
		core::Vector2 Position;
	};

	// Pointer ray derived from one retained frame's raster projection.
	struct SpatialPickRay {
		// World-space ray and its far-clip extent.
		//@{
		core::Vector3 Origin;
		core::Vector3 Direction;
		float Length = 0.0f;
		float LogicalHeight = 0.0f;
		// The device pixel center supplies depth while Origin/Direction preserve
		// the original logical pointer for GUI hit coordinates.
		core::Vector3 PixelOrigin;
		core::Vector3 PixelDirection;
		float PixelLength = 0.0f;
		bool HasPixelRay = false;
		//@}
	};

	// One projected collector, before GUI hit testing and scene-depth admission.
	struct SpatialPickCandidate {
		// Collector identity and canvas-space pointer location.
		//@{
		ecs::Entity Collector;
		core::Vector2 Position;
		//@}
		// Ray distance, normalized raster depth and overlay policy.
		//@{
		float Distance = 0.0f;
		float DeviceDepth = 0.0f;
		bool AlwaysOnTop = false;
		//@}
	};

	// Current foremost admitted spatial candidate.
	struct SpatialPickSelection {
		// Selected candidate and its last matching GUI paint order.
		//@{
		SpatialPickCandidate Candidate;
		size_t PaintOrder = 0;
		bool HasCandidate = false;
		//@}
	};

	// Admits a candidate only if it passes depth or bypasses occlusion, then
	// applies the same overlay, distance and paint-order precedence as routing.
	bool ConsiderSpatialCandidate(
		SpatialPickSelection &selection,
		const SpatialPickCandidate &candidate,
		size_t paintOrder,
		std::optional<float> sceneDepth,
		bool bypassOcclusion = false
	);

	// Maps a logical viewport pointer to the retained frame's device pixel.
	bool SpatialDepthPixel(
		const gui::Screen &screen,
		const gui::Pointer &pointer,
		const SpatialDepthFrameInfo &frame,
		uint32_t &x,
		uint32_t &y
	);

	// Derives one ray from copied frame metadata and a logical viewport pointer.
	bool MakeSpatialPickRay(
		const gui::Screen &screen,
		const gui::Pointer &pointer,
		const SpatialDepthFrameInfo &frame,
		SpatialPickRay &ray
	);

	// Projects a copied collector without touching a live Store or Router.
	bool ProjectSpatialCollector(
		const SpatialCollector &collector,
		const SpatialDepthFrameInfo &frame,
		const SpatialPickRay &ray,
		bool captured,
		SpatialPickCandidate &candidate
	);

	// Re-resolves every current spatial collector without changing the Store.
	// Both camera arguments must be present, or both absent for a camera-free snapshot.
	bool MatchesSpatialCollectors(
		ecs::Store &store,
		const gui::PointerFrame &frame,
		const gui::Screen &screen,
		const scene::Camera *camera,
		const core::CFrame *geometryFrame
	);

	// Checks whether a captured collector still resolves and accepts interaction.
	// Both camera arguments must be present, or both absent for a camera-free snapshot.
	bool SpatialCollectorResolvable(
		const ecs::Store &store,
		ecs::Entity collector,
		const gui::Screen &screen,
		const scene::Camera *camera,
		const core::CFrame *geometryFrame
	);

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

	// Explicit camera for an offscreen world view. Neither authored camera rows
	// nor ActiveCamera are changed. Null inputs use the active camera.
	size_t ResolveSpatialCanvases(
		ecs::Store &store, const gui::Screen &screen, const scene::Camera *camera, const core::CFrame *frame
	);

	// Projects a window pointer onto the foremost spatial collector that can
	// consume its current operation. The caller gives screen interfaces first
	// refusal through the same router.
	//
	// @param store The world holding the collectors.
	// @param list The current compiled interface.
	// @param screen The viewport containing the pointer.
	// @param pointer Window-space pointer and operation.
	// @param router The router that will consume the selected pointer.
	// @param out Receives the collector and its canvas-space position.
	// @param camera Optional viewport lens, used with frame when both are provided.
	// @param frame Optional viewport camera pose.
	// @param projectionAspect Positive viewport aspect, or zero to use screen aspect.
	// @param geometryFrame Optional camera used for billboard geometry and range.
	// @return Whether a spatial collector can consume the operation.
	bool ResolveSpatialPointer(
		ecs::Store &store,
		const gui::DrawList &list,
		const gui::Screen &screen,
		const gui::Pointer &pointer,
		const gui::Router &router,
		SpatialPointer &out,
		const scene::Camera *camera = nullptr,
		const core::CFrame *frame = nullptr,
		float projectionAspect = 0.0f,
		const core::CFrame *geometryFrame = nullptr
	);

	// Selects screen or spatial coordinates for a window pointer before passing
	// it to Router::Update. A spatial capture retains priority until release;
	// if its plane cannot be projected, the returned pointer cancels it.
	gui::Pointer RouteGuiPointer(
		ecs::Store &store,
		const gui::DrawList &list,
		const gui::Screen &screen,
		const gui::Router &router,
		gui::Pointer pointer,
		const scene::Camera *camera = nullptr,
		const core::CFrame *frame = nullptr,
		float projectionAspect = 0.0f,
		const core::CFrame *geometryFrame = nullptr
	);
}
