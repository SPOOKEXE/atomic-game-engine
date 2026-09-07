#include <engine/render/PortalImageInbox.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::render {
	namespace {
		constexpr size_t MAX_RECORDS = 256;
		constexpr size_t MAX_RETAINED_BYTES = 64 * 1024 * 1024;
		constexpr size_t MAX_ENDPOINT_TEXT = 256;
		constexpr auto MAX_TIMEOUT = std::chrono::hours(1);

		bool Valid(PortalEndpointView address) {
			const auto text = [](std::string_view value) {
				return !value.empty() && value.size() <= MAX_ENDPOINT_TEXT &&
					   value.find('\0') == std::string_view::npos;
			};
			return text(address.World) && text(address.Channel) && address.Session != 0 &&
				   address.Generation != 0;
		}
		bool Fits(size_t current, size_t extra, size_t maximum) {
			return extra <= maximum && current <= maximum - extra;
		}
	}

	PortalImageInbox::PortalImageInbox(PortalInboxLimits limits) : Limits(limits) {}

	bool PortalImageInbox::Address::Matches(PortalEndpointView view) const {
		return World == view.World && Channel == view.Channel && Session == view.Session &&
			   Generation == view.Generation;
	}
	size_t PortalImageInbox::Address::Bytes() const {
		return World.size() + Channel.size();
	}
	size_t PortalImageInbox::Pending::Bytes() const {
		return Local.Bytes() + Remote.Bytes() + Key.PortalKey.size();
	}
	size_t PortalImageInbox::Held::Bytes() const {
		size_t bytes = Local.Bytes() + Remote.Bytes() + Reply.Key.PortalKey.size() + Reply.Diagnostic.size() +
					   Reply.Pixels.size() + Reply.Depth.size();
		for (const auto &layer : Transparent)
			bytes += layer.Key.PortalKey.size() + layer.Diagnostic.size() + layer.Pixels.size() +
					 layer.Depth.size();
		return bytes;
	}
	bool PortalImageInbox::ValidLimits() const {
		return Limits.PendingCount > 0 && Limits.PendingCount <= MAX_RECORDS && Limits.HeldCount > 0 &&
			   Limits.HeldCount <= MAX_RECORDS && Limits.PendingBytes > 0 &&
			   Limits.PendingBytes <= MAX_RETAINED_BYTES && Limits.HeldBytes > 0 &&
			   Limits.HeldBytes <= MAX_RETAINED_BYTES && Limits.Timeout.count() > 0 &&
			   Limits.Timeout <= MAX_TIMEOUT;
	}
	PortalInboxUsage PortalImageInbox::Usage() const {
		PortalInboxUsage usage{Requests.size(), 0, Images.size(), 0};
		for (const auto &request : Requests) {
			usage.PendingBytes += request.Bytes();
		}
		for (const auto &image : Images) {
			usage.HeldBytes += image.Bytes();
		}
		return usage;
	}
	bool PortalImageInbox::Expire(Time now) {
		if (!ValidLimits() || (LastTime && now < *LastTime) || now > Time::max() - Limits.Timeout) {
			return false;
		}
		LastTime = now;
		std::erase_if(Requests, [now](const Pending &request) { return now >= request.Deadline; });
		std::erase_if(Images, [now](const Held &image) { return now >= image.Deadline; });
		return true;
	}
	PortalIssueResult PortalImageInbox::Issue(
		PortalEndpointView local, PortalEndpointView remote, PortalImageRequest request, Time now
	) {
		PortalIssueResult result;
		if (!ValidLimits() || !Valid(local) || !Valid(remote)) {
			return result;
		}
		if (!Expire(now)) {
			result.Status = PortalInboxStatus::ClockRegressed;
			return result;
		}
		if (NextRequest == 0) {
			result.Status = PortalInboxStatus::Exhausted;
			return result;
		}
		const auto previous = std::find_if(Requests.begin(), Requests.end(), [&](const Pending &pending) {
			return pending.Local.Matches(local) && pending.Remote.Matches(remote) &&
				   pending.Key.PortalKey == request.Key.PortalKey;
		});
		if (previous != Requests.end() && previous->Key.CameraRevision == request.Key.CameraRevision &&
			previous->Key.SeamRevision == request.Key.SeamRevision) {
			result.Status = PortalInboxStatus::Busy;
			return result;
		}
		const auto usage = Usage();
		const size_t oldBytes = previous == Requests.end() ? 0 : previous->Bytes();
		if (request.Key.PortalKey.size() > Limits.PendingBytes) {
			result.Status = PortalInboxStatus::Full;
			return result;
		}
		const size_t bytes = local.World.size() + local.Channel.size() + remote.World.size() +
							 remote.Channel.size() + request.Key.PortalKey.size();
		if ((previous == Requests.end() && Requests.size() >= Limits.PendingCount) ||
			!Fits(usage.PendingBytes - oldBytes, bytes, Limits.PendingBytes)) {
			result.Status = PortalInboxStatus::Full;
			return result;
		}
		request.Key.RequestId = NextRequest;
		if (!EncodePortalImageRequest(request, result.Wire, result.Error)) {
			return result;
		}
		Pending pending{
			{std::string(local.World), std::string(local.Channel), local.Session, local.Generation},
			{std::string(remote.World), std::string(remote.Channel), remote.Session, remote.Generation},
			request.Key,
			request.Scope,
			request.Width,
			request.Height,
			now + Limits.Timeout,
			request.OrderedLayers
		};
		if (previous == Requests.end()) {
			Requests.push_back(std::move(pending));
		} else {
			*previous = std::move(pending);
		}
		NextRequest = NextRequest == std::numeric_limits<uint64_t>::max() ? 0 : NextRequest + 1;
		result.Status = PortalInboxStatus::Issued;
		result.Request = std::move(request);
		return result;
	}

	PortalAcceptResult PortalImageInbox::AcceptAuthenticated(
		PortalEndpointView from,
		PortalEndpointView to,
		uint64_t correlation,
		std::span<const std::byte> payload,
		Time now
	) {
		PortalAcceptResult result;
		if (!ValidLimits() || !Valid(from) || !Valid(to)) {
			return result;
		}
		if (!Expire(now)) {
			result.Status = PortalInboxStatus::ClockRegressed;
			return result;
		}
		const auto pending =
			std::find_if(Requests.begin(), Requests.end(), [correlation](const Pending &request) {
				return request.Key.RequestId == correlation;
			});
		if (pending == Requests.end()) {
			result.Status = PortalInboxStatus::Unsolicited;
			return result;
		}
		if (!pending->Local.Matches(to) || !pending->Remote.Matches(from)) {
			result.Status = PortalInboxStatus::EndpointMismatch;
			return result;
		}
		const auto layerMatch =
			pending->OrderedLayers
				? MatchPortalImageLayerSet(payload, pending->Key, pending->Width, pending->Height)
				: std::nullopt;
		const auto match = layerMatch
							   ? layerMatch
							   : MatchPortalImageReply(
									 payload, pending->Key, pending->Width, pending->Height, pending->Scope
								 );
		if (!match || (pending->OrderedLayers && !layerMatch && match->Status == PortalImageStatus::Ok)) {
			result.Status = PortalInboxStatus::Stale;
			return result;
		}
		const auto held = std::find_if(Images.begin(), Images.end(), [&](const Held &image) {
			return image.Local.Matches(to) && image.Remote.Matches(from) &&
				   image.Reply.Key.PortalKey == pending->Key.PortalKey;
		});
		const auto usage = Usage();
		// Keep the old image charged while transactional decode owns the replacement too.
		const size_t members = layerMatch ? MAX_PORTAL_TRANSPARENT_LAYERS + 1 : 1;
		const size_t expectedBytes = pending->Bytes() + (members - 1) * pending->Key.PortalKey.size() +
									 size_t(pending->Width) * pending->Height * 8 * members +
									 match->DepthBytes + match->DiagnosticBytes;
		if (match->Status == PortalImageStatus::Ok &&
			((held == Images.end() && Images.size() >= Limits.HeldCount) ||
			 !Fits(usage.HeldBytes, expectedBytes, Limits.HeldBytes))) {
			result.Status = PortalInboxStatus::Full;
			return result;
		}
		if (match->Status == PortalImageStatus::Ok && held != Images.end() &&
			match->CaptureTick < held->Reply.CaptureTick) {
			result.Status = PortalInboxStatus::Stale;
			return result;
		}
		PortalImageReply reply;
		PortalImageLayerSet decoded;
		const bool valid = layerMatch ? DecodePortalImageLayerSet(payload, decoded, result.Error)
									  : DecodePortalImageReply(payload, reply, result.Error);
		if (!valid) {
			result.Status = PortalInboxStatus::Malformed;
			return result;
		}
		if (layerMatch) reply = std::move(decoded.Opaque);
		if (reply.Status != PortalImageStatus::Ok) {
			result.Status = PortalInboxStatus::CompletedFailure;
			result.Failure =
				PortalFailureCompletion{std::move(reply.Key), reply.Status, std::move(reply.Diagnostic)};
			Requests.erase(pending);
			return result;
		}
		Held image{
			pending->Local,
			pending->Remote,
			std::move(reply),
			now + Limits.Timeout,
			std::move(decoded.Transparent)
		};
		if (held == Images.end()) {
			Images.push_back(std::move(image));
		} else {
			*held = std::move(image);
		}
		Requests.erase(pending);
		result.Status = PortalInboxStatus::Accepted;
		return result;
	}
	std::optional<PortalImageReply> PortalImageInbox::Take(
		PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now
	) {
		if (!Expire(now)) {
			return {};
		}
		const auto held = std::find_if(Images.begin(), Images.end(), [&](const Held &image) {
			return image.Local.Matches(local) && image.Remote.Matches(remote) &&
				   image.Reply.Key.PortalKey == portal;
		});
		if (held == Images.end() || !held->Transparent.empty()) {
			return {};
		}
		PortalImageReply reply = std::move(held->Reply);
		Images.erase(held);
		return reply;
	}
	std::optional<PortalImageLayerSet> PortalImageInbox::TakeLayers(
		PortalEndpointView local, PortalEndpointView remote, std::string_view portal, Time now
	) {
		if (!Expire(now)) return {};
		const auto held = std::find_if(Images.begin(), Images.end(), [&](const Held &image) {
			return image.Local.Matches(local) && image.Remote.Matches(remote) &&
				   image.Reply.Key.PortalKey == portal;
		});
		if (held == Images.end() || held->Transparent.size() != MAX_PORTAL_TRANSPARENT_LAYERS) return {};
		PortalImageLayerSet layers{std::move(held->Reply), std::move(held->Transparent)};
		Images.erase(held);
		return layers;
	}

	void PortalImageInbox::InvalidateEndpoint(PortalEndpointView endpoint) {
		std::erase_if(Requests, [&](const Pending &request) {
			return request.Local.Matches(endpoint) || request.Remote.Matches(endpoint);
		});
		std::erase_if(Images, [&](const Held &image) {
			return image.Local.Matches(endpoint) || image.Remote.Matches(endpoint);
		});
	}
	void PortalImageInbox::InvalidatePortal(PortalEndpointView local, std::string_view portal) {
		std::erase_if(Requests, [&](const Pending &request) {
			return request.Local.Matches(local) && request.Key.PortalKey == portal;
		});
		std::erase_if(Images, [&](const Held &image) {
			return image.Local.Matches(local) && image.Reply.Key.PortalKey == portal;
		});
	}
	bool PortalImageInbox::CancelRequest(uint64_t requestId) {
		return std::erase_if(Requests, [requestId](const Pending &request) {
				   return request.Key.RequestId == requestId;
			   }) != 0;
	}
	void PortalImageInbox::Clear() {
		Requests.clear();
		Images.clear();
	}
}
