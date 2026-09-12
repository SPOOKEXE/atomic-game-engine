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

#include <memory>
#include <span>
#include <vector>

namespace client {

	// One copied active-camera packet, ordered by its world's stable name and
	// then its process-local handle. `View` borrows the two owned packets below.
	struct ActiveScene {
		engine::world::WorldId World;
		engine::core::Name Name;
		engine::ecs::Entity Camera;
		std::unique_ptr<engine::render::WorldViewFrame> Frame;
		std::unique_ptr<engine::render::WorldCameraFrame> CameraLayers;
		engine::render::View View;
	};

	// Runs the product presentation walk once and copies every valid active view.
	class ActiveSceneCollector {
	  public:
		// Presents the requested worlds as one universe batch and copies each
		// active-camera packet on that world's presentation lane. Invalid, remote,
		// retired, and camera-less worlds are omitted.
		//
		// @param extent Pixel extent used by camera-dependent spatial layers.
		// @return The number of active camera packets collected.
		size_t Collect(
			engine::world::Universe &universe,
			std::span<const engine::world::Presentation> requests,
			const engine::core::Vector2 &extent
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
		std::vector<engine::world::Presentation> Demands;
		std::vector<ActiveScene> Collected;
		std::vector<engine::render::View> Submitted;
	};
}
