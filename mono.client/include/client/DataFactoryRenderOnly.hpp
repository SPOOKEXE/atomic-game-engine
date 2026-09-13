#pragma once

// The client owns the handoff from a value-only data-factory request to one
// presentation opportunity. The world service does not name this type or a
// renderer, which keeps headless worlds honest about what they cannot do.

#include <engine/world/DataFactory.hpp>

#include <optional>
#include <string>

namespace client::data_factory_render_only {
	class Queue final {
	  public:
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

		bool Pending() const {
			return Pending_.has_value();
		}

		const engine::world::DataFactoryRenderOnlyRequest *Request() const {
			return Pending_ ? &*Pending_ : nullptr;
		}

		void Consume() {
			Pending_.reset();
		}

		// A forced capture frame draws the already uploaded interface. Routing a
		// click or typing here would mutate the retained world after its snapshot.
		bool AllowsInteractiveGui() const {
			return !Pending_;
		}

		float ParticleDelta(float accumulatedSeconds) const {
			return Pending_ ? 0.0f : accumulatedSeconds;
		}

	  private:
		std::optional<engine::world::DataFactoryRenderOnlyRequest> Pending_;
	};
}
