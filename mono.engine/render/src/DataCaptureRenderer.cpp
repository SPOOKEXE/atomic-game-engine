#include "CaptureRecordValidation.hpp"
#include "RendererState.hpp"

#include <engine/render/DataCapture.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <utility>

namespace engine::render {
	namespace {
		constexpr size_t MAX_DATA_CAPTURE_CHANNELS = static_cast<size_t>(DataCaptureChannel::OpticalFlow) + 1;

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
			const std::vector<std::byte> &bytes
		) {
			plane.Status = DataCaptureStatus::Ready;
			plane.Resource = resource;
			plane.Width = width;
			plane.Height = height;
			plane.RowStride = rowStride;
			plane.Scalar = scalar;
			plane.ColourSpace = colourSpace;
			plane.Bytes = bytes;
			plane.Hash = assets::Hasher::Of(plane.Bytes);
		}

		void FillPlane(DataCapturePlane &plane, const ResourceImage &image) {
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
			default:
				break;
			}
		}

	}

	bool Renderer::QueueDataCapture(const DataCaptureRequest &request, DataCaptureTicket &ticket) {
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
			.ResourceTokens = {}
		};
		std::vector<core::Name> resourceNodes;
		for (const DataCaptureChannel channel : request.Channels) {
			const core::Name node = CaptureNode(queued, channel);
			const auto existing = std::ranges::find(resourceNodes, node);
			if (existing != resourceNodes.end()) {
				queued.ChannelResourceIndices.push_back(
					static_cast<uint8_t>(existing - resourceNodes.begin())
				);
				continue;
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
			queued.ChannelResourceIndices.push_back(static_cast<uint8_t>(queued.ResourceTokens.size()));
			queued.ResourceTokens.push_back(token);
			resourceNodes.push_back(node);
		}
		ticket = std::move(queued);
		return true;
	}

	DataCapturePoll Renderer::PollDataCapture(DataCaptureTicket &ticket) {
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
			ticket.ChannelResourceIndices.size() != ticket.Channels.size() || ticket.ResourceTokens.empty() ||
			!HasSecondSurfacePair(ticket.Channels) ||
			std::ranges::any_of(ticket.ChannelResourceIndices, [&](uint8_t index) {
				return index >= ticket.ResourceTokens.size();
			})) {
			poll.Status = DataCaptureStatus::Invalid;
			return poll;
		}

		auto images = TakeResourceImages(ticket.ResourceTokens);
		if (!images) {
			poll.Status = DataCaptureStatus::Pending;
			return poll;
		}
		const ResourceImage &image = images->front();
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
			const ResourceImage &channelImage = (*images)[ticket.ChannelResourceIndices[channelIndex]];
			correctNodes = correctNodes && channelImage.Observation &&
				channelImage.Observation->Node == CaptureNode(ticket, ticket.Channels[channelIndex]);
		}
		if (image.SnapshotId != ticket.SnapshotId || !image.Observation || !correctNodes ||
			std::any_of(images->begin(), images->end(), [&](const ResourceImage &item) {
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
			return poll;
		}
		poll.Planes.reserve(ticket.Channels.size());
		for (size_t index = 0; index < ticket.Channels.size(); ++index) {
			const DataCaptureChannel channel = ticket.Channels[index];
			const ResourceImage &channelImage = (*images)[ticket.ChannelResourceIndices[index]];
			DataCapturePlane plane = Plane(channel, ticket);
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
		return poll;
	}

	void Renderer::CancelDataCapture(DataCaptureTicket &ticket) {
		RequireOwningThread("CancelDataCapture");
		for (const uint64_t token : ticket.ResourceTokens)
			(void)CancelResourceImage(token);
		ticket.ResourceTokens.clear();
		ticket.ChannelResourceIndices.clear();
		ticket.Cancelled = true;
	}
}
