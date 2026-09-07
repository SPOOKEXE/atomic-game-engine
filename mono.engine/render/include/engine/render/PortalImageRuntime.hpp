#pragma once

#include <engine/render/PortalImageImport.hpp>
#include <engine/render/PortalImageInbox.hpp>
#include <engine/world/PresentationBus.hpp>
#include <engine/world/World.hpp>

#include <array>
#include <memory>
#include <optional>

namespace engine::world {
	class Universe;
}
namespace engine::render {
	class Renderer;
	class PortalResidentImages;
	struct View;
	struct SceneLight;

	// Host setup opens separate endpoints for replies and requests. A source never
	// drains the producer endpoint, even when both roles belong to the same world.
	inline constexpr std::string_view PORTAL_REPLY_CHANNEL = "portal-image-replies";
	inline constexpr std::string_view PORTAL_REQUEST_CHANNEL = "portal-image-requests";
	// Open one reply endpoint per viewport. Its canonical name binds the source
	// to that renderer slot, so views of the same mouth cannot consume each other.
	core::Name PortalReplyChannel(size_t viewSlot = 0);

	struct PortalRuntimeIssue {
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		world::PresentationStatus Transport = world::PresentationStatus::Invalid;
		uint64_t RequestId = 0;
	};
	struct PortalCaptureCamera {
		std::array<float, 3> Position{};
		std::array<float, 4> Orientation{0, 0, 0, 1};
		std::array<float, 6> Frustum{};
		std::array<float, 4> ClipPlane{};
	};
	// Applies only the pose, lens and explicit projection. Both producers and retained
	// body views use this reconstruction. Invalid camera data leaves the view unchanged.
	bool ResolvePortalCaptureCamera(
		const PortalCaptureCamera &camera, PortalImageProjection projection, View &view
	);
	// Replaces captured shading terms and binds caller-owned light storage. Other
	// environment/layer inputs remain the caller's responsibility. Storage must
	// outlive the view's render; invalid input leaves both view and storage unchanged.
	bool ResolvePortalCaptureLighting(
		const PortalCaptureLighting &lighting, View &view, std::span<SceneLight> storage
	);
	struct PortalImageCapture {
		uint64_t Image = 0;
		world::PresentationAddress Producer;
		PortalImageBinding Binding;
		PortalCaptureCamera Camera;
		std::string EyePlayer;
		uint32_t Width = 0, Height = 0;
		std::optional<PortalCaptureLighting> CaptureLighting;
		std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS> TransparentImages{};
	};
	struct PortalRuntimeCompletion {
		uint64_t RequestId = 0;
		PortalImageStatus Status = PortalImageStatus::Failed;
		uint64_t Image = 0;
		std::string Diagnostic;
		// Accepted child version invalidates parent captures even when the image handle is reused.
		PortalImageVersion Version;
	};
	struct PortalProducerProgress {
		size_t Requests = 0;
		size_t Rendered = 0;
		size_t Reused = 0;
		size_t Sent = 0;
		size_t Refused = 0;
	};

	// Driver-thread adapter for one world's viewport. Universe and renderer must outlive it.
	// A supplied resident image table must also outlive the source and producer.
	// Receipts belong to trusted host setup, which publishes current endpoint incarnations
	// to Universe. Poll each presentation frame: producer withdrawal/replacement retires
	// retained images, and image expiry is independent of pending requests.
	// Destruction drops every retained GPU image.
	class PortalImageSource {
	  public:
		using Time = PortalImageInbox::Time;
		PortalImageSource(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress replies,
			PortalInboxLimits limits = {},
			PortalResidentImages *resident = nullptr
		);
		~PortalImageSource();
		PortalImageSource(const PortalImageSource &) = delete;
		PortalImageSource &operator=(const PortalImageSource &) = delete;
		// One request per mouth stays in flight. Retry the latest camera/geometry
		// after Poll completes it; seam and endpoint changes supersede immediately.
		PortalRuntimeIssue Issue(
			const world::PresentationAddress &producer,
			PortalImageRequest request,
			PortalImageBinding binding,
			Time now
		);
		std::vector<PortalRuntimeCompletion> Poll(Time now);
		// The host must render a view to submit group uploads even when old pixels are cached.
		bool HasPendingUploads() const;
		uint64_t Image(std::string_view portal) const;
		// Only a completed current request qualifies; retained older pixels do not.
		uint64_t CurrentImage(std::string_view portal) const;
		// Value snapshot for composing current geometry against the retained image.
		// A pending request never relabels its camera or sampling matrix. Poll applies expiry.
		// Copying this snapshot does not extend the GPU image lifetime.
		std::optional<PortalImageCapture> Capture(std::string_view portal) const;
		// A replaced transport cannot deliver its old requests. Keep unexpired
		// images, but allow the next Issue to use a new correlation immediately.
		void RestartRequests();
		void InvalidateEndpoint(const world::PresentationAddress &endpoint);
		void InvalidatePortal(std::string_view portal);
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};

	// Captures destination HDR composition, including transparency, particles,
	// ribbons and spatial UI. Host/screen UI is excluded. OpaqueLighting opts into
	// capture before transparency. Mixed local mirror/portal cameras share one bounded
	// child-first capture plan, including seam-light work in the pixel budget.
	// Cross-world children use named image requests with reduced depth and shared
	// pixel budgets. Up to sixteen parents wait for replies, with a one-second
	// deadline. Unchanged child images can renew the parent capture.
	// Failed replies retry in a separate sixteen-entry queue until their deadline;
	// overflow is refused. Clear cancels both waiting work and retained replies.
	// Call after simulation on the driver thread. PresentMany joins before copying
	// destination presentation; rendering happens after Enter. The provided renderer
	// owns destination assets and shader residency.
	class PortalImageProducer {
	  public:
		using Time = PortalImageInbox::Time;
		PortalImageProducer(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress requests,
			PortalResidentImages *resident = nullptr
		);
		~PortalImageProducer();
		PortalImageProducer(const PortalImageProducer &) = delete;
		PortalImageProducer &operator=(const PortalImageProducer &) = delete;
		// destinationPresented borrows the completed world's draw rows by copy,
		// preserving the alpha and frame delta already chosen by its host.
		PortalProducerProgress
		Pump(float frameSeconds, float alpha, Time now, bool destinationPresented = false);
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
