#pragma once

#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalTopologyHost.hpp>

namespace engine::world {
	class HostLink;
}
namespace engine::render {
	// Host policy resolves authored destination names to the correct player replica.
	// Remote endpoints must already be authenticated and registered with the universe.
	struct PortalImageDestination {
		core::Name Authored;
		world::WorldId World;
	};

	// Borrowed only during SubmitEye. The body world can differ from the reply
	// owner while a successor waits; the camera is already in the destination.
	struct PortalEyeGeometrySource {
		world::WorldId World;
		std::span<const scene::DrawInstance> Instances;
		std::span<const core::CFrame> JointFrames;
	};

	// One render-owner adapter shared by all viewports. The universe's presentation
	// bus is configured by the host before use. Universe and renderer outlive this
	// object; remove worlds/viewports before destroying their associated state.
	class PortalImageHost {
	  public:
		using Time = PortalImageInbox::Time;
		PortalImageHost(world::Universe &universe, Renderer &renderer);
		~PortalImageHost();
		PortalImageHost(const PortalImageHost &) = delete;
		PortalImageHost &operator=(const PortalImageHost &) = delete;
		// Advertise this local world's producer without requiring a local viewport.
		// Host control traffic authenticates and distributes the returned endpoint.
		world::PresentationAddress Serve(world::WorldId world);
		bool RequestTopology(world::WorldId source, world::WorldId destination, Time now);
		const PortalTopologySnapshot *Topology(world::WorldId destination, Time now) const;
		// Service an inherited, trusted driver link outside world ticks. False means
		// stop or disconnect; the caller ends its product loop before another tick.
		bool PumpDriverLink(world::HostLink &link);
		// Trusted connection adapter: creates and retires remote route worlds.
		world::PresentationStatus AcceptDriverRoutes(const world::PresentationDirectory &directory);
		size_t Submit(
			world::WorldId source,
			size_t viewSlot,
			std::span<const PortalImageDemand> demands,
			std::span<const PortalImageDestination> destinations,
			Time now
		);
		// Selects the shared eye graph and submits a mapped camera to a named
		// destination. World fields become the local image owner's identity.
		// A missing destination retires the viewport image and selects the empty eye graph.
		// After Pump, refresh EyeImage with Image(view.Slot, view.EyeImageKey).
		size_t SubmitEye(
			world::WorldId source,
			const PortalImageDestination &destination,
			View &view,
			const PortalImageDemandSettings &settings,
			Time now,
			const PortalEyeGeometrySource &geometry = {}
		);
		// Pump once after submitting all demanded views. CPU snapshots remain joined
		// to their destination presentation; only owned messages cross worlds.
		// destinationPresented uses the host's already completed presentations and
		// their per-world interpolation alpha, without running PreRender again.
		PortalProducerProgress
		Pump(float frameSeconds, float alpha, Time now, bool destinationPresented = false);
		// Render a view to submit queued groups even when displayed pixels are cached.
		bool HasPendingUploads() const;
		uint64_t Image(size_t viewSlot, core::Name portal) const;
		// Render current destination-space body rows against this portal's accepted layers.
		// The host owns the result until replacement, expiry or viewport/world removal.
		// A refusal returns zero and retains the preceding resource until retirement.
		uint64_t ComposeBodyImage(core::Name portal, const View &body);

		// Requires a completed current request. Pump first to retire expired receipts.
		uint64_t CurrentImage(size_t viewSlot, core::Name portal) const;
		// Accepted image and camera for this viewport, including retained replies
		// while a newer request is pending. Pump first; the copy does not own pixels.
		std::optional<PortalImageCapture> Capture(size_t viewSlot, core::Name portal) const;
		// Call after authenticated route replacement when the carrying connection
		// changes. Retained images and topology keep their original expiry times.
		void RestartRequests();
		void RemoveViewport(size_t viewSlot);
		void RemoveWorld(world::WorldId world);
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
