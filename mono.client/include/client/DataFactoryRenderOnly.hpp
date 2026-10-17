#pragma once

// The client owns the handoff from a value-only data-factory request to one
// presentation opportunity. The world service does not name this type or a
// renderer, which keeps headless worlds honest about what they cannot do.

#include <engine/world/DataFactory.hpp>

#include <optional>
#include <string>

namespace client::data_factory_render_only {
	// Queue declaration.
	class Queue final {
	  public:
		// Accepts one validated render-only request when no prior request is pending.
		bool Enqueue(const engine::world::DataFactoryRenderOnlyRequest &request, std::string &detail) {
			if (request.TemporalHistory != engine::world::DataFactoryTemporalHistory::Preserve) {
				detail = "reset and disable temporal history require renderer-local history support";
				return false;
			}
			if (Pending_) {
				detail = "a render-only presentation is already queued";
				return false;
			}
			Pending_ = request;
			detail.clear();
			return true;
		}

		// True while a render-only request still owns the queue slot.
		bool Pending() const {
			return Pending_.has_value();
		}

		// Borrows the pending request until Consume clears it.
		const engine::world::DataFactoryRenderOnlyRequest *Request() const {
			return Pending_ ? &*Pending_ : nullptr;
		}

		// Releases the queue slot after the renderer has handled its request.
		void Consume() {
			Pending_.reset();
		}

		// A paused world draws the already uploaded interface. Layout, routing,
		// and typing would mark ECS rows changed after its retained snapshot.
		bool AllowsInteractiveGui(bool allSystemsPaused = false) const {
			return !Pending_ && !allSystemsPaused;
		}

		// Returns zero while capture freezes particle advancement for the pending request.
		float ParticleDelta(float accumulatedSeconds) const {
			return Pending_ ? 0.0f : accumulatedSeconds;
		}

	  private:
		std::optional<engine::world::DataFactoryRenderOnlyRequest> Pending_;
	};
}
