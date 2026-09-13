#include "CaptureRecordValidation.hpp"

#include <engine/render/ScriptDataCaptureBridge.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

namespace engine::render {
	namespace {
		constexpr size_t LIMIT = 6;
		constexpr size_t RETAINED_BYTE_LIMIT = 64 * 1024 * 1024;

		bool Text(std::string_view value, size_t limit = 256) {
			return !value.empty() && value.size() <= limit && value.find('\0') == std::string_view::npos;
		}

		std::optional<DataCaptureChannel> Channel(std::string_view name) {
			if (name == "rgb_linear_hdr") return DataCaptureChannel::RgbLinearHdr;
			if (name == "linear_depth") return DataCaptureChannel::LinearDepth;
			if (name == "shading_normal") return DataCaptureChannel::ShadingNormal;
			if (name == "pbr_albedo") return DataCaptureChannel::PbrAlbedo;
			if (name == "pbr_material") return DataCaptureChannel::PbrMaterial;
			if (name == "pbr_emissive") return DataCaptureChannel::PbrEmissive;
			return std::nullopt;
		}

		bool Valid(std::string_view instanceId, const script::DataCaptureBridgeRequest &request) {
			if (!Text(instanceId) || request.InstanceId != instanceId || !Text(request.SnapshotId) ||
				!Text(request.Pipeline) || !Text(request.CaptureNode) || request.Channels.empty() ||
				request.Channels.size() > LIMIT || request.TemporalHistory != "preserve" ||
				request.ViewSlot > std::numeric_limits<size_t>::max())
				return false;
			for (size_t first = 0; first < request.Channels.size(); ++first) {
				if (!Text(request.Channels[first], 64) || !Channel(request.Channels[first])) return false;
				for (size_t second = first + 1; second < request.Channels.size(); ++second)
					if (request.Channels[first] == request.Channels[second]) return false;
			}
			return true;
		}

		const char *Status(DataCaptureStatus status) {
			switch (status) {
			case DataCaptureStatus::Pending:
				return "pending";
			case DataCaptureStatus::Ready:
				return "ready";
			case DataCaptureStatus::Partial:
				return "partial";
			case DataCaptureStatus::Unsupported:
				return "unsupported";
			case DataCaptureStatus::Invalid:
				return "invalid";
			case DataCaptureStatus::Failed:
				return "failed";
			case DataCaptureStatus::Cancelled:
				return "cancelled";
			}
			return "invalid";
		}

		const char *Scalar(DataCaptureScalar scalar) {
			switch (scalar) {
			case DataCaptureScalar::Float16:
				return "float16";
			case DataCaptureScalar::Float32:
				return "float32";
			case DataCaptureScalar::UNorm8:
				return "unorm8";
			case DataCaptureScalar::UNorm10A2:
				return "unorm10a2";
			case DataCaptureScalar::Unknown:
				return "unknown";
			}
			return "unknown";
		}

		const char *ColourSpace(DataCaptureColourSpace colourSpace) {
			switch (colourSpace) {
			case DataCaptureColourSpace::Linear:
				return "linear";
			case DataCaptureColourSpace::SRGB:
				return "srgb";
			case DataCaptureColourSpace::NotApplicable:
				return "not_applicable";
			case DataCaptureColourSpace::Unknown:
				return "unknown";
			}
			return "unknown";
		}

		std::string ResourceId(uint64_t ticket, DataCaptureChannel channel) {
			return "capture/" + std::to_string(ticket) + "/" + std::string(DataCaptureChannelName(channel));
		}

		std::string CoordinateConvention(const DataCaptureCameraConvention &camera) {
			return std::string(camera.RightHandedWorld ? "right_handed_world" : "left_handed_world") + "," +
				   (camera.CameraLooksNegativeZ ? "camera_negative_z" : "camera_positive_z") + "," +
				   (camera.ClipYUp ? "clip_y_up" : "clip_y_down") + "," +
				   (camera.DepthZeroToOne ? "depth_zero_to_one" : "depth_negative_one_to_one") + "," +
				   (camera.ProjectionIsColumnMajor ? "column_major_projection" : "row_major_projection") +
				   ",metres_per_world_unit=" + std::to_string(camera.MetresPerWorldUnit);
		}

		void CopyCamera(const DataCapturePoll &source, script::DataCaptureBridgePoll &destination) {
			destination.HasCamera = true;
			for (size_t index = 0; index < source.CameraPose.WorldFromCamera.size(); ++index)
				destination.WorldFromCamera[index] = source.CameraPose.WorldFromCamera[index];
			destination.HasProjection = source.CameraPose.ProjectionAvailable;
			if (destination.HasProjection)
				for (size_t index = 0; index < source.CameraPose.Projection.size(); ++index)
					destination.Projection[index] = source.CameraPose.Projection[index];
			destination.VerticalFieldOfViewRadians = source.CameraPose.VerticalFieldOfViewRadians;
			destination.NearMetres = source.CameraPose.NearPlaneMetres;
			destination.FarMetres = source.CameraPose.FarPlaneMetres;
			destination.CropLeft = source.CameraPose.CropLeft;
			destination.CropTop = source.CameraPose.CropTop;
			destination.CropWidth = source.CameraPose.CropWidth;
			destination.CropHeight = source.CameraPose.CropHeight;
			destination.CoordinateConvention = CoordinateConvention(source.Camera);
		}
	}

	ScriptDataCaptureBridge::~ScriptDataCaptureBridge() {
		std::vector<DataCaptureTicket> tickets;
		{
			std::lock_guard lock(Mutex);
			for (auto &entry : OwnerTickets)
				tickets.push_back(std::move(entry.second));
			OwnerTickets.clear();
		}
		for (DataCaptureTicket &ticket : tickets)
			RendererRef.CancelDataCapture(ticket);
	}

	script::DataCaptureBridgeCapabilities ScriptDataCaptureBridge::Capabilities() const {
		std::lock_guard lock(Mutex);
		return {
			.Available = CaptureAvailable,
			.Channels =
				{"rgb_linear_hdr",
				 "linear_depth",
				 "shading_normal",
				 "pbr_albedo",
				 "pbr_material",
				 "pbr_emissive"},
			.Detail = CaptureAvailable ? "requires a declared compatible capture node"
									   : "renderer is not ready for capture"
		};
	}

	void ScriptDataCaptureBridge::RefreshCapabilities() {
		const bool available = RendererRef.Backend().Device != nullptr;
		std::lock_guard lock(Mutex);
		CaptureAvailable = available;
	}

	bool ScriptDataCaptureBridge::Queue(
		std::string_view instanceId,
		const script::DataCaptureBridgeRequest &request,
		uint64_t &ticket,
		std::string &detail
	) {
		std::lock_guard lock(Mutex);
		if (!Valid(instanceId, request)) {
			detail = "invalid capture request";
			return false;
		}
		if (Entries.size() >= LIMIT) {
			detail = "capture queue is full; release a terminal capture";
			return false;
		}
		if (NextTicket == 0) {
			detail = "capture ticket space exhausted";
			return false;
		}
		ticket = NextTicket++;
		Entries.emplace(ticket, Entry{.Request = request, .Reply = {}, .PlaneBytes = {}, .Detail = {}});
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::Poll(
		std::string_view instanceId, uint64_t ticket, script::DataCaptureBridgePoll &poll, std::string &detail
	) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		poll = found->second.Reply;
		if (!found->second.Terminal) {
			poll.Status = "pending";
			poll.SnapshotId = found->second.Request.SnapshotId;
		}
		detail = found->second.Detail;
		return true;
	}

	bool ScriptDataCaptureBridge::ReadPlane(
		std::string_view instanceId,
		uint64_t ticket,
		std::string_view resource,
		size_t offset,
		size_t maximumBytes,
		std::vector<std::byte> &bytes,
		std::string &detail
	) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		if (!found->second.Terminal) {
			detail = "capture is pending";
			return false;
		}
		const auto plane = found->second.PlaneBytes.find(std::string(resource));
		if (plane == found->second.PlaneBytes.end()) {
			detail = "unknown capture resource";
			return false;
		}
		if (offset > plane->second.size()) {
			detail = "capture offset exceeds plane size";
			return false;
		}
		const size_t count = std::min(maximumBytes, plane->second.size() - offset);
		bytes.assign(
			plane->second.begin() + static_cast<std::ptrdiff_t>(offset),
			plane->second.begin() + static_cast<std::ptrdiff_t>(offset + count)
		);
		detail.clear();
		return true;
	}

	bool ScriptDataCaptureBridge::Release(std::string_view instanceId, uint64_t ticket, std::string &detail) {
		std::lock_guard lock(Mutex);
		const auto found = Entries.find(ticket);
		if (found == Entries.end() || found->second.Request.InstanceId != instanceId) {
			detail = "unknown capture ticket";
			return false;
		}
		if (!found->second.Terminal) {
			detail = "capture is pending";
			return false;
		}
		for (const auto &plane : found->second.PlaneBytes)
			RetainedBytes -= plane.second.size();
		Entries.erase(found);
		detail.clear();
		return true;
	}

	void ScriptDataCaptureBridge::Cancel(std::string_view instanceId, uint64_t ticket) {
		std::lock_guard lock(Mutex);
		if (const auto found = Entries.find(ticket); found != Entries.end() &&
													 found->second.Request.InstanceId == instanceId &&
													 !found->second.Terminal)
			found->second.CancelRequested = true;
	}

	void ScriptDataCaptureBridge::PrepareView(View &view) {
		RefreshCapabilities();
		std::vector<PendingRequest> pending;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (OwnerTickets.contains(id) || entry.Preparing || entry.CancelRequested || entry.Terminal ||
					entry.Request.InstanceId != view.WorldName.Text() ||
					entry.Request.Pipeline != view.Pipeline.Text() || entry.Request.ViewSlot != view.Slot)
					continue;
				pending.push_back({.Id = id, .Request = entry.Request});
			}
			std::sort(
				pending.begin(), pending.end(), [](const PendingRequest &left, const PendingRequest &right) {
					return left.Id < right.Id;
				}
			);
			const std::string selectedSnapshot =
				view.SnapshotId.empty() ? (pending.empty() ? "" : pending.front().Request.SnapshotId)
										: view.SnapshotId;
			std::erase_if(pending, [&](const PendingRequest &request) {
				return request.Request.SnapshotId != selectedSnapshot;
			});
			for (const PendingRequest &request : pending)
				Entries.find(request.Id)->second.Preparing = true;
		}

		for (const PendingRequest &pendingRequest : pending) {
			const world::DataFactoryReply barrier = Session.RenderSnapshotBarrier(
				pendingRequest.Request.InstanceId, pendingRequest.Request.SnapshotId
			);
			if (barrier.Status != world::DataFactoryStatus::Ok) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id);
					entry != Entries.end() && !entry->second.CancelRequested) {
					entry->second.Reply.Status = "stale_snapshot";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = barrier.Detail;
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(pendingRequest.Id);
				if (entry == Entries.end() || entry->second.CancelRequested || entry->second.Terminal)
					continue;
			}
			DataCaptureRequest request{
				.SnapshotId = pendingRequest.Request.SnapshotId,
				.Pipeline = view.Pipeline,
				.CaptureNode = core::Name(pendingRequest.Request.CaptureNode),
				.ViewSlot = view.Slot,
				.Channels = {},
			};
			for (const std::string &name : pendingRequest.Request.Channels) {
				const auto channel = Channel(name);
				if (!channel) {
					std::lock_guard lock(Mutex);
					if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
						entry->second.Reply.Status = "invalid";
						entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
						entry->second.Detail = "unknown capture channel";
						entry->second.Terminal = true;
						entry->second.Preparing = false;
					}
					continue;
				}
				request.Channels.push_back(*channel);
			}
			if (request.Channels.size() != pendingRequest.Request.Channels.size()) continue;
			DataCaptureTicket rendererTicket;
			const bool queued = RendererRef.QueueDataCapture(request, rendererTicket);
			bool cancel = false;
			{
				std::lock_guard lock(Mutex);
				auto entry = Entries.find(pendingRequest.Id);
				if (entry == Entries.end() || entry->second.CancelRequested || entry->second.Terminal) {
					cancel = queued;
				} else if (!queued) {
					entry->second.Reply.Status = "failed";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "renderer refused capture";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				} else {
					OwnerTickets.emplace(pendingRequest.Id, std::move(rendererTicket));
					entry->second.Preparing = false;
					view.SnapshotId = request.SnapshotId;
				}
			}
			if (cancel) RendererRef.CancelDataCapture(rendererTicket);
		}
	}

	void ScriptDataCaptureBridge::Pump() {
		RefreshCapabilities();
		std::vector<DataCaptureTicket> cancelled;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (!entry.CancelRequested || entry.Terminal) continue;
				if (auto ticket = OwnerTickets.find(id); ticket != OwnerTickets.end()) {
					cancelled.push_back(std::move(ticket->second));
					OwnerTickets.erase(ticket);
				}
				entry.Reply.Status = "cancelled";
				entry.Reply.SnapshotId = entry.Request.SnapshotId;
				entry.Detail.clear();
				entry.Terminal = true;
				entry.Preparing = false;
			}
		}
		for (DataCaptureTicket &ticket : cancelled)
			RendererRef.CancelDataCapture(ticket);

		std::vector<std::pair<uint64_t, DataCaptureTicket>> polling;
		{
			std::lock_guard lock(Mutex);
			for (auto &entry : OwnerTickets)
				polling.emplace_back(entry.first, std::move(entry.second));
			OwnerTickets.clear();
		}
		for (auto &ticket : polling) {
			DataCapturePoll captured = RendererRef.PollDataCapture(ticket.second);
			if (captured.Status == DataCaptureStatus::Pending) {
				bool cancel = false;
				{
					std::lock_guard lock(Mutex);
					if (const auto entry = Entries.find(ticket.first); entry != Entries.end())
						cancel = entry->second.CancelRequested || entry->second.Terminal;
					if (!cancel) OwnerTickets.emplace(ticket.first, std::move(ticket.second));
				}
				if (cancel) {
					RendererRef.CancelDataCapture(ticket.second);
				}
				continue;
			}
			script::DataCaptureBridgePoll reply;
			reply.Status = Status(captured.Status);
			reply.SnapshotId = captured.SnapshotId;
			reply.CaptureFrame = captured.CaptureFrame;
			if (captured.Status == DataCaptureStatus::Ready || captured.Status == DataCaptureStatus::Partial)
				CopyCamera(captured, reply);
			std::unordered_map<std::string, std::vector<std::byte>> bytes;
			capture_record_validation::State validation;
			bool malformedPlane = false;
			size_t totalBytes = 0;
			for (DataCapturePlane &plane : captured.Planes) {
				if (!capture_record_validation::Plane(ticket.second, plane, ticket.first, validation)) {
					malformedPlane = true;
					break;
				}
				const std::string resource = ResourceId(ticket.first, plane.Channel);
				const std::string channel(DataCaptureChannelName(plane.Channel));
				const std::string source(plane.Resource.Text());
				const std::string hash = plane.Hash.ToHex();
				reply.Planes.push_back(
					{.Channel = channel,
					 .Status = Status(plane.Status),
					 .Resource = resource,
					 .SourceResource = source,
					 .HashAlgorithm = "blake3-256",
					 .Hash = hash,
					 .Width = plane.Width,
					 .Height = plane.Height,
					 .RowStride = plane.RowStride,
					 .Scalar = Scalar(plane.Scalar),
					 .ColourSpace = ColourSpace(plane.ColourSpace),
					 .Origin = "top_left",
					 .Packing = plane.Scalar == DataCaptureScalar::UNorm8	   ? "rgba8_unorm"
								: plane.Scalar == DataCaptureScalar::UNorm10A2 ? "unorm10a2"
																			   : ""}
				);
				if (!plane.Bytes.empty()) {
					if (totalBytes > RETAINED_BYTE_LIMIT ||
						plane.Bytes.size() > RETAINED_BYTE_LIMIT - totalBytes)
						totalBytes = RETAINED_BYTE_LIMIT + 1;
					else
						totalBytes += plane.Bytes.size();
					bytes.emplace(resource, std::move(plane.Bytes));
				}
			}
			const bool coherentStatus =
				(captured.Status == DataCaptureStatus::Ready &&
				 validation.ReadyPlanes == ticket.second.Channels.size() &&
				 validation.Channels.size() == ticket.second.Channels.size()) ||
				(captured.Status == DataCaptureStatus::Partial && validation.ReadyPlanes > 0 &&
				 validation.ReadyPlanes < ticket.second.Channels.size() &&
				 validation.Channels.size() == ticket.second.Channels.size()) ||
				((captured.Status == DataCaptureStatus::Unsupported ||
				  captured.Status == DataCaptureStatus::Invalid ||
				  captured.Status == DataCaptureStatus::Failed ||
				  captured.Status == DataCaptureStatus::Cancelled) &&
				 validation.ReadyPlanes == 0 && validation.Channels.size() == ticket.second.Channels.size());
			malformedPlane = malformedPlane || !coherentStatus;
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(ticket.first);
				if (entry != Entries.end() && !entry->second.Terminal) {
					if (entry->second.CancelRequested) {
						entry->second.Reply.Status = "cancelled";
						entry->second.Reply.SnapshotId = entry->second.Request.SnapshotId;
						entry->second.Detail.clear();
					} else if (captured.SnapshotId != entry->second.Request.SnapshotId) {
						entry->second.Reply.Status = "stale_snapshot";
						entry->second.Reply.SnapshotId = entry->second.Request.SnapshotId;
						entry->second.Detail = "renderer returned a different snapshot";
					} else if (malformedPlane) {
						entry->second.Reply.Status = "failed";
						entry->second.Reply.SnapshotId = captured.SnapshotId;
						entry->second.Detail = "renderer returned a malformed capture plane";
					} else if (totalBytes > RETAINED_BYTE_LIMIT - RetainedBytes) {
						entry->second.Reply.Status = "failed";
						entry->second.Reply.SnapshotId = captured.SnapshotId;
						entry->second.Detail = "capture exceeds retained byte quota";
					} else {
						RetainedBytes += totalBytes;
						entry->second.Reply = std::move(reply);
						entry->second.PlaneBytes = std::move(bytes);
						entry->second.Detail.clear();
					}
					entry->second.Terminal = true;
				}
			}
		}
	}

	bool ScriptDataCaptureBridge::HasPending() const {
		std::lock_guard lock(Mutex);
		return std::any_of(Entries.begin(), Entries.end(), [](const auto &entry) {
			return !entry.second.Terminal;
		});
	}
}
