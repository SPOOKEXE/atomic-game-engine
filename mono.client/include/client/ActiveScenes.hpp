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
#include <vector>

namespace client {

	// One copied active-camera packet, ordered by its world's stable name and
	// then its process-local handle. `View` borrows the two owned packets below.
	struct ActiveScene {
		engine::world::WorldId World;
		engine::core::Name Name;
		engine::core::Name Pipeline;
		engine::ecs::Entity Camera;
		std::unique_ptr<engine::render::WorldViewFrame> Frame;
		std::unique_ptr<engine::render::WorldCameraFrame> CameraLayers;
		engine::render::View View;
	};

	// One product presentation request with the renderer key already selected
	// on the driver thread. The key is copied into the packet on its world lane.
	struct ActiveSceneDemand {
		engine::world::Presentation Request;
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
		// @return The number of active camera packets collected.
		size_t Collect(
			engine::world::Universe &universe,
			std::span<const ActiveSceneDemand> requests,
			const engine::core::Vector2 &extent
		);

		// Builds one camera batch and invokes its sink exactly once. Offscreen
		// targets remain owned by the collector until the next call. A headless
		// product display needs its own target because it has no swapchain.
		engine::render::FrameResult SubmitBatch(
			engine::world::WorldId displayedWorld,
			const engine::render::View &displayedView,
			uint32_t width,
			uint32_t height,
			bool offscreenDisplayed,
			std::span<const engine::render::WorldContentOwner> foreignContentOwners,
			const std::function<engine::render::FrameResult(std::span<const engine::render::View>)> &submit
		);

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
