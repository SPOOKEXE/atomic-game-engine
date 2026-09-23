#pragma once

#include <engine/assets/ContentHash.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/ResourceImage.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

namespace engine::render::capture_record_validation {
	inline bool SameCaptureBundle(const ResourceImage &first, const ResourceImage &candidate) {
		return candidate.SnapshotId == first.SnapshotId && candidate.CaptureFrame == first.CaptureFrame &&
			   candidate.CameraWorldFromCamera == first.CameraWorldFromCamera &&
			   candidate.CameraProjectionAvailable == first.CameraProjectionAvailable &&
			   candidate.CameraProjection == first.CameraProjection &&
			   candidate.CameraFieldOfViewRadians == first.CameraFieldOfViewRadians &&
			   candidate.CameraNearPlane == first.CameraNearPlane &&
			   candidate.CameraFarPlane == first.CameraFarPlane &&
			   candidate.CameraCropLeft == first.CameraCropLeft &&
			   candidate.CameraCropTop == first.CameraCropTop &&
			   candidate.CameraCropWidth == first.CameraCropWidth &&
			   candidate.CameraCropHeight == first.CameraCropHeight &&
			   candidate.CaptureWidth == first.CaptureWidth && candidate.CaptureHeight == first.CaptureHeight;
	}

	struct State {
		// Channel plus source light for local-light planes, channel alone otherwise.
		std::unordered_set<std::string> Channels;
		std::unordered_set<std::string> Resources;
		size_t ReadyPlanes = 0;
		std::optional<std::pair<uint32_t, uint32_t>> SecondSurfaceExtent;
		std::optional<std::string> SecondSurfaceProvenance;
		std::optional<DataCaptureStatus> SecondSurfaceStatus;
	};

	inline size_t MinimumRowStride(DataCaptureChannel channel, DataCaptureScalar scalar, uint32_t width) {
		const bool valid =
			(channel == DataCaptureChannel::RgbLinearHdr || channel == DataCaptureChannel::PbrEmissive ||
			 channel == DataCaptureChannel::PbrTransmission ||
			 channel == DataCaptureChannel::LocalLightContribution)
				? scalar == DataCaptureScalar::Float16
			: (channel == DataCaptureChannel::MeshUv || channel == DataCaptureChannel::MotionVectors)
				? scalar == DataCaptureScalar::Float16
			: (channel == DataCaptureChannel::LinearDepth ||
			   channel == DataCaptureChannel::SecondSurfaceDepth ||
			   channel == DataCaptureChannel::PackedGpu || channel == DataCaptureChannel::DirectionalResponse)
				? scalar == DataCaptureScalar::Float32
			: channel == DataCaptureChannel::ShadingNormal ? scalar == DataCaptureScalar::UNorm10A2
			: channel == DataCaptureChannel::ObjectIds	   ? scalar == DataCaptureScalar::UInt32
			: (channel == DataCaptureChannel::SemanticMask || channel == DataCaptureChannel::PartMask)
				? scalar == DataCaptureScalar::UInt32
			: (channel == DataCaptureChannel::AmbientOcclusion ||
			   channel == DataCaptureChannel::ShadowVisibility ||
			   channel == DataCaptureChannel::FirstSurfaceValidity ||
			   channel == DataCaptureChannel::SecondSurfaceValidity ||
			   channel == DataCaptureChannel::PbrSpecular)
				? scalar == DataCaptureScalar::UNorm8
			: (channel == DataCaptureChannel::PbrAlbedo || channel == DataCaptureChannel::PbrMaterial)
				? scalar == DataCaptureScalar::UNorm8
				: false;
		const size_t bytesPerPixel =
			(channel == DataCaptureChannel::AmbientOcclusion ||
			 channel == DataCaptureChannel::ShadowVisibility ||
			 channel == DataCaptureChannel::FirstSurfaceValidity ||
			 channel == DataCaptureChannel::SecondSurfaceValidity ||
			 channel == DataCaptureChannel::PbrSpecular)
				? 1
			: channel == DataCaptureChannel::PbrTransmission										  ? 2
			: (channel == DataCaptureChannel::MeshUv || channel == DataCaptureChannel::MotionVectors) ? 4
			: (channel == DataCaptureChannel::PackedGpu || channel == DataCaptureChannel::DirectionalResponse)
				? 16
			: scalar == DataCaptureScalar::Float16 ? 8
												   : 4;
		return valid && width > 0 && bytesPerPixel <= std::numeric_limits<size_t>::max() / width
				   ? bytesPerPixel * width
				   : 0;
	}

	inline bool ValidAmbientOcclusion(const AmbientOcclusionProvenance &provenance) {
		const bool noFacts = !provenance.ProducerFrame && !provenance.Enabled && !provenance.SampleCount &&
							 !provenance.RadiusWorldUnits && !provenance.Denoiser &&
							 !provenance.TemporalHistory && !provenance.BackgroundValue;
		if (provenance.SourceState == AmbientOcclusionSourceState::Unavailable)
			return noFacts && provenance.BackgroundClassification &&
				   *provenance.BackgroundClassification ==
					   AmbientOcclusionBackgroundClassification::Unavailable;
		if (!provenance.BackgroundValue || *provenance.BackgroundValue != 1.0f ||
			!provenance.BackgroundClassification ||
			*provenance.BackgroundClassification != AmbientOcclusionBackgroundClassification::Unavailable)
			return false;
		if (provenance.SourceState == AmbientOcclusionSourceState::ClearedNoPass)
			return provenance.ProducerFrame && provenance.Enabled && !provenance.SampleCount &&
				   !provenance.RadiusWorldUnits && !provenance.Denoiser && !provenance.TemporalHistory;
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

	inline bool ValidSecondSurfaceProvenance(std::string_view provenance) {
		constexpr std::string_view suffix =
			";eligibility=built_in_plain_opaque_front_facing;invalid_depth_metres=0;"
			"validity=0_or_255;identity=unavailable;amodal_ground_truth=false";
		constexpr std::array<std::string_view, 3> prefixes{
			"second_surface_depth_peel/v1;source_depth=d16_unorm;equality_bias=one_source_quantum",
			"second_surface_depth_peel/v1;source_depth=d24_unorm;equality_bias=one_source_quantum",
			"second_surface_depth_peel/v1;source_depth=d32_float;equality_bias=next_representable_float",
		};
		return std::ranges::any_of(prefixes, [&](std::string_view prefix) {
			return provenance.size() == prefix.size() + suffix.size() && provenance.starts_with(prefix) &&
				   provenance.ends_with(suffix);
		});
	}

	inline bool ValidFirstSurfaceProvenance(std::string_view provenance) {
		return provenance == "first_surface_depth_test/v1;surface=visible_builtin_opaque_or_masked;"
							 "background=0;validity=0_or_255;transparent_geometry=excluded;"
							 "amodal_ground_truth=false";
	}

	inline bool ValidFirstSurfaceValidityBytes(std::span<const std::byte> bytes) {
		return std::ranges::all_of(bytes, [](std::byte value) {
			return value == std::byte{0} || value == std::byte{255};
		});
	}

	inline bool ValidDirectionalResponseProvenance(std::string_view provenance) {
		return provenance == "directional_response/v1;components=unshadowed_directional_radiance_rgb_"
							 "shadow_visibility_a;radiance=linear_after_fog;visibility=directional_shadow_"
							 "and_portal_beam_factor;range_a=0_to_1";
	}

	inline bool ValidShadowVisibilityProvenance(std::string_view provenance) {
		return provenance ==
			   "shadow_visibility/v1;source=directional_response_alpha;factor=directional_shadow_"
			   "and_portal_beam_visibility;encoding=unorm8_round_to_nearest;source_range=0_to_1";
	}

	inline bool
	Plane(const DataCaptureTicket &ticket, const DataCapturePlane &plane, uint64_t id, State &state) {
		const std::string channel(DataCaptureChannelName(plane.Channel));
		const bool localLight = plane.Channel == DataCaptureChannel::LocalLightContribution;
		const std::string planeKey = localLight ? channel + "\n" + plane.LightId : channel;
		const std::string resource =
			"capture/" + std::to_string(id) + "/" + channel + (localLight ? "/" + plane.LightId : "");
		const bool requested =
			std::find(ticket.Channels.begin(), ticket.Channels.end(), plane.Channel) != ticket.Channels.end();
		const bool ready = plane.Status == DataCaptureStatus::Ready;
		const bool secondSurface = plane.Channel == DataCaptureChannel::SecondSurfaceDepth ||
								   plane.Channel == DataCaptureChannel::SecondSurfaceValidity;
		const DataCaptureChannel counterpart = plane.Channel == DataCaptureChannel::SecondSurfaceDepth
												   ? DataCaptureChannel::SecondSurfaceValidity
												   : DataCaptureChannel::SecondSurfaceDepth;
		if (ready) state.ReadyPlanes++;
		const bool requestedLight =
			!localLight ||
			std::find(ticket.LightIds.begin(), ticket.LightIds.end(), plane.LightId) != ticket.LightIds.end();
		if (!requested || !requestedLight || plane.CaptureNode != ticket.CaptureNode ||
			!state.Channels.insert(planeKey).second || !state.Resources.insert(resource).second)
			return false;
		if (secondSurface && (std::find(ticket.Channels.begin(), ticket.Channels.end(), counterpart) ==
								  ticket.Channels.end() ||
							  (state.SecondSurfaceStatus && *state.SecondSurfaceStatus != plane.Status)))
			return false;
		if (secondSurface) state.SecondSurfaceStatus = plane.Status;
		if (!ready) {
			const bool terminal = plane.Status == DataCaptureStatus::Unsupported ||
								  plane.Status == DataCaptureStatus::Invalid ||
								  plane.Status == DataCaptureStatus::Failed ||
								  plane.Status == DataCaptureStatus::Cancelled;
			const bool localUnavailable =
				localLight && plane.Status == DataCaptureStatus::Unsupported &&
				plane.Provenance == "unavailable/local_light_not_visible_or_culled/v1";
			const bool authoredUnavailable = plane.Channel == DataCaptureChannel::MotionVectors ||
											 plane.Channel == DataCaptureChannel::OpticalFlow;
			const std::string_view expectedUnavailable =
				plane.Channel == DataCaptureChannel::MotionVectors
					? "unavailable/camera_reprojection_history_not_verified/v1"
					: "unavailable/optical_flow_not_implemented/v1";
			return terminal && !plane.AmbientOcclusion && !plane.PreviousCameraMotionFrame &&
				   ((authoredUnavailable ? plane.Status == DataCaptureStatus::Unsupported &&
											   plane.Provenance == expectedUnavailable
										 : localUnavailable || plane.Provenance.empty())) &&
				   !plane.Resource.IsValid() && plane.Hash.IsZero() && plane.Bytes.empty() &&
				   plane.Width == 0 && plane.Height == 0 && plane.RowStride == 0 &&
				   plane.Scalar == DataCaptureScalar::Unknown &&
				   plane.ColourSpace == DataCaptureColourSpace::Unknown;
		}

		const size_t stride = MinimumRowStride(plane.Channel, plane.Scalar, plane.Width);
		const bool validSecondProvenance = ValidSecondSurfaceProvenance(plane.Provenance);
		const bool validFirstProvenance = ValidFirstSurfaceProvenance(plane.Provenance);
		if ((plane.Channel == DataCaptureChannel::AmbientOcclusion
				 ? !plane.AmbientOcclusion || !ValidAmbientOcclusion(*plane.AmbientOcclusion)
				 : plane.AmbientOcclusion.has_value()) ||
			(secondSurface ? !validSecondProvenance
			 : plane.Channel == DataCaptureChannel::DirectionalResponse
				 ? !ValidDirectionalResponseProvenance(plane.Provenance)
			 : plane.Channel == DataCaptureChannel::ShadowVisibility
				 ? !ValidShadowVisibilityProvenance(plane.Provenance)
			 : plane.Channel == DataCaptureChannel::FirstSurfaceValidity
				 ? !validFirstProvenance || !ValidFirstSurfaceValidityBytes(plane.Bytes)
			 : plane.Channel == DataCaptureChannel::MeshUv
				 ? plane.Provenance != "authored_mesh_texcoord/"
									   "v1;components=u_v;units=dimensionless;range=unbounded;interpolation="
									   "perspective_correct;surface=visible_builtin_opaque_or_masked;"
									   "validity=both_float16_components_finite"
			 : plane.Channel == DataCaptureChannel::PackedGpu
				 ? plane.Provenance != "render_graph_pack_channels/"
									   "v1;mapping=author_defined;resampling=pixel_center_nearest;extent=r"
			 : plane.Channel == DataCaptureChannel::RgbLinearHdr
				 ? plane.Provenance != "scene_linear_hdr/v2;source=transparent_composited_display;"
										 "includes=opaque_sky_fog_portal_mirror_transparent;before=lenses_and_tonemap"
			 : plane.Channel == DataCaptureChannel::MotionVectors
				 ? plane.Provenance != "camera_reprojection/v1;components=delta_x_delta_y;units=pixels;"
									   "surface=visible_static_builtin_opaque_or_masked;object_motion=false;"
									   "disocclusion=unavailable;camera_history=verified"
			 : plane.Channel == DataCaptureChannel::PbrSpecular
				 ? plane.Provenance != "authored_specular_factor/v1;source=material_alpha;range=zero_to_one"
			 : plane.Channel == DataCaptureChannel::PbrTransmission
				 ? plane.Provenance != "authored_transmission_factor/"
									   "v1;source=emissive_alpha;range=zero_to_one;"
									   "visual_model=screen_space_refraction;ior=1_5"
			 : plane.Channel == DataCaptureChannel::LocalLightContribution
				 ? plane.Provenance != "local_light_contribution/v1;source=single_selected_local_light;"
									   "radiance=additive_linear_before_tonemap;encoding=rgba16_float"
				 : !plane.Provenance.empty()) ||
			(plane.Channel == DataCaptureChannel::MotionVectors
				 ? !plane.PreviousCameraMotionFrame || *plane.PreviousCameraMotionFrame == 0
				 : plane.PreviousCameraMotionFrame.has_value()) ||
			!plane.Resource.IsValid() || plane.Hash.IsZero() || plane.Width == 0 || plane.Height == 0 ||
			stride == 0 || plane.RowStride < stride || plane.Origin != DataCaptureOrigin::TopLeft ||
			plane.ColourSpace != ((plane.Channel == DataCaptureChannel::RgbLinearHdr ||
								   plane.Channel == DataCaptureChannel::PbrEmissive ||
								   plane.Channel == DataCaptureChannel::LocalLightContribution)
									  ? DataCaptureColourSpace::Linear
								  : plane.Channel == DataCaptureChannel::PbrAlbedo
									  ? DataCaptureColourSpace::SRGB
									  : DataCaptureColourSpace::NotApplicable) ||
			plane.RowStride == 0 || plane.Height > std::numeric_limits<size_t>::max() / plane.RowStride ||
			plane.Bytes.size() != static_cast<size_t>(plane.Height) * plane.RowStride)
			return false;
		if (secondSurface) {
			const std::pair extent{plane.Width, plane.Height};
			if ((state.SecondSurfaceExtent && *state.SecondSurfaceExtent != extent) ||
				(state.SecondSurfaceProvenance && *state.SecondSurfaceProvenance != plane.Provenance))
				return false;
			state.SecondSurfaceExtent = extent;
			state.SecondSurfaceProvenance = plane.Provenance;
		}
		return assets::Hasher::Of(plane.Bytes) == plane.Hash;
	}
}
