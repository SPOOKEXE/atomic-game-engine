#pragma once

#include <engine/game/PortalSession.hpp>

// Bounded client-side evidence required before a portal may replace its retained image with local geometry.

#include <engine/assets/ContentHash.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace client {
	// An approach is reusable only when the authoritative transfer names the
	// same destination endpoint and signing identity.
	inline bool PortalApproachMatchesTransfer(
		const engine::game::PortalSessionMessage &approach, const engine::game::PortalSessionMessage &transfer
	) {
		return approach.Kind == engine::game::PortalSessionKind::Approach &&
			   transfer.Kind == engine::game::PortalSessionKind::Transfer &&
			   approach.Destination == transfer.Claim.Destination && approach.Identity == transfer.Identity &&
			   approach.Port == transfer.Port;
	}
	// Why the portal remains image-only for this presentation frame.
	enum class PortalImageOnlyReason : uint8_t {
		None,
		OutsideEnterRange,
		ReplicaBaseline,
		Topology,
		Assets,
		PoseRange,
		Capacity,
		StaleCapture
	};

	// The readiness facts sampled from the destination replica and render host.
	struct PortalReadinessEvidence {
		uint64_t RequiredBaseline = 0;
		uint64_t ReplicaBaseline = 0;
		engine::assets::ContentHash RequiredBaselineHash;
		engine::assets::ContentHash ReplicaBaselineHash;
		uint64_t RequiredTopologyRevision = 0;
		uint64_t ReplicaTopologyRevision = 0;
		uint64_t RequiredAuthorityEpoch = 0;
		uint64_t ReplicaAuthorityEpoch = 0;
		uint64_t RequiredPrepareRevision = 0;
		uint64_t ReplicaPrepareRevision = 0;
		std::string RequiredClockDomain;
		std::string ReplicaClockDomain;
		uint64_t RequiredSourceTick = 0;
		uint64_t ReplicaSourceTick = 0;
		uint64_t RequiredDestinationTick = 0;
		uint64_t ReplicaDestinationTick = 0;
		uint64_t RequiredAssetRevision = 0;
		uint64_t ResidentAssetRevision = 0;
		uint64_t RequiredPoseBegin = 0;
		uint64_t RequiredPoseEnd = 0;
		uint64_t ReplicaPoseBegin = 0;
		uint64_t ReplicaPoseEnd = 0;
		float Distance = 0;
		bool AssetsResident = false;
		bool CapacityReserved = false;
		bool RetainedCapture = false;
		bool RetainedCaptureFresh = false;
	};

	// Bounds local portal work independently of how many portal images are visible.
	struct PortalReadinessSettings {
		float EnterDistance = 0;
		float ExitDistance = 0;
		uint64_t CapacityBytes = 0;
	};

	// The allocation held while a destination replica becomes safe to present.
	// Each eye is RGBA16F and the content allowance covers resident assets plus
	// uploads that may coexist with the retained source image.
	struct PortalReadinessReservation {
		uint64_t DestinationEyeBytes = 0;
		uint64_t ReturnEyeBytes = 0;
		uint64_t AssetUploadBytes = 0;

		static std::optional<PortalReadinessReservation>
		ForRgba16fEyes(uint32_t width, uint32_t height, uint64_t assetUploadBytes) {
			constexpr uint64_t RGBA16F_BYTES_PER_PIXEL = 8;
			if (width == 0 || height == 0 || assetUploadBytes == 0) return std::nullopt;
			if (uint64_t(width) > std::numeric_limits<uint64_t>::max() / uint64_t(height))
				return std::nullopt;
			const uint64_t pixels = uint64_t(width) * uint64_t(height);
			if (pixels > std::numeric_limits<uint64_t>::max() / RGBA16F_BYTES_PER_PIXEL) return std::nullopt;
			const uint64_t eyeBytes = pixels * RGBA16F_BYTES_PER_PIXEL;
			if (eyeBytes > std::numeric_limits<uint64_t>::max() - eyeBytes ||
				assetUploadBytes > std::numeric_limits<uint64_t>::max() - 2 * eyeBytes)
				return std::nullopt;
			return PortalReadinessReservation{
				.DestinationEyeBytes = eyeBytes,
				.ReturnEyeBytes = eyeBytes,
				.AssetUploadBytes = assetUploadBytes
			};
		}

		std::optional<uint64_t> TotalBytes() const {
			if (DestinationEyeBytes == 0 || ReturnEyeBytes == 0 || AssetUploadBytes == 0 ||
				DestinationEyeBytes > std::numeric_limits<uint64_t>::max() - ReturnEyeBytes)
				return std::nullopt;
			const uint64_t eyes = DestinationEyeBytes + ReturnEyeBytes;
			if (AssetUploadBytes > std::numeric_limits<uint64_t>::max() - eyes) return std::nullopt;
			return eyes + AssetUploadBytes;
		}
	};

	// The selected presentation route for one portal mouth.
	struct PortalReadiness {
		bool Live = false;
		bool SuppressRetainedCapture = false;
		PortalImageOnlyReason ImageOnly = PortalImageOnlyReason::ReplicaBaseline;
	};

	// Keeps a live portal live through small motion, while any lost readiness fact returns it to image-only
	// immediately.
	class PortalReadinessController {
	  public:
		explicit PortalReadinessController(PortalReadinessSettings settings = {}) : Settings(settings) {}

		// Reserves both views and the destination's content allowance before a
		// capture can be replaced. One crossing owns one complete reservation.
		bool Reserve(const PortalReadinessReservation &reservation) {
			const auto bytes = reservation.TotalBytes();
			if (!bytes || *bytes > Settings.CapacityBytes) return false;
			if (ReservedBytes != 0)
				return ReservedBytes == *bytes &&
					   ReservedReservation.DestinationEyeBytes == reservation.DestinationEyeBytes &&
					   ReservedReservation.ReturnEyeBytes == reservation.ReturnEyeBytes &&
					   ReservedReservation.AssetUploadBytes == reservation.AssetUploadBytes;
			ReservedReservation = reservation;
			ReservedBytes = *bytes;
			return true;
		}

		void Release() {
			ReservedBytes = 0;
			ReservedReservation = {};
			Live = false;
		}

		uint64_t Reserved() const {
			return ReservedBytes;
		}

		PortalReadiness Evaluate(const PortalReadinessEvidence &evidence) {
			const bool wasLive = Live;
			const PortalImageOnlyReason unavailable = Unavailable(evidence);
			if (unavailable != PortalImageOnlyReason::None) {
				Live = false;
				if (evidence.RetainedCapture && !evidence.RetainedCaptureFresh &&
					evidence.Distance <= Settings.ExitDistance)
					return {
						.Live = false,
						.SuppressRetainedCapture = false,
						.ImageOnly = PortalImageOnlyReason::StaleCapture
					};
				if (evidence.Distance > (wasLive ? Settings.ExitDistance : Settings.EnterDistance))
					return {
						.Live = false,
						.SuppressRetainedCapture = false,
						.ImageOnly = PortalImageOnlyReason::OutsideEnterRange
					};
				return {.Live = false, .SuppressRetainedCapture = false, .ImageOnly = unavailable};
			}
			const float limit = Live ? Settings.ExitDistance : Settings.EnterDistance;
			if (evidence.Distance > limit) {
				Live = false;
				return {
					.Live = false,
					.SuppressRetainedCapture = false,
					.ImageOnly = PortalImageOnlyReason::OutsideEnterRange
				};
			}
			Live = true;
			return {
				.Live = true,
				.SuppressRetainedCapture = evidence.RetainedCapture,
				.ImageOnly = PortalImageOnlyReason::None
			};
		}

	  private:
		PortalReadinessSettings Settings;
		bool Live = false;
		uint64_t ReservedBytes = 0;
		PortalReadinessReservation ReservedReservation;

		PortalImageOnlyReason Unavailable(const PortalReadinessEvidence &evidence) const {
			if (evidence.RequiredBaseline == 0 || evidence.ReplicaBaseline != evidence.RequiredBaseline)
				return PortalImageOnlyReason::ReplicaBaseline;
			if (evidence.RequiredBaselineHash.IsZero() ||
				evidence.ReplicaBaselineHash != evidence.RequiredBaselineHash)
				return PortalImageOnlyReason::ReplicaBaseline;
			if (evidence.RequiredTopologyRevision == 0 ||
				evidence.ReplicaTopologyRevision != evidence.RequiredTopologyRevision)
				return PortalImageOnlyReason::Topology;
			if (evidence.RequiredAuthorityEpoch == 0 ||
				evidence.ReplicaAuthorityEpoch != evidence.RequiredAuthorityEpoch ||
				evidence.RequiredPrepareRevision == 0 ||
				evidence.ReplicaPrepareRevision != evidence.RequiredPrepareRevision ||
				evidence.RequiredClockDomain.empty() ||
				evidence.ReplicaClockDomain != evidence.RequiredClockDomain ||
				evidence.RequiredSourceTick == 0 ||
				evidence.ReplicaSourceTick != evidence.RequiredSourceTick ||
				evidence.RequiredDestinationTick == 0 ||
				evidence.ReplicaDestinationTick != evidence.RequiredDestinationTick)
				return PortalImageOnlyReason::ReplicaBaseline;
			if (!evidence.AssetsResident || evidence.RequiredAssetRevision == 0 ||
				evidence.ResidentAssetRevision != evidence.RequiredAssetRevision)
				return PortalImageOnlyReason::Assets;
			if (evidence.ReplicaPoseBegin > evidence.RequiredPoseBegin ||
				evidence.ReplicaPoseEnd < evidence.RequiredPoseEnd)
				return PortalImageOnlyReason::PoseRange;
			if (!evidence.CapacityReserved || ReservedBytes == 0) return PortalImageOnlyReason::Capacity;
			return PortalImageOnlyReason::None;
		}
	};
}
