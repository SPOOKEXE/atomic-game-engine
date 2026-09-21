#pragma once

// Consumer-owned request correlation and retained copied images. No transport authentication,
// scene storage or GPU residency lives here; the adapter supplies authenticated endpoint metadata.
#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalExchange.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {
	// Borrowed endpoint identity used to correlate portal messages.
	using PortalEndpointView = PortalCaptureTreeEndpointView;

	// Bounds for pending requests and retained completed images.
	struct PortalInboxLimits {
		// Maximum requests awaiting a reply.
		size_t PendingCount = 16;
		// Maximum completed images retained for extraction.
		size_t HeldCount = 16;
		// Total owned bytes allowed for pending requests.
		size_t PendingBytes = 64 * 1024;
		// Total owned bytes allowed for retained completed images.
		size_t HeldBytes = 16 * 1024 * 1024;
		// Deadline from issue to terminal reply.
		std::chrono::milliseconds Timeout{1000};
	};

	// Outcome of issuing, accepting, or expiring a portal message.
	enum class PortalInboxStatus {
		Issued,
		Accepted,
		CompletedFailure,
		Busy,
		Full,
		Invalid,
		Unsolicited,
		EndpointMismatch,
		Stale,
		Malformed,
		ClockRegressed,
		Exhausted
	};

	// The request bytes produced by successful inbox admission.
	struct PortalIssueResult {
		// Admission outcome.
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		// Request with its inbox-assigned correlation key.
		PortalImageRequest Request;
		// Encoded request payload for transport.
		std::vector<std::byte> Wire;
		// Failure detail when Status is not Issued.
		std::string Error;
	};

	// A terminal producer failure retained for an outstanding request.
	struct PortalFailureCompletion {
		// Request identity matched against the pending entry.
		PortalExchangeKey Key;
		// Producer-reported terminal image status.
		PortalImageStatus Status = PortalImageStatus::Failed;
		// Producer diagnostic for the failure.
		std::string Diagnostic;
	};

	// Result of authenticating and admitting a producer reply.
	struct PortalAcceptResult {
		// Acceptance outcome.
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		// Terminal failure copied from a valid failure reply.
		std::optional<PortalFailureCompletion> Failure;
		// Rejection detail when the reply was not accepted.
		std::string Error;
	};

	// Current resource consumption against PortalInboxLimits.
	struct PortalInboxUsage {
		// Current number of pending requests.
		size_t PendingCount = 0;
		// Bytes owned by pending requests.
		size_t PendingBytes = 0;
		// Current number of retained completed images.
		size_t HeldCount = 0;
		// Bytes owned by retained completed images.
		size_t HeldBytes = 0;
	};

	// Single-owner CPU state. Passed times must come from the same steady clock.
	// Byte limits count owned text/pixels and lens records; record counts bound other metadata.
	class PortalImageInbox {
	  public:
		// Steady-clock timestamp used for issue and expiry deadlines.
		using Time = std::chrono::steady_clock::time_point;
		// Creates an empty inbox using validated resource limits.
		explicit PortalImageInbox(PortalInboxLimits limits = {});

		// RequestId is assigned here. Same camera/seam revisions return Busy while pending.
		// A changed revision supersedes that portal's pending request. Callers should throttle
		// moving-camera requests, since constantly superseding them can starve completions.
		PortalIssueResult
		Issue(PortalEndpointView local, PortalEndpointView remote, PortalImageRequest request, Time now);

		// Call only after the outer transport authenticates both endpoints. Correlation must be
		// RequestId. Endpoint and borrowed key/dimension matching precede image decoding/allocation.
		// Recursive captures additionally require caller-approved child provenance; the default
		// rejects every non-root endpoint. Actual aggregate image pixels are admitted together.
		// publishedProducer is optional and must come from a trusted full-tuple relay binding;
		// it changes embedded root identity matching, never outer transport authentication.
		PortalAcceptResult AcceptAuthenticated(
			PortalEndpointView from,
			PortalEndpointView to,
			uint64_t correlation,
			std::span<const std::byte> payload,
			Time now,
			const PortalCaptureTreeAllowChild &allowChild = {},
			PortalEndpointView publishedProducer = {}
		);

		// Transfers ownership out, after expiring entries at now. Failure replies never replace held images.
		// Each extractor leaves images belonging to the other profile untouched.
		std::optional<PortalImageReply>
		Take(PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now);
		// Transfers a completed ordered-layer capture out of the inbox.
		std::optional<PortalImageLayerSet>
		TakeLayers(PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now);
		// Transfers a completed recursive capture tree out of the inbox.
		std::optional<PortalCaptureTree>
		TakeTree(PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now);
		// Expires requests and completed images whose deadlines have passed.
		bool Expire(Time now);
		// Rolls back an unsent request without discarding the previous accepted image.
		bool CancelRequest(uint64_t requestId);
		// Drops requests and images associated with an endpoint incarnation.
		void InvalidateEndpoint(PortalEndpointView endpoint);
		// Drops requests and images for one local portal key.
		void InvalidatePortal(PortalEndpointView local, std::string_view portal);
		// Request IDs never reset, including across Clear, so a delayed reply cannot become current again.
		void Clear();
		// Returns a snapshot of resource use against PortalInboxLimits.
		PortalInboxUsage Usage() const;

	  private:
		struct Address {
			std::string World;
			std::string Channel;
			uint64_t Session = 0;
			uint64_t Generation = 0;
			bool Matches(PortalEndpointView view) const;
			size_t Bytes() const;
		};
		struct Pending {
			Address Local;
			Address Remote;
			PortalExchangeKey Key;
			PortalImageScope Scope = PortalImageScope::CompleteWorld;
			uint32_t Width = 0;
			uint32_t Height = 0;
			Time Deadline;
			bool OrderedLayers = false;
			uint32_t RecursionDepth = 0, PixelBudget = 0;
			PortalCaptureTreeCamera Camera;
			std::string RetainedBodyPlayer{};
			size_t Bytes() const;
		};
		struct Held {
			Address Local;
			Address Remote;
			PortalImageReply Reply;
			Time Deadline;
			std::vector<PortalImageReply> Transparent;
			std::optional<PortalImageReply> SpatialOverlay = std::nullopt;
			PortalCaptureLenses Lenses{};
			std::optional<PortalCaptureTree> Tree = std::nullopt;
			size_t TreeBytes = 0;
			size_t Bytes() const;
		};
		bool ValidLimits() const;
		PortalInboxLimits Limits;
		uint64_t NextRequest = 1;
		std::optional<Time> LastTime;
		std::vector<Pending> Requests;
		std::vector<Held> Images;
	};
}
