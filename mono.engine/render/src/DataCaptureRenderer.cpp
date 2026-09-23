#include "CaptureRecordValidation.hpp"
#include "RendererState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

namespace engine::render {
	namespace {
		constexpr size_t MAX_DATA_CAPTURE_CHANNELS = static_cast<size_t>(DataCaptureChannel::PackedGpu) + 1;
		constexpr uint8_t NO_DATA_CAPTURE_RESOURCE = UINT8_MAX;

		bool UnimplementedTemporalFact(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::OpticalFlow;
		}

		const char *UnavailableProvenance(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::MotionVectors
					   ? "unavailable/camera_reprojection_history_not_verified/v1"
					   : "unavailable/optical_flow_not_implemented/v1";
		}

		bool UniqueChannels(std::span<const DataCaptureChannel> channels) {
			if (channels.empty() || channels.size() > MAX_DATA_CAPTURE_CHANNELS) return false;
			for (size_t first = 0; first < channels.size(); ++first) {
				if (static_cast<size_t>(channels[first]) >= MAX_DATA_CAPTURE_CHANNELS) return false;
				for (size_t second = first + 1; second < channels.size(); ++second)
					if (channels[first] == channels[second]) return false;
			}
			return true;
		}

		bool HasChannel(std::span<const DataCaptureChannel> channels, DataCaptureChannel channel) {
			return std::ranges::find(channels, channel) != channels.end();
		}

		bool HasSecondSurfacePair(std::span<const DataCaptureChannel> channels) {
			return HasChannel(channels, DataCaptureChannel::SecondSurfaceDepth) ==
				   HasChannel(channels, DataCaptureChannel::SecondSurfaceValidity);
		}

		core::Name
		CaptureNode(const DataCaptureTicket &ticket, DataCaptureChannel channel, size_t planeIndex) {
			if (channel == DataCaptureChannel::LocalLightContribution ||
				channel == DataCaptureChannel::LocalLightShadowVisibility) {
				size_t slot = 0;
				for (size_t index = 0; index < planeIndex; ++index)
					if (ticket.Channels[index] == channel) ++slot;
				const char *const name = channel == DataCaptureChannel::LocalLightContribution
											 ? "-local-light-response-"
											 : "-local-light-shadow-visibility-";
				return core::Name(std::string(ticket.CaptureNode.Text()) + name + std::to_string(slot));
			}
			return DataCaptureNode(ticket.CaptureNode, channel);
		}

		bool ValidSnapshotId(std::string_view snapshotId) {
			return !snapshotId.empty() && snapshotId.size() <= 256 &&
				   snapshotId.find('\0') == std::string_view::npos;
		}

		DataCapturePlane Plane(DataCaptureChannel channel, const DataCaptureTicket &ticket) {
			DataCapturePlane plane;
			plane.Channel = channel;
			plane.Status = DataCaptureStatus::Unsupported;
			plane.CaptureNode = ticket.CaptureNode;
			if (channel == DataCaptureChannel::MotionVectors || channel == DataCaptureChannel::OpticalFlow)
				plane.Provenance = UnavailableProvenance(channel);
			return plane;
		}

		void Ready(
			DataCapturePlane &plane,
			core::Name resource,
			uint32_t width,
			uint32_t height,
			uint32_t rowStride,
			DataCaptureScalar scalar,
			DataCaptureColourSpace colourSpace,
			std::vector<std::byte> &bytes
		) {
			plane.Status = DataCaptureStatus::Ready;
			plane.Resource = resource;
			plane.Width = width;
			plane.Height = height;
			plane.RowStride = rowStride;
			plane.Scalar = scalar;
			plane.ColourSpace = colourSpace;
			plane.Bytes = std::move(bytes);
			plane.Hash = assets::Hasher::Of(plane.Bytes);
		}

		void FillPlane(DataCapturePlane &plane, ResourceImage &image) {
			const core::Name display("display"), albedo("albedo"), material("material"), emissive("emissive"),
				depth("linear-depth"), normal("normal"), directionalResponse("directional-response");
			auto primary = [&](core::Name expected,
							   DataCaptureScalar scalar,
							   DataCaptureColourSpace colourSpace,
							   ResourceImageFormat format) {
				if (image.Resource == expected && image.Format == format && !image.Pixels.empty())
					Ready(
						plane,
						image.Resource,
						image.Width,
						image.Height,
						image.RowStride,
						scalar,
						colourSpace,
						image.Pixels
					);
			};
			switch (plane.Channel) {
			case DataCaptureChannel::RgbLinearHdr:
				primary(
					display,
					DataCaptureScalar::Float16,
					DataCaptureColourSpace::Linear,
					ResourceImageFormat::RGBA16_Float
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance =
						"scene_linear_hdr/v2;source=transparent_composited_display;"
						"includes=opaque_sky_fog_portal_mirror_transparent;before=lenses_and_tonemap";
				break;
			case DataCaptureChannel::LinearDepth:
				if (image.DepthResource == depth && !image.Depth.empty())
					Ready(
						plane,
						image.DepthResource,
						image.Width,
						image.Height,
						image.Width * 4,
						DataCaptureScalar::Float32,
						DataCaptureColourSpace::NotApplicable,
						image.Depth
					);
				break;
			case DataCaptureChannel::ShadingNormal:
				if (image.NormalResource == normal && !image.Normal.empty())
					Ready(
						plane,
						image.NormalResource,
						image.Width,
						image.Height,
						image.Width * 4,
						DataCaptureScalar::UNorm10A2,
						DataCaptureColourSpace::NotApplicable,
						image.Normal
					);
				break;
			case DataCaptureChannel::PbrAlbedo:
				primary(
					albedo,
					DataCaptureScalar::UNorm8,
					DataCaptureColourSpace::SRGB,
					ResourceImageFormat::RGBA8_SRGB
				);
				break;
			case DataCaptureChannel::PbrMaterial:
				primary(
					material,
					DataCaptureScalar::UNorm8,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::RGBA8_UNorm
				);
				break;
			case DataCaptureChannel::PbrEmissive:
				primary(
					emissive,
					DataCaptureScalar::Float16,
					DataCaptureColourSpace::Linear,
					ResourceImageFormat::RGBA16_Float
				);
				break;
			case DataCaptureChannel::PbrSpecular:
				if (image.Resource == material && image.Format == ResourceImageFormat::RGBA8_UNorm &&
					image.RowStride >= image.Width * 4) {
					std::vector<std::byte> values(size_t(image.Width) * image.Height);
					for (uint32_t y = 0; y < image.Height; ++y)
						for (uint32_t x = 0; x < image.Width; ++x)
							values[size_t(y) * image.Width + x] =
								image.Pixels[size_t(y) * image.RowStride + size_t(x) * 4 + 3];
					Ready(
						plane,
						material,
						image.Width,
						image.Height,
						image.Width,
						DataCaptureScalar::UNorm8,
						DataCaptureColourSpace::NotApplicable,
						values
					);
					plane.Provenance = "authored_specular_factor/v1;source=material_alpha;range=zero_to_one";
				}
				break;
			case DataCaptureChannel::PbrTransmission:
				if (image.Resource == emissive && image.Format == ResourceImageFormat::RGBA16_Float &&
					image.RowStride >= image.Width * 8) {
					std::vector<std::byte> values(size_t(image.Width) * image.Height * 2);
					for (uint32_t y = 0; y < image.Height; ++y)
						for (uint32_t x = 0; x < image.Width; ++x) {
							const size_t source = size_t(y) * image.RowStride + size_t(x) * 8 + 6;
							const size_t destination = (size_t(y) * image.Width + x) * 2;
							values[destination] = image.Pixels[source];
							values[destination + 1] = image.Pixels[source + 1];
						}
					Ready(
						plane,
						emissive,
						image.Width,
						image.Height,
						image.Width * 2,
						DataCaptureScalar::Float16,
						DataCaptureColourSpace::NotApplicable,
						values
					);
					plane.Provenance = "authored_transmission_factor/"
									   "v2;source=emissive_alpha;range=zero_to_one;"
									   "visual_model=authored_ior_thickness_guard_refraction";
				}
				break;
			case DataCaptureChannel::DirectionalResponse:
				primary(
					directionalResponse,
					DataCaptureScalar::Float32,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::RGBA32_Float
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance =
						"directional_response/v1;components=unshadowed_directional_radiance_rgb_"
						"shadow_visibility_a;radiance=linear_after_fog;visibility=directional_shadow_"
						"and_portal_beam_factor;range_a=0_to_1";
				break;
			case DataCaptureChannel::LocalLightContribution:
				if (image.Resource.Text().starts_with("local-light-response-") &&
					image.Format == ResourceImageFormat::RGBA32_Float &&
					image.RowStride >= image.Width * 16 && image.Height > 0 &&
					image.Pixels.size() >=
						size_t(image.Height - 1) * image.RowStride + size_t(image.Width) * 16) {
					std::vector<std::byte> values(size_t(image.Width) * image.Height * 8);
					for (uint32_t y = 0; y < image.Height; ++y)
						for (uint32_t x = 0; x < image.Width; ++x) {
							float rgba[4];
							std::memcpy(
								rgba, image.Pixels.data() + size_t(y) * image.RowStride + size_t(x) * 16, 16
							);
							const uint64_t packed =
								glm::packHalf4x16(glm::vec4(rgba[0], rgba[1], rgba[2], rgba[3]));
							const size_t offset = (size_t(y) * image.Width + x) * 8;
							for (size_t byte = 0; byte < 8; ++byte)
								values[offset + byte] = std::byte((packed >> (byte * 8)) & 0xff);
						}
					Ready(
						plane,
						image.Resource,
						image.Width,
						image.Height,
						image.Width * 8,
						DataCaptureScalar::Float16,
						DataCaptureColourSpace::Linear,
						values
					);
					plane.Provenance = "local_light_contribution/v1;source=single_selected_local_light;"
									   "radiance=additive_linear_before_tonemap;encoding=rgba16_float";
				}
				break;
			case DataCaptureChannel::LocalLightShadowVisibility:
				if (image.Resource.Text().starts_with("local-light-shadow-visibility-") &&
					image.Format == ResourceImageFormat::R8_UNorm && image.RowStride >= image.Width &&
					image.Height > 0 && image.Pixels.size() >= size_t(image.Height) * image.RowStride) {
					Ready(
						plane,
						image.Resource,
						image.Width,
						image.Height,
						image.RowStride,
						DataCaptureScalar::UNorm8,
						DataCaptureColourSpace::NotApplicable,
						image.Pixels
					);
					plane.Provenance =
						"local_light_shadow_visibility/v1;source=selected_local_light_shadow_map;"
						"factor=pcf_visibility;encoding=unorm8_direct;range=zero_to_one";
				}
				break;
			case DataCaptureChannel::MeshUv:
				primary(
					core::Name("mesh-uv"),
					DataCaptureScalar::Float16,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::RG16_Float
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance =
						"authored_mesh_texcoord/v1;components=u_v;units=dimensionless;range=unbounded;"
						"interpolation=perspective_correct;surface=visible_builtin_opaque_or_masked;"
						"validity=both_float16_components_finite";
				break;
			case DataCaptureChannel::AmbientOcclusion:
				primary(
					core::Name("occlusion"),
					DataCaptureScalar::UNorm8,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::R8_UNorm
				);
				if (plane.Status == DataCaptureStatus::Ready) plane.AmbientOcclusion = image.AmbientOcclusion;
				break;
			case DataCaptureChannel::ObjectIds:
			case DataCaptureChannel::SemanticMask:
			case DataCaptureChannel::PartMask:
				primary(
					plane.Channel == DataCaptureChannel::ObjectIds		? core::Name("object-ids")
					: plane.Channel == DataCaptureChannel::SemanticMask ? core::Name("semantic-ids")
																		: core::Name("part-ids"),
					DataCaptureScalar::UInt32,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::R32_UInt
				);
				break;
			case DataCaptureChannel::FirstSurfaceValidity:
				primary(
					core::Name("first-surface-validity"),
					DataCaptureScalar::UNorm8,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::R8_UNorm
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance = "first_surface_depth_test/v1;surface=visible_builtin_opaque_or_masked;"
									   "background=0;validity=0_or_255;transparent_geometry=excluded;"
									   "amodal_ground_truth=false";
				break;
			case DataCaptureChannel::SecondSurfaceDepth:
				if (image.DepthResource == core::Name("second-surface-depth") && !image.Depth.empty())
					Ready(
						plane,
						image.DepthResource,
						image.Width,
						image.Height,
						image.Width * 4,
						DataCaptureScalar::Float32,
						DataCaptureColourSpace::NotApplicable,
						image.Depth
					);
				if (plane.Status == DataCaptureStatus::Ready) plane.Provenance = image.Provenance;
				break;
			case DataCaptureChannel::SecondSurfaceValidity:
				primary(
					core::Name("second-surface-validity"),
					DataCaptureScalar::UNorm8,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::R8_UNorm
				);
				if (plane.Status == DataCaptureStatus::Ready) plane.Provenance = image.Provenance;
				break;
			case DataCaptureChannel::PackedGpu:
				primary(
					core::Name("packed-gpu"),
					DataCaptureScalar::Float32,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::RGBA32_Float
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance = "render_graph_pack_channels/"
									   "v1;mapping=author_defined;resampling=pixel_center_nearest;extent=r";
				break;
			case DataCaptureChannel::MotionVectors:
				primary(
					core::Name("camera-motion-vectors"),
					DataCaptureScalar::Float16,
					DataCaptureColourSpace::NotApplicable,
					ResourceImageFormat::RG16_Float
				);
				if (plane.Status == DataCaptureStatus::Ready)
					plane.Provenance = "camera_reprojection/v1;components=delta_x_delta_y;units=pixels;"
									   "surface=visible_static_builtin_opaque_or_masked;object_motion=false;"
									   "disocclusion=unavailable;camera_history=verified";
				if (plane.Status == DataCaptureStatus::Ready)
					plane.PreviousCameraMotionFrame = image.PreviousCameraMotionFrame;
				break;
			default:
				break;
			}
		}

		void FillShadowVisibility(DataCapturePlane &plane, const DataCapturePlane &directionalResponse) {
			if (directionalResponse.Status != DataCaptureStatus::Ready ||
				directionalResponse.Resource != core::Name("directional-response") ||
				directionalResponse.Scalar != DataCaptureScalar::Float32 || directionalResponse.Width == 0 ||
				directionalResponse.Height == 0 ||
				directionalResponse.RowStride < directionalResponse.Width * 16 ||
				directionalResponse.Bytes.size() !=
					static_cast<size_t>(directionalResponse.Height) * directionalResponse.RowStride)
				return;
			std::vector<std::byte> visibility(
				static_cast<size_t>(directionalResponse.Width) * directionalResponse.Height
			);
			for (uint32_t row = 0; row < directionalResponse.Height; ++row)
				for (uint32_t column = 0; column < directionalResponse.Width; ++column) {
					float factor = 0.0f;
					const size_t source = static_cast<size_t>(row) * directionalResponse.RowStride +
										  static_cast<size_t>(column) * 16 + 12;
					std::memcpy(&factor, directionalResponse.Bytes.data() + source, sizeof(factor));
					if (!std::isfinite(factor)) return;
					visibility[static_cast<size_t>(row) * directionalResponse.Width + column] =
						static_cast<std::byte>(std::lround(std::clamp(factor, 0.0f, 1.0f) * 255.0f));
				}
			Ready(
				plane,
				directionalResponse.Resource,
				directionalResponse.Width,
				directionalResponse.Height,
				directionalResponse.Width,
				DataCaptureScalar::UNorm8,
				DataCaptureColourSpace::NotApplicable,
				visibility
			);
			plane.Provenance =
				"shadow_visibility/v1;source=directional_response_alpha;factor=directional_shadow_"
				"and_portal_beam_visibility;encoding=unorm8_round_to_nearest;source_range=0_to_1";
		}

	}

	bool Renderer::QueueDataCapture(const DataCaptureRequest &request, DataCaptureTicket &ticket) {
		ENGINE_PROFILE_CAT("data capture queue", core::ProfileCategory::Render);
		RequireOwningThread("QueueDataCapture");
		const bool wantsObjectIds =
			std::ranges::find(request.Channels, DataCaptureChannel::ObjectIds) != request.Channels.end();
		const bool wantsSemantic =
			std::ranges::find(request.Channels, DataCaptureChannel::SemanticMask) != request.Channels.end();
		const bool wantsPart =
			std::ranges::find(request.Channels, DataCaptureChannel::PartMask) != request.Channels.end();
		const bool wantsLocalLights =
			HasChannel(request.Channels, DataCaptureChannel::LocalLightContribution) ||
			HasChannel(request.Channels, DataCaptureChannel::LocalLightShadowVisibility);
		if (!ValidSnapshotId(request.SnapshotId) || !request.Pipeline.IsValid() ||
			!request.CaptureNode.IsValid() ||
			request.TemporalHistory != DataCaptureTemporalHistory::Preserve ||
			!UniqueChannels(request.Channels) || !HasSecondSurfacePair(request.Channels) ||
			!ticket.ChannelResourceIndices.empty() || !ticket.ResourceTokens.empty() ||
			(wantsObjectIds && !ValidDataCaptureObjectLabels(request.ObjectLabels)) ||
			(wantsSemantic && !ValidDataCaptureObjectLabels(request.SemanticLabels)) ||
			(wantsPart && !ValidDataCaptureObjectLabels(request.PartLabels)) ||
			wantsLocalLights != !request.LocalLightIds.empty() ||
			request.LocalLightIds.size() > MAX_DATA_CAPTURE_LOCAL_LIGHT_IDS)
			return false;
		for (size_t index = 0; index < request.LocalLightIds.size(); ++index) {
			if (!ValidSnapshotId(request.LocalLightIds[index])) return false;
			for (size_t prior = 0; prior < index; ++prior)
				if (request.LocalLightIds[prior] == request.LocalLightIds[index]) return false;
		}
		std::vector<DataCaptureChannel> expandedChannels;
		std::vector<std::string> expandedLightIds;
		std::vector<uint8_t> localLightMatched;
		for (const DataCaptureChannel channel : request.Channels) {
			if (channel == DataCaptureChannel::LocalLightContribution ||
				channel == DataCaptureChannel::LocalLightShadowVisibility) {
				for (const std::string &id : request.LocalLightIds) {
					expandedChannels.push_back(channel);
					expandedLightIds.push_back(id);
					localLightMatched.push_back(0);
				}
			} else {
				expandedChannels.push_back(channel);
				expandedLightIds.emplace_back();
				localLightMatched.push_back(1);
			}
		}

		const std::vector<uint8_t> localLightShadowAvailable(expandedChannels.size(), 1);

		DataCaptureTicket queued{
			.SnapshotId = request.SnapshotId,
			.Pipeline = request.Pipeline,
			.CaptureNode = request.CaptureNode,
			.ViewSlot = request.ViewSlot,
			.TemporalHistory = request.TemporalHistory,
			.Channels = std::move(expandedChannels),
			.LightIds = std::move(expandedLightIds),
			.LocalLightMatched = std::move(localLightMatched),
			.LocalLightShadowAvailable = localLightShadowAvailable,
			.ObjectLabels = wantsObjectIds ? request.ObjectLabels : std::vector<DataCaptureObjectLabel>{},
			.SemanticLabels =
				wantsSemantic ? request.SemanticLabels : std::vector<DataCaptureSemanticLabel>{},
			.PartLabels = wantsPart ? request.PartLabels : std::vector<DataCapturePartLabel>{},
			.ChannelResourceIndices = {},
			.ResourceTokens = {},
			.GpuTimingId = 0,
			.CompletedImages = {},
			.ImagesTaken = false,
		};
		if (State->NextCaptureTimingId == 0) return false;
		queued.GpuTimingId = State->NextCaptureTimingId++;
		State->CaptureTimings.emplace(queued.GpuTimingId, Impl::CaptureTiming{});
		std::vector<core::Name> resourceNodes;
		for (size_t channelIndex = 0; channelIndex < queued.Channels.size(); ++channelIndex) {
			const DataCaptureChannel channel = queued.Channels[channelIndex];
			if (UnimplementedTemporalFact(channel)) {
				queued.ChannelResourceIndices.push_back(NO_DATA_CAPTURE_RESOURCE);
				continue;
			}
			const core::Name node = CaptureNode(queued, channel, channelIndex);
			const auto existing = std::ranges::find(resourceNodes, node);
			if (existing != resourceNodes.end()) {
				queued.ChannelResourceIndices.push_back(
					static_cast<uint8_t>(existing - resourceNodes.begin())
				);
				continue;
			}
			if (queued.ResourceTokens.size() >= MAX_DATA_FACTORY_READBACK_NODES) {
				CancelDataCapture(queued);
				return false;
			}
			const uint64_t token = QueueResourceImage(
				request.Pipeline,
				node,
				request.ViewSlot,
				ResourceImageDelivery::CopiedPixels,
				request.SnapshotId
			);
			if (token == 0) {
				CancelDataCapture(queued);
				return false;
			}
			for (auto &slot : State->GraphResources.Images)
				if (slot.Phase != Impl::ResourceImagePhase::Free && slot.Image.Request.Token == token) {
					slot.Image.DataCaptureTimingId = queued.GpuTimingId;
					break;
				}
			queued.ChannelResourceIndices.push_back(static_cast<uint8_t>(queued.ResourceTokens.size()));
			queued.ResourceTokens.push_back(token);
			resourceNodes.push_back(node);
		}
		ticket = std::move(queued);
		core::Metrics::Count("render.data_capture.readback_resources", ticket.ResourceTokens.size());
		return true;
	}

	DataCapturePoll Renderer::PollDataCapture(DataCaptureTicket &ticket) {
		ENGINE_PROFILE_CAT("data capture poll", core::ProfileCategory::Render);
		RequireOwningThread("PollDataCapture");
		DataCapturePoll poll;
		poll.SnapshotId = ticket.SnapshotId;
		poll.Camera = DataCaptureCameraConventions();
		poll.ObjectLabels = ticket.ObjectLabels;
		poll.SemanticLabels = ticket.SemanticLabels;
		poll.PartLabels = ticket.PartLabels;
		if (ticket.Cancelled) {
			poll.Status = DataCaptureStatus::Cancelled;
			return poll;
		}
		if (ticket.SnapshotId.empty() || ticket.Channels.empty() ||
			ticket.LightIds.size() != ticket.Channels.size() ||
			ticket.LocalLightMatched.size() != ticket.Channels.size() ||
			(std::ranges::any_of(
				 ticket.Channels,
				 [](DataCaptureChannel channel) {
					 return channel == DataCaptureChannel::LocalLightShadowVisibility;
				 }
			 ) &&
			 ticket.LocalLightShadowAvailable.size() != ticket.Channels.size()) ||
			ticket.ChannelResourceIndices.size() != ticket.Channels.size() ||
			!HasSecondSurfacePair(ticket.Channels) ||
			std::ranges::any_of(ticket.ChannelResourceIndices, [&](uint8_t index) {
				return index != NO_DATA_CAPTURE_RESOURCE && index >= ticket.ResourceTokens.size();
			})) {
			poll.Status = DataCaptureStatus::Invalid;
			return poll;
		}

		if (!ticket.ImagesTaken) {
			// A disabled local shadow has no producer. Remove its pending readback
			// before waiting on the remaining selected lights, then compact their
			// resource indices so a partial capture can complete normally.
			for (size_t index = 0; index < ticket.Channels.size(); ++index) {
				if (ticket.Channels[index] != DataCaptureChannel::LocalLightShadowVisibility ||
					(ticket.LocalLightMatched[index] && ticket.LocalLightShadowAvailable[index]) ||
					ticket.ChannelResourceIndices[index] == NO_DATA_CAPTURE_RESOURCE)
					continue;
				(void)CancelResourceImage(ticket.ResourceTokens[ticket.ChannelResourceIndices[index]]);
				ticket.ChannelResourceIndices[index] = NO_DATA_CAPTURE_RESOURCE;
			}
			std::vector<uint8_t> remapped(ticket.ResourceTokens.size(), NO_DATA_CAPTURE_RESOURCE);
			std::vector<uint64_t> retainedTokens;
			for (uint8_t &resource : ticket.ChannelResourceIndices) {
				if (resource == NO_DATA_CAPTURE_RESOURCE) continue;
				if (remapped[resource] == NO_DATA_CAPTURE_RESOURCE) {
					remapped[resource] = static_cast<uint8_t>(retainedTokens.size());
					retainedTokens.push_back(ticket.ResourceTokens[resource]);
				}
				resource = remapped[resource];
			}
			ticket.ResourceTokens = std::move(retainedTokens);
			auto completed = ticket.ResourceTokens.empty()
								 ? std::optional<std::vector<ResourceImage>>(std::in_place)
								 : TakeResourceImages(ticket.ResourceTokens);
			if (!completed) {
				poll.Status = DataCaptureStatus::Pending;
				return poll;
			}
			ticket.CompletedImages = std::move(*completed);
			ticket.ImagesTaken = true;
		}
		State->CollectTimings();
		if (const auto timing = State->CaptureTimings.find(ticket.GpuTimingId);
			timing != State->CaptureTimings.end()) {
			if (timing->second.State == Impl::CaptureTimingState::Pending) {
				poll.Status = DataCaptureStatus::Pending;
				return poll;
			}
			if (timing->second.State == Impl::CaptureTimingState::Ready) {
				poll.GpuNanoseconds = timing->second.Nanoseconds;
				poll.GpuTimingReason.clear();
			} else if (timing->second.State == Impl::CaptureTimingState::Unavailable) {
				poll.GpuTimingReason = timing->second.Reason;
			} else {
				poll.GpuTimingReason = "unavailable/no_capture_gpu_commands";
			}
		}
		auto &images = ticket.CompletedImages;
		poll.CpuReadbackNanoseconds = std::accumulate(
			images.begin(), images.end(), uint64_t{0}, [](uint64_t total, const ResourceImage &image) {
				return total + image.ReadbackCpuNanoseconds;
			}
		);
		for (const ResourceImage &image : images) {
			poll.HostReadbackReservedCapacityBytes += image.ReadbackHostReservedCapacityBytes;
			poll.DeviceReadbackStagingReservedCapacityBytes +=
				image.ReadbackDeviceStagingReservedCapacityBytes;
		}
		if (images.empty()) {
			poll.Pipeline = ticket.Pipeline;
			poll.ViewSlot = ticket.ViewSlot;
			for (const DataCaptureChannel channel : ticket.Channels)
				poll.Planes.push_back(Plane(channel, ticket));
			poll.Status = DataCaptureStatus::Unsupported;
			ticket.ResourceTokens.clear();
			ticket.ChannelResourceIndices.clear();
			ticket.CompletedImages.clear();
			State->CaptureTimings.erase(ticket.GpuTimingId);
			return poll;
		}
		const ResourceImage &image = images.front();
		poll.CaptureFrame = image.CaptureFrame;
		poll.Pipeline = image.Observation ? image.Observation->Pipeline : ticket.Pipeline;
		poll.PipelineRevision = image.Observation ? image.Observation->PipelineRevision : 0;
		poll.WorldName = image.Observation ? image.Observation->WorldName : core::Name{};
		poll.ViewSlot = image.Observation ? image.Observation->ViewSlot : ticket.ViewSlot;
		poll.CameraPose.WorldFromCamera = image.CameraWorldFromCamera;
		poll.CameraPose.ProjectionAvailable = image.CameraProjectionAvailable;
		poll.CameraPose.Projection = image.CameraProjection;
		poll.CameraPose.VerticalFieldOfViewRadians = image.CameraFieldOfViewRadians;
		poll.CameraPose.NearPlaneMetres = image.CameraNearPlane;
		poll.CameraPose.FarPlaneMetres = image.CameraFarPlane;
		poll.CameraPose.CropLeft = image.CameraCropLeft;
		poll.CameraPose.CropTop = image.CameraCropTop;
		poll.CameraPose.CropWidth = image.CameraCropWidth;
		poll.CameraPose.CropHeight = image.CameraCropHeight;
		bool correctNodes = true;
		for (size_t channelIndex = 0; channelIndex < ticket.Channels.size(); ++channelIndex) {
			if (ticket.ChannelResourceIndices[channelIndex] == NO_DATA_CAPTURE_RESOURCE) continue;
			const ResourceImage &channelImage = images[ticket.ChannelResourceIndices[channelIndex]];
			correctNodes = correctNodes && channelImage.Observation &&
						   channelImage.Observation->Node ==
							   CaptureNode(ticket, ticket.Channels[channelIndex], channelIndex);
		}
		if (image.SnapshotId != ticket.SnapshotId || !image.Observation || !correctNodes ||
			std::any_of(images.begin(), images.end(), [&](const ResourceImage &item) {
				return !capture_record_validation::SameCaptureBundle(image, item) || !item.Observation ||
					   item.Observation->Pipeline != image.Observation->Pipeline ||
					   item.Observation->PipelineRevision != image.Observation->PipelineRevision ||
					   item.Observation->WorldName != image.Observation->WorldName ||
					   item.Observation->ViewSlot != image.Observation->ViewSlot;
			})) {
			poll.Status = DataCaptureStatus::Invalid;
			for (const DataCaptureChannel channel : ticket.Channels) {
				DataCapturePlane plane = Plane(channel, ticket);
				plane.Status = DataCaptureStatus::Invalid;
				poll.Planes.push_back(std::move(plane));
			}
			ticket.ResourceTokens.clear();
			ticket.ChannelResourceIndices.clear();
			ticket.CompletedImages.clear();
			State->CaptureTimings.erase(ticket.GpuTimingId);
			return poll;
		}
		poll.Planes.reserve(ticket.Channels.size());
		for (size_t index = 0; index < ticket.Channels.size(); ++index) {
			const DataCaptureChannel channel = ticket.Channels[index];
			DataCapturePlane plane = Plane(channel, ticket);
			plane.LightId = ticket.LightIds[index];
			if ((channel == DataCaptureChannel::LocalLightContribution ||
				 channel == DataCaptureChannel::LocalLightShadowVisibility) &&
				!ticket.LocalLightMatched[index]) {
				plane.Provenance = "unavailable/local_light_not_visible_or_culled/v1";
				poll.Planes.push_back(std::move(plane));
				continue;
			}
			if (channel == DataCaptureChannel::LocalLightShadowVisibility &&
				!ticket.LocalLightShadowAvailable[index]) {
				plane.Provenance = "unavailable/local_light_shadows_disabled/v1";
				poll.Planes.push_back(std::move(plane));
				continue;
			}
			if (channel == DataCaptureChannel::ShadowVisibility) {
				poll.Planes.push_back(std::move(plane));
				continue;
			}
			if (ticket.ChannelResourceIndices[index] == NO_DATA_CAPTURE_RESOURCE) {
				poll.Planes.push_back(std::move(plane));
				continue;
			}
			ResourceImage &channelImage = images[ticket.ChannelResourceIndices[index]];
			if (channelImage.Status == ResourceImageStatus::Ok) {
				FillPlane(plane, channelImage);
			} else if (channelImage.Status == ResourceImageStatus::Failed) {
				plane.Status = DataCaptureStatus::Failed;
			}
			poll.Planes.push_back(std::move(plane));
		}
		for (size_t index = 0; index < ticket.Channels.size(); ++index) {
			if (ticket.Channels[index] != DataCaptureChannel::ShadowVisibility) continue;
			DataCapturePlane &plane = poll.Planes[index];
			if (ticket.ChannelResourceIndices[index] == NO_DATA_CAPTURE_RESOURCE) continue;
			ResourceImage &image = images[ticket.ChannelResourceIndices[index]];
			if (image.Status == ResourceImageStatus::Failed) {
				plane.Status = DataCaptureStatus::Failed;
				continue;
			}
			if (image.Status != ResourceImageStatus::Ok) continue;
			const auto response = std::ranges::find_if(poll.Planes, [](const DataCapturePlane &candidate) {
				return candidate.Channel == DataCaptureChannel::DirectionalResponse;
			});
			if (response != poll.Planes.end()) {
				FillShadowVisibility(plane, *response);
				continue;
			}
			DataCapturePlane temporary;
			temporary.Channel = DataCaptureChannel::DirectionalResponse;
			temporary.CaptureNode = ticket.CaptureNode;
			FillPlane(temporary, image);
			FillShadowVisibility(plane, temporary);
		}
		const auto secondDepth = std::ranges::find_if(poll.Planes, [](const DataCapturePlane &plane) {
			return plane.Channel == DataCaptureChannel::SecondSurfaceDepth;
		});
		const auto secondValidity = std::ranges::find_if(poll.Planes, [](const DataCapturePlane &plane) {
			return plane.Channel == DataCaptureChannel::SecondSurfaceValidity;
		});
		bool invalidSecondSurfacePair = false;
		if (secondDepth != poll.Planes.end() && secondValidity != poll.Planes.end() &&
			secondDepth->Status != secondValidity->Status) {
			*secondDepth = Plane(DataCaptureChannel::SecondSurfaceDepth, ticket);
			secondDepth->Status = DataCaptureStatus::Invalid;
			*secondValidity = Plane(DataCaptureChannel::SecondSurfaceValidity, ticket);
			secondValidity->Status = DataCaptureStatus::Invalid;
			invalidSecondSurfacePair = true;
		}
		const bool anyReady =
			std::any_of(poll.Planes.begin(), poll.Planes.end(), [](const DataCapturePlane &plane) {
				return plane.Status == DataCaptureStatus::Ready;
			});
		const bool anyUnavailable =
			std::any_of(poll.Planes.begin(), poll.Planes.end(), [](const DataCapturePlane &plane) {
				return plane.Status != DataCaptureStatus::Ready;
			});
		poll.Status = invalidSecondSurfacePair ? DataCaptureStatus::Invalid
					  : anyReady
						  ? (anyUnavailable ? DataCaptureStatus::Partial : DataCaptureStatus::Ready)
						  : (image.Status == ResourceImageStatus::Failed ? DataCaptureStatus::Failed
																		 : DataCaptureStatus::Unsupported);
		ticket.ResourceTokens.clear();
		ticket.ChannelResourceIndices.clear();
		ticket.CompletedImages.clear();
		State->CaptureTimings.erase(ticket.GpuTimingId);
		return poll;
	}

	void Renderer::CancelDataCapture(DataCaptureTicket &ticket) {
		RequireOwningThread("CancelDataCapture");
		for (const uint64_t token : ticket.ResourceTokens)
			(void)CancelResourceImage(token);
		ticket.ResourceTokens.clear();
		ticket.ChannelResourceIndices.clear();
		ticket.CompletedImages.clear();
		State->CaptureTimings.erase(ticket.GpuTimingId);
		ticket.Cancelled = true;
	}
}
