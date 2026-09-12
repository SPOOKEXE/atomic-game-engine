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

		core::Name CaptureNode(const DataCaptureTicket &ticket, DataCaptureChannel channel) {
			const std::string_view suffix = channel == DataCaptureChannel::PbrAlbedo	 ? "-albedo"
											: channel == DataCaptureChannel::PbrMaterial ? "-material"
											: channel == DataCaptureChannel::PbrEmissive ? "-emissive"
																						 : "";
			return suffix.empty() ? ticket.CaptureNode
								  : core::Name(std::string(ticket.CaptureNode.Text()) + std::string(suffix));
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
			default:
				break;
			}
		}

	}

	bool Renderer::QueueDataCapture(const DataCaptureRequest &request, DataCaptureTicket &ticket) {
		RequireOwningThread("QueueDataCapture");
		if (!ValidSnapshotId(request.SnapshotId) || !request.Pipeline.IsValid() ||
			!request.CaptureNode.IsValid() ||
			request.TemporalHistory != DataCaptureTemporalHistory::Preserve ||
			!UniqueChannels(request.Channels) || !ticket.ResourceTokens.empty())
			return false;

		DataCaptureTicket queued{
			.SnapshotId = request.SnapshotId,
			.Pipeline = request.Pipeline,
			.CaptureNode = request.CaptureNode,
			.ViewSlot = request.ViewSlot,
			.TemporalHistory = request.TemporalHistory,
			.Channels = request.Channels,
			.ResourceTokens = {}
		};
		for (const DataCaptureChannel channel : request.Channels) {
			const uint64_t token = QueueResourceImage(
				request.Pipeline,
				CaptureNode(queued, channel),
				request.ViewSlot,
				ResourceImageDelivery::CopiedPixels,
				request.SnapshotId
			);
			if (token == 0) {
				CancelDataCapture(queued);
				return false;
			}
			queued.ResourceTokens.push_back(token);
		}
		ticket = std::move(queued);
		return true;
	}

	DataCapturePoll Renderer::PollDataCapture(DataCaptureTicket &ticket) {
		RequireOwningThread("PollDataCapture");
		DataCapturePoll poll;
		poll.SnapshotId = ticket.SnapshotId;
		poll.Camera = DataCaptureCameraConventions();
		if (ticket.Cancelled) {
			poll.Status = DataCaptureStatus::Cancelled;
			return poll;
		}
		if (ticket.SnapshotId.empty() || ticket.ResourceTokens.size() != ticket.Channels.size() ||
			ticket.Channels.empty()) {
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
		poll.CameraPose.WorldFromCamera = image.CameraWorldFromCamera;
		poll.CameraPose.ProjectionAvailable = image.CameraProjectionAvailable;
		poll.CameraPose.Projection = image.CameraProjection;
		poll.CameraPose.VerticalFieldOfViewRadians = image.CameraFieldOfViewRadians;
		poll.CameraPose.NearPlaneMetres = image.CameraNearPlane;
		poll.CameraPose.FarPlaneMetres = image.CameraFarPlane;
		if (std::any_of(images->begin(), images->end(), [&](const ResourceImage &item) {
				return item.SnapshotId != ticket.SnapshotId;
			})) {
			poll.Status = DataCaptureStatus::Invalid;
			for (const DataCaptureChannel channel : ticket.Channels) {
				DataCapturePlane plane = Plane(channel, ticket);
				plane.Status = DataCaptureStatus::Invalid;
				poll.Planes.push_back(std::move(plane));
			}
			ticket.ResourceTokens.clear();
			return poll;
		}
		poll.Planes.reserve(ticket.Channels.size());
		for (size_t index = 0; index < ticket.Channels.size(); ++index) {
			const DataCaptureChannel channel = ticket.Channels[index];
			const ResourceImage &channelImage = (*images)[index];
			DataCapturePlane plane = Plane(channel, ticket);
			if (channelImage.Status == ResourceImageStatus::Ok) {
				FillPlane(plane, channelImage);
			} else if (channelImage.Status == ResourceImageStatus::Failed) {
				plane.Status = DataCaptureStatus::Failed;
			}
			poll.Planes.push_back(std::move(plane));
		}
		const bool anyReady =
			std::any_of(poll.Planes.begin(), poll.Planes.end(), [](const DataCapturePlane &plane) {
				return plane.Status == DataCaptureStatus::Ready;
			});
		const bool anyUnavailable =
			std::any_of(poll.Planes.begin(), poll.Planes.end(), [](const DataCapturePlane &plane) {
				return plane.Status != DataCaptureStatus::Ready;
			});
		poll.Status = anyReady
						  ? (anyUnavailable ? DataCaptureStatus::Partial : DataCaptureStatus::Ready)
						  : (image.Status == ResourceImageStatus::Failed ? DataCaptureStatus::Failed
																		 : DataCaptureStatus::Unsupported);
		ticket.ResourceTokens.clear();
		return poll;
	}

	void Renderer::CancelDataCapture(DataCaptureTicket &ticket) {
		RequireOwningThread("CancelDataCapture");
		for (const uint64_t token : ticket.ResourceTokens)
			(void)CancelResourceImage(token);
		ticket.ResourceTokens.clear();
		ticket.Cancelled = true;
	}
}
