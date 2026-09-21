#pragma once

// Collects every active product scene after one presentation barrier.
//
// A world owns its rows and simulation clock. The client needs copies of those
// rows after `PresentMany` has joined every `PreRender` walk, so rendering can
// submit all active cameras without reopening a world or retaining its store.
// arch-waiver public-header: forward API. Client owns the product collector,
// while its focused suite constructs one directly to verify packet lifetime.

#include <engine/ecs/Entity.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/world/Universe.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace client {
	// Identifies a camera binding across renders while rejecting a replaced store
	// or a new entity with the same authored path.
	std::string CameraTemporalId(
		const engine::core::Name &world, const engine::ecs::Store &store, engine::ecs::Entity camera
	);

	// One copied active-camera packet, ordered by its world's stable name and
	// then its process-local handle. `View` borrows the two owned packets below.
	struct ActiveScene {
		// Stable identifier for world.
		engine::world::WorldId World;
		// Stable identifier for name.
		engine::core::Name Name;
		// Stable identifier for pipeline.
		engine::core::Name Pipeline;
		// Entity associated with camera.
		engine::ecs::Entity Camera;
		// Coordinate frame associated with this record.
		std::unique_ptr<engine::render::WorldViewFrame> Frame;
		// Camera-layer frame owned by the active scene.
		std::unique_ptr<engine::render::WorldCameraFrame> CameraLayers;
		// Render view that borrows the scene frames.
		engine::render::View View;
	};

	// One product presentation request with the renderer key already selected
	// on the driver thread. The key is copied into the packet on its world lane.
	struct ActiveSceneDemand {
		// Presentation request copied from the world lane.
		engine::world::Presentation Request;
		// Stable identifier for pipeline.
		engine::core::Name Pipeline;
	};

	// Runs the product presentation walk once and copies every valid active view.
	class ActiveSceneCollector {
	  public:
		// Presents the requested worlds as one universe batch and copies each
		// active-camera packet on that world's presentation lane. Invalid, remote,
		// retired, and camera-less worlds are omitted.
		//
		// @param universe The worlds and their presentation lanes.
		// @param requests The product views to collect.
		// @param extent Pixel extent used by camera-dependent spatial layers.
		// @param frozen Copies current rows without PreRender. Only an immutable
		// data-factory capture uses this path after its snapshot barrier.
		// @return The number of active camera packets collected.
		size_t Collect(
			engine::world::Universe &universe,
			std::span<const ActiveSceneDemand> requests,
			const engine::core::Vector2 &extent,
			bool frozen = false
		);

		// Builds one camera batch and invokes its sink exactly once. Offscreen
		// targets remain owned by the collector until the next call. A headless
		// product display needs its own target because it has no swapchain.
		engine::render::FrameResult SubmitBatch(
			engine::world::WorldId displayedWorld,
			const engine::render::View &displayedView,
			std::span<const engine::render::View> captureViews,
			uint32_t width,
			uint32_t height,
			bool offscreenDisplayed,
			std::span<const engine::render::WorldContentOwner> foreignContentOwners,
			const std::function<bool(std::span<engine::render::View>)> &prepareCaptures,
			const std::function<engine::render::FrameResult(std::span<engine::render::View>)> &submit
		);
		// Forwards one displayed view without capture views or a capture-preparation callback.
		engine::render::FrameResult SubmitBatch(
			engine::world::WorldId displayedWorld,
			const engine::render::View &displayedView,
			uint32_t width,
			uint32_t height,
			bool offscreenDisplayed,
			std::span<const engine::render::WorldContentOwner> foreignContentOwners,
			const std::function<engine::render::FrameResult(std::span<engine::render::View>)> &submit
		) {
			return SubmitBatch(
				displayedWorld,
				displayedView,
				{},
				width,
				height,
				offscreenDisplayed,
				foreignContentOwners,
				{},
				submit
			);
		}

		// The owned packets in deterministic product order.
		std::span<const ActiveScene> Scenes() const {
			return Collected;
		}

		// The contiguous renderer inputs. Valid until the next `Collect`.
		std::span<const engine::render::View> Views() const {
			return Submitted;
		}

	  private:
		std::vector<ActiveSceneDemand> Demands;
		std::vector<ActiveScene> Collected;
		std::vector<engine::render::View> Submitted;
		std::vector<engine::render::SceneTarget> BatchTargets;
		std::vector<engine::render::View> BatchViews;
	};
}
