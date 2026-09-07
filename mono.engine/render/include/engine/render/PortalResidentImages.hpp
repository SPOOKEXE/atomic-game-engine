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
		using Time = std::chrono::steady_clock::time_point;
		explicit PortalResidentImages(Renderer &renderer);
		~PortalResidentImages();
		PortalResidentImages(const PortalResidentImages &) = delete;
		PortalResidentImages &operator=(const PortalResidentImages &) = delete;
		bool Owns(const Renderer &renderer) const;
		bool Reserve(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalImageRequest &request,
			const PortalImageBinding &binding,
			Time now
		);
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
		uint64_t Take(
			const world::PresentationAddress &source,
			const world::PresentationAddress &producer,
			const PortalResidentReceipt &receipt,
			Time now
		);
		void Cancel(const world::PresentationAddress &source, uint64_t requestId);
		void Invalidate(const world::PresentationAddress &endpoint);
		bool Expire(Time now);
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
