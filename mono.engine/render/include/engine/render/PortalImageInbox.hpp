#pragma once

// Consumer-owned request correlation and retained copied images. No transport authentication,
// scene storage or GPU residency lives here; the adapter supplies authenticated endpoint metadata.
#include <engine/render/PortalExchange.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {
	struct PortalEndpointView {
		std::string_view World;
		std::string_view Channel;
		uint64_t Session = 0;
		uint64_t Generation = 0;
	};

	struct PortalInboxLimits {
		size_t PendingCount = 16;
		size_t HeldCount = 16;
		size_t PendingBytes = 64 * 1024;
		size_t HeldBytes = 16 * 1024 * 1024;
		std::chrono::milliseconds Timeout{1000};
	};

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

	struct PortalIssueResult {
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		PortalImageRequest Request;
		std::vector<std::byte> Wire;
		std::string Error;
	};

	struct PortalFailureCompletion {
		PortalExchangeKey Key;
		PortalImageStatus Status = PortalImageStatus::Failed;
		std::string Diagnostic;
	};

	struct PortalAcceptResult {
		PortalInboxStatus Status = PortalInboxStatus::Invalid;
		std::optional<PortalFailureCompletion> Failure;
		std::string Error;
	};

	struct PortalInboxUsage {
		size_t PendingCount = 0;
		size_t PendingBytes = 0;
		size_t HeldCount = 0;
		size_t HeldBytes = 0;
	};

	// Single-owner CPU state. Passed times must come from the same steady clock.
	// Byte limits count owned text/pixel payload; record counts bound container and metadata overhead.
	class PortalImageInbox {
	  public:
		using Time = std::chrono::steady_clock::time_point;
		explicit PortalImageInbox(PortalInboxLimits limits = {});

		// RequestId is assigned here. Same camera/seam revisions return Busy while pending.
		// A changed revision supersedes that portal's pending request. Callers should throttle
		// moving-camera requests, since constantly superseding them can starve completions.
		PortalIssueResult
		Issue(PortalEndpointView local, PortalEndpointView remote, PortalImageRequest request, Time now);

		// Call only after the outer transport authenticates both endpoints. Correlation must be
		// RequestId. Endpoint and borrowed key/dimension matching precede image decoding/allocation.
		PortalAcceptResult AcceptAuthenticated(
			PortalEndpointView from,
			PortalEndpointView to,
			uint64_t correlation,
			std::span<const std::byte> payload,
			Time now
		);

		// Transfers ownership out, after expiring entries at now. Failure replies never replace held images.
		// Each extractor leaves images belonging to the other profile untouched.
		std::optional<PortalImageReply>
		Take(PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now);
		std::optional<PortalImageLayerSet>
		TakeLayers(PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now);
		bool Expire(Time now);
		// Rolls back an unsent request without discarding the previous accepted image.
		bool CancelRequest(uint64_t requestId);
		void InvalidateEndpoint(PortalEndpointView endpoint);
		void InvalidatePortal(PortalEndpointView local, std::string_view portal);
		// Request IDs never reset, including across Clear, so a delayed reply cannot become current again.
		void Clear();
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
			size_t Bytes() const;
		};
		struct Held {
			Address Local;
			Address Remote;
			PortalImageReply Reply;
			Time Deadline;
			std::vector<PortalImageReply> Transparent;
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
