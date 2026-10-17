#pragma once

#include <engine/render/PortalImageImport.hpp>
#include <engine/render/PortalImageInbox.hpp>
#include <engine/render/PortalShadowImage.hpp>
#include <engine/world/PresentationBus.hpp>
#include <engine/world/World.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace engine::world {
	class Universe;
	struct PresentationBindings;
}
namespace engine::render {
	class Renderer;
	class ShaderLibrary;
	class PortalResidentImages;
	struct View;
	struct WorldContentOwner;
	struct SceneLight;

	// Host setup opens separate endpoints for replies and requests. A source never
	// drains the producer endpoint, even when both roles belong to the same world.
	inline constexpr std::string_view PORTAL_REPLY_CHANNEL = "portal-image-replies";
	// Producer endpoint channel for portal-image requests.
	inline constexpr std::string_view PORTAL_REQUEST_CHANNEL = "portal-image-requests";
	// Open one reply endpoint per viewport. Its canonical name binds the source
	// to that renderer slot, so views of the same mouth cannot consume each other.
	core::Name PortalReplyChannel(size_t viewSlot = 0);

	// Result of issuing a portal request through a source viewport.
	struct PortalRuntimeIssue {
		// Inbox admission outcome.
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		// Outer presentation transport outcome.
		world::PresentationStatus Transport = world::PresentationStatus::Invalid;
		// Correlation assigned to the admitted request.
		uint64_t RequestId = 0;
	};
	// Camera parameters copied into a portal capture request.
	struct PortalCaptureCamera {
		// Position in the producer world.
		std::array<float, 3> Position{};
		// Unit XYZW orientation in the producer world.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Off-axis frustum bounds at the near and far planes.
		std::array<float, 6> Frustum{};
		// Normalized world-space clipping plane.
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
	// Renderer-local state for one completed portal image capture.
	struct PortalImageCapture {
		// Renderer image handle and producer endpoint.
		uint64_t Image = 0;
		// Endpoint that produced the retained image.
		world::PresentationAddress Producer;
		// Sampling transform that maps the image to its portal.
		PortalImageBinding Binding;
		// Camera used to capture the image.
		PortalCaptureCamera Camera;
		// Primary-eye player removed from the capture when authorized.
		std::string EyePlayer;
		// Captured image width in pixels.
		uint32_t Width = 0;
		// Captured image height in pixels.
		uint32_t Height = 0;
		// Captured lighting needed to compose the image.
		std::optional<PortalCaptureLighting> CaptureLighting;
		// Renderer handles for ordered transparent layers.
		std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS> TransparentImages{};
		// Renderer handle for the spatial interface layer.
		uint64_t SpatialOverlayImage = 0;
		// Lens programs applied to the capture.
		PortalCaptureLenses Lenses;
		// Renderer-local lease. Copying this capture does not extend its lifetime.
		uint64_t LensPrograms = 0;
		// Nonzero when the capture owns a complete nested tree on this renderer.
		uint64_t Tree = 0;
		// Authorized body retained in this non-primary capture.
		std::string RetainedBodyPlayer{};
		// Local steady-clock admission time for measuring retained image age.
		PortalImageInbox::Time AcceptedAt{};
		// Source world's capture tick, meaningful only with that world's tick rate.
		uint64_t CaptureTick = 0;
	};
	// Terminal completion reported for one issued portal request.
	struct PortalRuntimeCompletion {
		// Correlation of the completed request.
		uint64_t RequestId = 0;
		// Terminal producer status.
		PortalImageStatus Status = PortalImageStatus::Failed;
		// Renderer image handle, zero on failure.
		uint64_t Image = 0;
		// Producer diagnostic on failure.
		std::string Diagnostic;
		// Accepted child version invalidates parent captures even when the image handle is reused.
		PortalImageVersion Version;
	};
	// Per-frame producer request and rendering totals.
	struct PortalProducerProgress {
		// Requests inspected during this frame.
		size_t Requests = 0;
		// Requests rendered into a new image.
		size_t Rendered = 0;
		// Requests satisfied from a current image.
		size_t Reused = 0;
		// Replies sent to requesting endpoints.
		size_t Sent = 0;
		// Requests refused by validation or resource limits.
		size_t Refused = 0;
	};
	// Reservation cap for the two retained shadow maps.
	inline constexpr size_t MAX_PORTAL_PRODUCER_SHADOW_BYTES = 2 * PORTAL_SHADOW_BYTES;
	// Maximum routed shadow pulls retained by a producer.
	inline constexpr size_t MAX_PORTAL_PRODUCER_SHADOW_ROUTES = 4;
	// Shadow-map memory and transfer totals held by a producer.
	struct PortalProducerShadowUsage {
		// Retained shadow maps.
		size_t Images = 0;
		// Shadow maps with completed readback.
		size_t Ready = 0;
		// Device bytes reserved for retained shadow maps.
		size_t ReservedBytes = 0;
		// CPU bytes held for shadow maps.
		size_t CpuBytes = 0;
		// Retained requester-to-producer routes.
		size_t Routes = 0;
		// Routes whose shadow map is ready.
		size_t ReadyRoutes = 0;
		// CPU bytes used by route metadata.
		size_t RouteMetadataBytes = 0;
		// Shadow-map transfers in flight or completed this frame.
		size_t Transfers = 0;
		// Bytes carried by shadow-map transfer packets.
		size_t TransferPacketBytes = 0;
	};
	// Exact requester-to-producer route for a retained shadow pull.
	struct PortalShadowRoute {
		// Endpoint that requested the shadow map.
		world::PresentationAddress Requester;
		// Endpoint that produced the shadow map.
		world::PresentationAddress Producer;
		// Parent eye request that authorized this route.
		PortalExchangeKey ParentEye;
		// Compares exact shadow routes.
		bool operator==(const PortalShadowRoute &) const = default;
	};
	// How a source hands completed portal captures to its parent.
	enum class PortalImageSourceDelivery { ImportedImages, CapturePayloads };

	// Driver-thread adapter for one world's viewport. Universe and renderer must outlive it.
	// A supplied resident image table must also outlive the source and producer.
	// Receipts belong to trusted host setup, which publishes current endpoint incarnations
	// to Universe. Poll each presentation frame: producer withdrawal/replacement retires
	// retained images, and image expiry is independent of pending requests.
	// Destruction drops every retained GPU image.
	class PortalImageSource {
	  public:
		// Steady-clock timestamp used by inbox deadlines.
		using Time = PortalImageInbox::Time;
		// Connects one world viewport to its portal reply endpoint.
		PortalImageSource(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress replies,
			PortalInboxLimits limits = {},
			PortalResidentImages *resident = nullptr,
			PortalImageSourceDelivery delivery = PortalImageSourceDelivery::ImportedImages
		);
		~PortalImageSource();
		PortalImageSource(const PortalImageSource &) = delete;
		PortalImageSource &operator=(const PortalImageSource &) = delete;
		// One request per mouth stays in flight. A distinct latest camera, body or
		// geometry demand is retained. Poll issues it when an imported image completes;
		// a capture payload waits until TakeTree consumes the completed payload. Seam
		// and endpoint changes supersede immediately.
		PortalRuntimeIssue Issue(
			const world::PresentationAddress &producer,
			PortalImageRequest request,
			PortalImageBinding binding,
			Time now
		);
		// Advances requests, expiry, and completed reply processing.
		std::vector<PortalRuntimeCompletion> Poll(Time now);
		// Current inbox use and cumulative successful decode bytes.
		PortalInboxUsage InboxUsage() const;
		// One bounded protocol reply, separated from ordinary eye-image completions.
		// The caller matches its authenticated envelope and exact outstanding pull.
		std::optional<world::PresentationMessage> TakeShadowReply();
		// Trusted driver mappings outlive this source. Invalidate changed endpoint tuples
		// before replacing the snapshot. Changing the snapshot object clears retained work.
		void SetEndpointBindings(const world::PresentationBindings *bindings);
		// Payload sources retain admitted ordered captures in the bounded inbox until taken.
		// Leaf layer sets become one-node trees. No GPU upload is needed by a parent producer.
		std::optional<PortalCaptureTree> TakeTree(std::string_view portal, Time now);
		// The host must render a view to submit group uploads even when old pixels are cached.
		bool HasPendingUploads() const;
		// Returns the retained image handle, including a previous usable image.
		uint64_t Image(std::string_view portal) const;
		// Only a completed current request qualifies; retained older pixels do not.
		uint64_t CurrentImage(std::string_view portal) const;
		// Value snapshot for composing current geometry against the retained image.
		// A pending request never relabels its camera or sampling matrix. Poll applies expiry.
		// Copying this snapshot does not extend the GPU image lifetime.
		std::optional<PortalImageCapture> Capture(std::string_view portal) const;
		// One host-owned lease for an exact completed imported tree. The first deadline
		// is fixed, at most ten seconds from now; repeated pins cannot renew it.
		// New demands can remain pending; accepted replacement or invalidation retires the lease.
		std::optional<Time> PinCapture(
			std::string_view portal,
			uint64_t exactTree,
			const PortalExchangeKey &exactEye,
			Time now,
			Time requestedDeadline
		);
		// Releases one host pin on the exact retained tree.
		void UnpinCapture(uint64_t exactTree);
		// A replaced transport cannot deliver its old requests. Keep unexpired
		// images, but allow the next Issue to use a new correlation immediately.
		void RestartRequests();
		// Retires work that names an endpoint whose incarnation changed.
		void InvalidateEndpoint(const world::PresentationAddress &endpoint);
		// Retires requests and images for one portal key.
		void InvalidatePortal(std::string_view portal);
		// Drops every pending request and retained renderer image.
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
	// Ordered recursion retains child payloads and authored aperture geometry as a
	// bounded capture tree. Each node keeps its own camera, layers and depth units.
	// Failed replies retry in a separate sixteen-entry queue until their deadline;
	// overflow is refused. Clear cancels both waiting work and retained replies.
	// Call after simulation on the driver thread. PresentMany joins before copying
	// destination presentation; rendering happens after Enter. The provided renderer
	// owns destination assets and shader residency.
	class PortalImageProducer {
	  public:
		// Steady-clock timestamp used for capture and retry deadlines.
		using Time = PortalImageInbox::Time;
		// Trusted host check for an authorized retained body.
		using RetainedBodyAuthorization =
			std::function<bool(const world::PresentationAddress &, std::string_view)>;
		// Trusted host policy, never inferred from request payloads. Replacing the
		// policy cancels pending work; mutable policies are rechecked before use/send.
		void SetRetainedBodyAuthorization(RetainedBodyAuthorization authorize);
		// Transfers the original paired map after its eye reply was delivered. Exact
		// requester incarnation and eye identity are required; policy is checked again.
		// The host authenticates and routes its manifest and tiles. No later world state is sampled.
		// Two maps share a 32 MiB reservation cap, expiring one second after eye delivery.
		std::optional<PortalShadowImage>
		TakeShadow(const world::PresentationAddress &requester, const PortalExchangeKey &eye, Time now);
		// Resolves only targets in the delivered parent's accepted tree. Descendants
		// retain their original immediate-parent request identity, even after TakeShadow.
		std::optional<PortalShadowRoute> ResolveShadowRoute(
			const world::PresentationAddress &parentRequester,
			const PortalExchangeKey &parentEye,
			const PortalCaptureTreeEndpoint &targetProducer,
			const PortalExchangeKey &targetEye,
			Time now
		);
		// Returns current shadow-map memory, route, and transfer totals.
		PortalProducerShadowUsage ShadowUsage() const;
		// A supplied library outlives the producer. Otherwise one is created on demand.
		// Residency defaults to the local world's name, matching direct world views.
		PortalImageProducer(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress requests,
			PortalResidentImages *resident = nullptr,
			ShaderLibrary *shaders = nullptr,
			bool postProcessing = true
		);
		~PortalImageProducer();
		PortalImageProducer(const PortalImageProducer &) = delete;
		PortalImageProducer &operator=(const PortalImageProducer &) = delete;
		// Render-local residency bindings. Copied, never sent in a portal request.
		void SetContentOwner(core::Name owner, std::span<const WorldContentOwner> foreign = {});
		// Local transport endpoints remain unchanged; tree metadata uses the published identity.
		// The host clears affected work before replacing a mapping in this borrowed snapshot.
		void SetEndpointBindings(const world::PresentationBindings *bindings);
		// destinationPresented borrows the completed world's draw rows by copy,
		// preserving the alpha and frame delta already chosen by its host.
		PortalProducerProgress
		Pump(float frameSeconds, float alpha, Time now, bool destinationPresented = false);
		// A retained exact-domain shadow fit has queued a renderer readback.
		bool HasPendingShadowFits() const;
		// Cancels pending captures and releases retained producer state.
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
