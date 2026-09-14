#pragma once

#include <engine/assets/ContentHash.hpp>
#include <engine/render/DataCapture.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

namespace engine::render::capture_record_validation {
	struct State {
		std::unordered_set<std::string> Channels;
		std::unordered_set<std::string> Resources;
		size_t ReadyPlanes = 0;
	};

	inline size_t MinimumRowStride(DataCaptureChannel channel, DataCaptureScalar scalar, uint32_t width) {
		const bool valid =
			(channel == DataCaptureChannel::RgbLinearHdr || channel == DataCaptureChannel::PbrEmissive)
				? scalar == DataCaptureScalar::Float16
			: channel == DataCaptureChannel::LinearDepth   ? scalar == DataCaptureScalar::Float32
			: channel == DataCaptureChannel::ShadingNormal ? scalar == DataCaptureScalar::UNorm10A2
			: channel == DataCaptureChannel::ObjectIds	   ? scalar == DataCaptureScalar::UInt32
			: (channel == DataCaptureChannel::SemanticMask || channel == DataCaptureChannel::PartMask)
				? scalar == DataCaptureScalar::UInt32
			: channel == DataCaptureChannel::AmbientOcclusion ? scalar == DataCaptureScalar::UNorm8
			: (channel == DataCaptureChannel::PbrAlbedo || channel == DataCaptureChannel::PbrMaterial)
				? scalar == DataCaptureScalar::UNorm8
				: false;
		const size_t bytesPerPixel =
			channel == DataCaptureChannel::AmbientOcclusion ? 1
			: scalar == DataCaptureScalar::Float16			 ? 8
														 : 4;
		return valid && width > 0 && bytesPerPixel <= std::numeric_limits<size_t>::max() / width
				   ? bytesPerPixel * width
				   : 0;
	}

	inline bool ValidAmbientOcclusion(const AmbientOcclusionProvenance &provenance) {
		const bool noFacts = !provenance.ProducerFrame && !provenance.Enabled && !provenance.SampleCount &&
			!provenance.RadiusWorldUnits && !provenance.Denoiser && !provenance.TemporalHistory &&
			!provenance.BackgroundValue;
		if (provenance.SourceState == AmbientOcclusionSourceState::Unavailable)
			return noFacts && provenance.BackgroundClassification &&
				*provenance.BackgroundClassification == AmbientOcclusionBackgroundClassification::Unavailable;
		if (!provenance.BackgroundValue || *provenance.BackgroundValue != 1.0f ||
			!provenance.BackgroundClassification ||
			*provenance.BackgroundClassification != AmbientOcclusionBackgroundClassification::Unavailable)
			return false;
		if (provenance.SourceState == AmbientOcclusionSourceState::ClearedNoPass)
			return provenance.ProducerFrame && provenance.Enabled &&
				!provenance.SampleCount && !provenance.RadiusWorldUnits && !provenance.Denoiser &&
				!provenance.TemporalHistory;
		const bool builtIn = provenance.Enabled && provenance.SampleCount && provenance.RadiusWorldUnits &&
			provenance.Denoiser && provenance.TemporalHistory &&
			*provenance.Denoiser == AmbientOcclusionDenoiser::None &&
			*provenance.TemporalHistory == AmbientOcclusionTemporalHistory::Disabled;
		return provenance.SourceState == AmbientOcclusionSourceState::Estimated
			? builtIn && *provenance.Enabled && provenance.ProducerFrame.has_value()
			: provenance.SourceState == AmbientOcclusionSourceState::ClearedDisabled
				? builtIn && !*provenance.Enabled && provenance.ProducerFrame.has_value()
				: false;
	}

	inline bool
	Plane(const DataCaptureTicket &ticket, const DataCapturePlane &plane, uint64_t id, State &state) {
		const std::string channel(DataCaptureChannelName(plane.Channel));
		const std::string resource = "capture/" + std::to_string(id) + "/" + channel;
		const bool requested =
			std::find(ticket.Channels.begin(), ticket.Channels.end(), plane.Channel) != ticket.Channels.end();
		const bool ready = plane.Status == DataCaptureStatus::Ready;
		if (ready) state.ReadyPlanes++;
		if (!requested || plane.CaptureNode != ticket.CaptureNode || !state.Channels.insert(channel).second ||
			!state.Resources.insert(resource).second)
			return false;
		if (!ready) {
			const bool terminal = plane.Status == DataCaptureStatus::Unsupported ||
								  plane.Status == DataCaptureStatus::Invalid ||
								  plane.Status == DataCaptureStatus::Failed ||
								  plane.Status == DataCaptureStatus::Cancelled;
			return terminal && !plane.AmbientOcclusion && !plane.Resource.IsValid() && plane.Hash.IsZero() &&
				   plane.Bytes.empty() &&
				   plane.Width == 0 && plane.Height == 0 && plane.RowStride == 0 &&
				   plane.Scalar == DataCaptureScalar::Unknown &&
				   plane.ColourSpace == DataCaptureColourSpace::Unknown;
		}
		const size_t stride = MinimumRowStride(plane.Channel, plane.Scalar, plane.Width);
		if ((plane.Channel == DataCaptureChannel::AmbientOcclusion
				 ? !plane.AmbientOcclusion || !ValidAmbientOcclusion(*plane.AmbientOcclusion)
				 : plane.AmbientOcclusion.has_value()) ||
			!plane.Resource.IsValid() || plane.Hash.IsZero() || plane.Width == 0 || plane.Height == 0 ||
			stride == 0 || plane.RowStride < stride || plane.Origin != DataCaptureOrigin::TopLeft ||
			plane.ColourSpace != ((plane.Channel == DataCaptureChannel::RgbLinearHdr ||
								   plane.Channel == DataCaptureChannel::PbrEmissive)
									  ? DataCaptureColourSpace::Linear
								  : plane.Channel == DataCaptureChannel::PbrAlbedo
									  ? DataCaptureColourSpace::SRGB
									  : DataCaptureColourSpace::NotApplicable) ||
			plane.RowStride == 0 || plane.Height > std::numeric_limits<size_t>::max() / plane.RowStride ||
			plane.Bytes.size() != static_cast<size_t>(plane.Height) * plane.RowStride)
			return false;
		return assets::Hasher::Of(plane.Bytes) == plane.Hash;
	}
}
