#include "CaptureRecordValidation.hpp"

#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/render/DataFactoryHookBind.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace engine::render {
	struct ScriptDataCaptureBridge::HookState {
		std::unordered_map<uint64_t, ConnectionHandle> Connections;
		std::unordered_map<uint64_t, BatchHandle> Batches;
	};

	ScriptDataCaptureBridge::ScriptDataCaptureBridge(world::DataFactorySession &session, Renderer &renderer)
		: Session(session), RendererRef(renderer) {
		RefreshCapabilities();
	}

	namespace {
		constexpr size_t MAX_CAPTURE_CHANNELS = 12;
		constexpr size_t MAX_CAPTURE_TICKETS = 6;
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
			if (name == "ambient_occlusion") return DataCaptureChannel::AmbientOcclusion;
			if (name == "object_ids") return DataCaptureChannel::ObjectIds;
			if (name == "semantic_ids") return DataCaptureChannel::SemanticMask;
			if (name == "part_ids") return DataCaptureChannel::PartMask;
			if (name == "second_surface_depth") return DataCaptureChannel::SecondSurfaceDepth;
			if (name == "second_surface_validity") return DataCaptureChannel::SecondSurfaceValidity;
			return std::nullopt;
		}

		bool Valid(std::string_view instanceId, const script::DataCaptureBridgeRequest &request) {
			if (!Text(instanceId) || request.InstanceId != instanceId || !Text(request.SnapshotId) ||
				!Text(request.Pipeline) || !Text(request.CaptureNode) || request.Channels.empty() ||
				request.Channels.size() > MAX_CAPTURE_CHANNELS || request.TemporalHistory != "preserve" ||
				request.ViewSlot > std::numeric_limits<size_t>::max())
				return false;
			for (size_t first = 0; first < request.Channels.size(); ++first) {
				if (!Text(request.Channels[first], 64) || !Channel(request.Channels[first])) return false;
				for (size_t second = first + 1; second < request.Channels.size(); ++second)
					if (request.Channels[first] == request.Channels[second]) return false;
			}
			const bool secondDepth =
				std::ranges::find(request.Channels, "second_surface_depth") != request.Channels.end();
			const bool secondValidity =
				std::ranges::find(request.Channels, "second_surface_validity") != request.Channels.end();
			return secondDepth == secondValidity;
		}

		bool PipelineMatches(std::string_view requested, const View &view) {
			const std::string_view runtime = view.Pipeline.Text();
			if (requested == runtime) return true;
			const std::string suffix = "#" + std::to_string(view.World);
			return runtime.ends_with(suffix) &&
				   requested == runtime.substr(0, runtime.size() - suffix.size());
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
			case DataCaptureScalar::UInt32:
				return "uint32";
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

		const char *Provenance(DataCaptureChannel channel) {
			return channel == DataCaptureChannel::AmbientOcclusion
					   ? "ssao_estimator_visibility_factor_not_ground_truth"
					   : "";
		}

		const char *SourceState(AmbientOcclusionSourceState state) {
			switch (state) {
			case AmbientOcclusionSourceState::Estimated:
				return "estimated";
			case AmbientOcclusionSourceState::ClearedDisabled:
				return "cleared_disabled";
			case AmbientOcclusionSourceState::ClearedNoPass:
				return "cleared_no_pass";
			case AmbientOcclusionSourceState::Unavailable:
				return "unavailable";
			}
			return "unavailable";
		}

		std::optional<script::DataCaptureBridgeAmbientOcclusion>
		CopyAmbientOcclusion(const std::optional<AmbientOcclusionProvenance> &source) {
			if (!source) return std::nullopt;
			const auto denoiser = source->Denoiser == std::optional(AmbientOcclusionDenoiser::None)
									  ? std::optional<std::string>("none")
									  : std::nullopt;
			const auto temporalHistory =
				source->TemporalHistory == std::optional(AmbientOcclusionTemporalHistory::Disabled)
					? std::optional<std::string>("none")
					: std::nullopt;
			const auto backgroundClassification =
				source->BackgroundClassification ? std::optional<std::string>("unavailable") : std::nullopt;
			return script::DataCaptureBridgeAmbientOcclusion{
				.SourceState = SourceState(source->SourceState),
				.ProducerFrame = source->ProducerFrame,
				.Enabled = source->Enabled,
				.SampleCount = source->SampleCount,
				.RadiusWorldUnits = source->RadiusWorldUnits
										? std::optional<double>(*source->RadiusWorldUnits)
										: std::nullopt,
				.Denoiser = std::move(denoiser),
				.TemporalHistory = std::move(temporalHistory),
				.BackgroundValue =
					source->BackgroundValue ? std::optional<double>(*source->BackgroundValue) : std::nullopt,
				.BackgroundClassification = std::move(backgroundClassification)
			};
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
		if (!Hooks) return;
		std::vector<ConnectionHandle> connections;
		for (const auto &[id, connection] : Hooks->Connections) connections.push_back(connection);
		RendererRef.Hooks().DisconnectHooks(connections);
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
				 "pbr_emissive",
				 "ambient_occlusion",
				 "object_ids",
				 "semantic_ids",
				 "part_ids",
				 "second_surface_depth",
				 "second_surface_validity"},
			.HookRecords = HookCapabilities,
			.MaximumHooks = static_cast<uint32_t>(MAX_DATA_FACTORY_HOOKS),
			.MaximumConnections = static_cast<uint32_t>(MAX_DATA_FACTORY_CONNECTIONS),
			.MaximumBatches = static_cast<uint32_t>(MAX_DATA_FACTORY_BATCHES),
			.MaximumReadbackNodes = static_cast<uint32_t>(MAX_DATA_FACTORY_READBACK_NODES),
			.MaximumRetainedBytes = MAX_DATA_FACTORY_RETAINED_BYTES,
			.MaximumPendingPumps = MAX_DATA_FACTORY_PENDING_PUMPS,
			.Detail = CaptureAvailable ? "requires a declared compatible capture node"
									   : "renderer is not ready for capture"
		};
	}

	void ScriptDataCaptureBridge::RefreshCapabilities() {
		const bool available = RendererRef.Backend().Device != nullptr;
		std::vector<script::DataCaptureBridgeHookCapability> hooks;
		for (const RenderHookCapability &hook : RendererRef.Hooks().DescribeHooks()) {
			if (hook.Kind != RenderHookKind::DataCapture) continue;
			script::DataCaptureBridgeHookCapability record;
			record.Name = hook.Name.Text();
			record.SchemaVersion = hook.SchemaVersion;
			record.NodeKind = hook.NodeKind.Text();
			record.Required = hook.Required;
			record.Channels.reserve(hook.ChannelCount);
			for (size_t index = 0; index < hook.ChannelCount; ++index)
				record.Channels.emplace_back(DataCaptureChannelName(hook.Channels[index]));
			hooks.push_back(std::move(record));
		}
		std::lock_guard lock(Mutex);
		CaptureAvailable = available;
		HookCapabilities = std::move(hooks);
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
		if (Entries.size() >= MAX_CAPTURE_TICKETS) {
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

	bool ScriptDataCaptureBridge::TeardownInstance(std::string_view instanceId, std::string &detail) {
		if (!Text(instanceId)) {
			detail = "invalid capture instance";
			return false;
		}
		std::vector<ConnectionHandle> connections;
		{
			std::lock_guard lock(Mutex);
			for (auto entry = Entries.begin(); entry != Entries.end();) {
				if (entry->second.Request.InstanceId != instanceId) {
					++entry;
					continue;
				}
				if (Hooks) {
					if (auto owner = Hooks->Connections.find(entry->first); owner != Hooks->Connections.end()) {
						connections.push_back(owner->second);
						Hooks->Connections.erase(owner);
					}
					Hooks->Batches.erase(entry->first);
				}
				for (const auto &[resource, bytes] : entry->second.PlaneBytes)
					RetainedBytes -= bytes.size();
				entry = Entries.erase(entry);
			}
		}
		RendererRef.Hooks().DisconnectHooks(connections);
		detail.clear();
		return true;
	}

	void ScriptDataCaptureBridge::PrepareView(View &view) {
		RefreshCapabilities();
		if (!Hooks) Hooks = std::make_unique<HookState>();
		std::vector<PendingRequest> pending;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (Hooks->Batches.contains(id) || entry.Preparing || entry.CancelRequested || entry.Terminal ||
					entry.Request.InstanceId != view.WorldName.Text() ||
					!PipelineMatches(entry.Request.Pipeline, view) || entry.Request.ViewSlot != view.Slot)
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
			bool releaseHook = false;
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
				.ObjectLabels = {view.ObjectLabels.begin(), view.ObjectLabels.end()},
				.SemanticLabels = {view.SemanticLabels.begin(), view.SemanticLabels.end()},
				.PartLabels = {view.PartLabels.begin(), view.PartLabels.end()},
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
			const bool wantsObjectIds =
				std::ranges::find(request.Channels, DataCaptureChannel::ObjectIds) != request.Channels.end();
			const bool wantsSemantic =
				std::ranges::find(request.Channels, DataCaptureChannel::SemanticMask) !=
				request.Channels.end();
			const bool wantsPart =
				std::ranges::find(request.Channels, DataCaptureChannel::PartMask) != request.Channels.end();
			if (wantsObjectIds && !view.ObjectLabelsValid) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
					entry->second.Reply.Status = "invalid";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "object ids require unique bounded DataFactoryId values";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			if ((wantsSemantic && !view.SemanticLabelsValid) || (wantsPart && !view.PartLabelsValid)) {
				std::lock_guard lock(Mutex);
				if (auto entry = Entries.find(pendingRequest.Id); entry != Entries.end()) {
					entry->second.Reply.Status = "invalid";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "semantic or part ids require unique bounded authored labels";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				}
				continue;
			}
			std::array<HookHandle, MAX_CAPTURE_CHANNELS> hookHandles{};
			for (size_t index = 0; index < request.Channels.size(); ++index)
				hookHandles[index] = RendererRef.Hooks().FindHook(
					core::Name("data_capture." + std::string(DataCaptureChannelName(request.Channels[index])))
				);
			const auto described = RendererRef.DescribePipeline(
				view.Pipeline, view.Target != nullptr ? view.Target->Width : 1, view.Target != nullptr ? view.Target->Height : 1
			);
			const ConnectHooksResult connection = described ? RendererRef.Hooks().ConnectHooks(
				{.Session = {.WorldName = pendingRequest.Request.InstanceId},
				 .Pipeline = view.Pipeline,
				 .PipelineRevision = described->Revision,
				 .ViewSlot = view.Slot,
				 .CaptureNode = request.CaptureNode,
				 .ViewWidth = view.Target != nullptr ? view.Target->Width : 1,
				 .ViewHeight = view.Target != nullptr ? view.Target->Height : 1},
				std::span(hookHandles).first(request.Channels.size())
			) : ConnectHooksResult{.Status = HookBindStatus::Stale, .Connection = {}};
			const auto batch = connection.Status == HookBindStatus::Ok
				? RendererRef.Hooks().ArmDataCapture(connection.Connection, request)
				: std::optional<BatchHandle>{};
			const RenderObservationContext observation{
				.Pipeline = view.Pipeline,
				.Node = request.CaptureNode,
				.WorldName = view.WorldName,
				.ViewSlot = view.Slot,
				.SnapshotId = request.SnapshotId,
				.PipelineRevision = described ? described->Revision : 0,
				.Camera = {},
			};
			const CallHooksResult queued = batch
				? RendererRef.Hooks().CallHooks(connection.Connection, *batch, observation)
				: CallHooksResult{.Status = HookBindStatus::Backpressured, .Batch = {}};
			{
				std::lock_guard lock(Mutex);
				auto entry = Entries.find(pendingRequest.Id);
				if (entry == Entries.end() || entry->second.CancelRequested || entry->second.Terminal) {
					releaseHook = true;
				} else if (queued.Status != HookBindStatus::Ok) {
					releaseHook = true;
					entry->second.Reply.Status = "failed";
					entry->second.Reply.SnapshotId = pendingRequest.Request.SnapshotId;
					entry->second.Detail = "renderer capture hook backpressured";
					entry->second.Terminal = true;
					entry->second.Preparing = false;
				} else {
					Hooks->Connections.emplace(pendingRequest.Id, connection.Connection);
					Hooks->Batches.emplace(pendingRequest.Id, *batch);
					entry->second.Preparing = false;
					view.SnapshotId = request.SnapshotId;
				}
			}
			if (releaseHook) {
				if (batch) RendererRef.Hooks().Cancel(*batch);
				if (connection.Status == HookBindStatus::Ok)
					RendererRef.Hooks().DisconnectHooks(std::array{connection.Connection});
			}
		}
	}

	void ScriptDataCaptureBridge::Pump() {
		RefreshCapabilities();
		std::vector<ConnectionHandle> cancelled;
		{
			std::lock_guard lock(Mutex);
			for (auto &[id, entry] : Entries) {
				if (!entry.CancelRequested || entry.Terminal) continue;
				if (Hooks) {
					if (auto connection = Hooks->Connections.find(id); connection != Hooks->Connections.end()) {
						cancelled.push_back(connection->second);
						Hooks->Connections.erase(connection);
						Hooks->Batches.erase(id);
					}
				}
				entry.Reply.Status = "cancelled";
				entry.Reply.SnapshotId = entry.Request.SnapshotId;
				entry.Detail.clear();
				entry.Terminal = true;
				entry.Preparing = false;
			}
		}
		if (Hooks) RendererRef.Hooks().DisconnectHooks(cancelled);
		if (!Hooks) return;
		RendererRef.Hooks().Pump();

		std::vector<std::pair<uint64_t, BatchHandle>> polling;
		std::vector<ConnectionHandle> completedConnections;
		{
			std::lock_guard lock(Mutex);
			for (const auto &entry : Hooks->Batches) polling.emplace_back(entry.first, entry.second);
		}
		for (const auto &ticket : polling) {
			auto bundle = RendererRef.Hooks().TakeCompleted(ticket.second);
			if (!bundle) continue;
			DataCapturePoll captured = std::move(bundle->Capture);
			DataCaptureTicket validationTicket;
			{
				std::lock_guard lock(Mutex);
				const auto entry = Entries.find(ticket.first);
				if (entry == Entries.end()) continue;
				validationTicket.SnapshotId = entry->second.Request.SnapshotId;
				validationTicket.CaptureNode = core::Name(entry->second.Request.CaptureNode);
				for (const std::string &name : entry->second.Request.Channels)
					if (const auto channel = Channel(name)) validationTicket.Channels.push_back(*channel);
				Hooks->Batches.erase(ticket.first);
				if (const auto connection = Hooks->Connections.find(ticket.first);
					connection != Hooks->Connections.end()) {
					completedConnections.push_back(connection->second);
					Hooks->Connections.erase(connection);
				}
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
			bool objectIdsReady = false;
			bool semanticIdsReady = false;
			bool partIdsReady = false;
			size_t totalBytes = 0;
			for (DataCapturePlane &plane : captured.Planes) {
				if (!capture_record_validation::Plane(validationTicket, plane, ticket.first, validation)) {
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
					 .ByteSize = plane.Bytes.size(),
					 .Scalar = Scalar(plane.Scalar),
					 .ColourSpace = ColourSpace(plane.ColourSpace),
					 .Origin = "top_left",
					 .Packing = plane.Channel == DataCaptureChannel::AmbientOcclusion ||
										plane.Channel == DataCaptureChannel::SecondSurfaceValidity
									? "unorm8"
								: plane.Scalar == DataCaptureScalar::UNorm8	   ? "rgba8_unorm"
								: plane.Scalar == DataCaptureScalar::UNorm10A2 ? "unorm10a2"
																			   : "",
					 .Provenance = plane.Provenance.empty() ? Provenance(plane.Channel) : plane.Provenance,
					 .AmbientOcclusion = CopyAmbientOcclusion(plane.AmbientOcclusion)}
				);
				if (!plane.Bytes.empty()) {
					if (totalBytes > RETAINED_BYTE_LIMIT ||
						plane.Bytes.size() > RETAINED_BYTE_LIMIT - totalBytes)
						totalBytes = RETAINED_BYTE_LIMIT + 1;
					else
						totalBytes += plane.Bytes.size();
					bytes.emplace(resource, std::move(plane.Bytes));
				}
				if (plane.Status == DataCaptureStatus::Ready) {
					objectIdsReady = objectIdsReady || plane.Channel == DataCaptureChannel::ObjectIds;
					semanticIdsReady = semanticIdsReady || plane.Channel == DataCaptureChannel::SemanticMask;
					partIdsReady = partIdsReady || plane.Channel == DataCaptureChannel::PartMask;
				}
			}
			if (objectIdsReady)
				for (const DataCaptureObjectLabel &label : captured.ObjectLabels)
					reply.ObjectLabels.push_back({label.Label, label.StableId});
			if (semanticIdsReady)
				for (const DataCaptureSemanticLabel &label : captured.SemanticLabels)
					reply.SemanticLabels.push_back({label.Label, label.StableId});
			if (partIdsReady)
				for (const DataCapturePartLabel &label : captured.PartLabels)
					reply.PartLabels.push_back({label.Label, label.StableId});
			const bool coherentStatus =
				(captured.Status == DataCaptureStatus::Ready &&
					 validation.ReadyPlanes == validationTicket.Channels.size() &&
					 validation.Channels.size() == validationTicket.Channels.size()) ||
				(captured.Status == DataCaptureStatus::Partial && validation.ReadyPlanes > 0 &&
					 validation.ReadyPlanes < validationTicket.Channels.size() &&
					 validation.Channels.size() == validationTicket.Channels.size()) ||
				((captured.Status == DataCaptureStatus::Unsupported ||
				  captured.Status == DataCaptureStatus::Invalid ||
				  captured.Status == DataCaptureStatus::Failed ||
				  captured.Status == DataCaptureStatus::Cancelled) &&
				 validation.ReadyPlanes == 0 && validation.Channels.size() == validationTicket.Channels.size());
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
		RendererRef.Hooks().DisconnectHooks(completedConnections);
	}

	bool ScriptDataCaptureBridge::HasPending() const {
		std::lock_guard lock(Mutex);
		return std::any_of(Entries.begin(), Entries.end(), [](const auto &entry) {
			return !entry.second.Terminal;
		});
	}
}
