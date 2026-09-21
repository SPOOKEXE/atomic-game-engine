#pragma once

#include <engine/render/PortalImageImport.hpp>
#include <engine/world/PresentationBus.hpp>

#include <chrono>
#include <optional>

namespace engine::render {
	class Renderer;

	// Host-owned rendezvous for roles sharing one renderer. Owned endpoint values
	// and request metadata identify each capture; the bus never carries its token.
	// All calls use the renderer owner thread. The renderer outlives this table.
	class PortalResidentImages {
	  public:
		// Monotonic clock used for capture expiry and request admission.
		using Time = std::chrono::steady_clock::time_point;
		// Binds the table to the renderer that owns its image tokens.
		explicit PortalResidentImages(Renderer &renderer);
		~PortalResidentImages();
		PortalResidentImages(const PortalResidentImages &) = delete;
		PortalResidentImages &operator=(const PortalResidentImages &) = delete;
		// Checks that the supplied renderer owns every token in this table.
		bool Owns(const Renderer &renderer) const;
		// Reserves a request key before the producer submits an image.
		bool Reserve(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalImageRequest &request,
			const PortalImageBinding &binding,
			Time now
		);
		// Checks for an unexpired reservation with matching endpoints and request.
		bool Contains(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalImageRequest &request,
			Time now
		);
		// On success this table owns token cancellation until Take transfers it.
		bool Publish(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalResidentReceipt &receipt,
			uint64_t token,
			Time now
		);
		// Transfers an accepted resident token to the caller exactly once.
		uint64_t Take(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalResidentReceipt &receipt,
			Time now
		);
		// Cancels a source request and any published token it still owns.
		void Cancel(const world::PresentationAddress &source, uint64_t requestId);
		// Drops entries involving a retired presentation endpoint.
		void Invalidate(const world::PresentationAddress &endpoint);
		// Retires elapsed reservations and reports whether any were removed.
		bool Expire(Time now);
		// Drops all reservations and their still-owned tokens.
		void Clear();

	  private:
		struct Entry {
			world::PresentationAddress Source;
			world::PresentationAddress Producer;
			PortalImageRequest Request;
			PortalImageBinding Binding;
			std::optional<PortalResidentReceipt> Receipt;
			uint64_t Token = 0;
			Time Deadline;
		};
		Renderer &Render;
		std::vector<Entry> Entries;
		std::optional<Time> LastTime;
	};
}
