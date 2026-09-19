#include "CaptureRecordValidation.hpp"
#include "RendererState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <numeric>
#include <utility>

namespace engine::render {
	namespace {
		constexpr size_t MAX_DATA_CAPTURE_CHANNELS = static_cast<size_t>(DataCaptureChannel::OpticalFlow) + 1;
		constexpr uint8_t NO_DATA_CAPTURE_RESOURCE = UINT8_MAX;

		bool AuthoredFactUnavailable(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::PbrSpecular ||
				   channel == DataCaptureChannel::PbrTransmission;
		}

		bool UnimplementedTemporalFact(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::OpticalFlow;
		}

		const char *UnavailableProvenance(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::PbrSpecular
					   ? "unavailable/authored_specular_not_in_current_material_model/v1"
				   : channel == DataCaptureChannel::PbrTransmission
					   ? "unavailable/authored_transmission_not_in_current_material_model/v1"
				   : channel == DataCaptureChannel::MotionVectors
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

		core::Name CaptureNode(const DataCaptureTicket &ticket, DataCaptureChannel channel) {
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
			if (AuthoredFactUnavailable(channel) || channel == DataCaptureChannel::MotionVectors ||
				channel == DataCaptureChannel::OpticalFlow)
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
			const core::Name lit("lit"), albedo("albedo"), material("material"), emissive("emissive"),
				depth("linear-depth"), normal("normal");
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
					lit,
					DataCaptureScalar::Float16,
					DataCaptureColourSpace::Linear,
					ResourceImageFormat::RGBA16_Float
				);
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
					plane.Provenance =
						"first_surface_depth_test/v1;surface=visible_builtin_opaque_or_masked;"
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
		if (!ValidSnapshotId(request.SnapshotId) || !request.Pipeline.IsValid() ||
			!request.CaptureNode.IsValid() ||
			request.TemporalHistory != DataCaptureTemporalHistory::Preserve ||
			!UniqueChannels(request.Channels) || !HasSecondSurfacePair(request.Channels) ||
			!ticket.ChannelResourceIndices.empty() || !ticket.ResourceTokens.empty() ||
			(wantsObjectIds && !ValidDataCaptureObjectLabels(request.ObjectLabels)) ||
			(wantsSemantic && !ValidDataCaptureObjectLabels(request.SemanticLabels)) ||
			(wantsPart && !ValidDataCaptureObjectLabels(request.PartLabels)))
			return false;

		DataCaptureTicket queued{
			.SnapshotId = request.SnapshotId,
			.Pipeline = request.Pipeline,
			.CaptureNode = request.CaptureNode,
			.ViewSlot = request.ViewSlot,
			.TemporalHistory = request.TemporalHistory,
			.Channels = request.Channels,
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
		for (const DataCaptureChannel channel : request.Channels) {
			if (AuthoredFactUnavailable(channel) || UnimplementedTemporalFact(channel)) {
				queued.ChannelResourceIndices.push_back(NO_DATA_CAPTURE_RESOURCE);
				continue;
			}
			const core::Name node = CaptureNode(queued, channel);
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
			for (auto &slot : State->ResourceImages)
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
			ticket.ChannelResourceIndices.size() != ticket.Channels.size() ||
			!HasSecondSurfacePair(ticket.Channels) ||
			std::ranges::any_of(ticket.ChannelResourceIndices, [&](uint8_t index) {
				return index != NO_DATA_CAPTURE_RESOURCE && index >= ticket.ResourceTokens.size();
			})) {
			poll.Status = DataCaptureStatus::Invalid;
			return poll;
		}

		if (!ticket.ImagesTaken) {
			auto completed = ticket.ResourceTokens.empty() ? std::optional<std::vector<ResourceImage>>(std::in_place)
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
			poll.DeviceReadbackStagingReservedCapacityBytes += image.ReadbackDeviceStagingReservedCapacityBytes;
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
			correctNodes =
				correctNodes && channelImage.Observation &&
				channelImage.Observation->Node == CaptureNode(ticket, ticket.Channels[channelIndex]);
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
