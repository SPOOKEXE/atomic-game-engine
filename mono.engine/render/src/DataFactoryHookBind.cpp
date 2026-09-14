#include "ViewRecording.hpp"

#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace engine::render {
	RenderObservationContext DataFactoryObservation(
		const ViewRecording &recording, const graph::RunContext &context, core::Name pipeline, size_t viewSlot
	) {
		RenderObservationContext observation;
		observation.Pipeline = pipeline;
		observation.Node = context.Name;
		observation.WorldName = recording.State->ActiveDataCaptureSource.WorldName;
		observation.ViewSlot = viewSlot;
		observation.SnapshotId = recording.State->ActiveDataCaptureSource.SnapshotId;
		observation.Frame = recording.State->FrameCounter;
		observation.PipelineRevision = recording.Pipeline != nullptr ? recording.Pipeline->Revision : 0;
		const glm::mat4 camera = recording.State->ActiveDataCaptureSource.CameraFrame.ToMatrix();
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				observation.Camera.WorldFromCamera[column * 4 + row] = camera[column][row];
		observation.Camera.ProjectionAvailable = recording.State->ActiveDataCaptureSource.ProjectionAvailable;
		observation.Camera.Projection = recording.State->ActiveDataCaptureSource.Projection;
		observation.Camera.FieldOfViewRadians =
			recording.State->ActiveDataCaptureSource.Camera.FieldOfViewRadians;
		observation.Camera.NearPlane = recording.State->ActiveDataCaptureSource.Camera.NearPlane;
		observation.Camera.FarPlane = recording.State->ActiveDataCaptureSource.Camera.FarPlane;
		observation.Camera.Width = recording.State->ActiveDataCaptureSource.Width;
		observation.Camera.Height = recording.State->ActiveDataCaptureSource.Height;
		const auto nameOf = [&recording](graph::ResourceId resource) {
			const graph::ResourceDesc *desc = recording.Pipeline->Graph.FindResource(resource);
			return desc != nullptr ? desc->Name : core::Name{};
		};
		for (graph::ResourceId resource : context.Reads) {
			if (observation.ReadCount == observation.ReadResources.size()) break;
			observation.ReadResources[observation.ReadCount++] = nameOf(resource);
		}
		for (graph::ResourceId resource : context.Writes) {
			if (observation.WriteCount == observation.WriteResources.size()) break;
			observation.WriteResources[observation.WriteCount++] = nameOf(resource);
		}
		return observation;
	}

	namespace {
		bool SameSpec(const RenderHookSpec &left, const RenderHookSpec &right) {
			return left.Name == right.Name && left.Kind == right.Kind && left.NodeKind == right.NodeKind &&
				   left.SchemaVersion == right.SchemaVersion && left.Required == right.Required &&
				   left.ChannelCount == right.ChannelCount && left.Channels == right.Channels;
		}

		bool ValidSpec(const RenderHookSpec &spec) {
			if (!spec.Name.IsValid() || spec.Name.Text().empty() || spec.Name.Text().size() > 256 ||
				!spec.NodeKind.IsValid() || spec.NodeKind.Text().size() > 256 ||
				spec.Kind != RenderHookKind::DataCapture || !spec.Required || spec.SchemaVersion == 0 ||
				spec.ChannelCount == 0 || spec.ChannelCount > spec.Channels.size())
				return false;
			for (size_t index = 0; index < spec.ChannelCount; ++index) {
				if (static_cast<size_t>(spec.Channels[index]) >
					static_cast<size_t>(DataCaptureChannel::OpticalFlow))
					return false;
				for (size_t previous = 0; previous < index; ++previous)
					if (spec.Channels[previous] == spec.Channels[index]) return false;
			}
			return true;
		}

		uint32_t NextGeneration(uint32_t generation) {
			return generation == UINT32_MAX ? 0 : generation + 1;
		}

	}

	struct DataFactoryHookBind::Impl {
		explicit Impl(Renderer &renderer) : RendererRef(renderer) {}
		Renderer &RendererRef;
		struct HookSlot {
			RenderHookSpec Spec;
			uint32_t Generation = 1;
			bool Used = false;
		};
		struct ConnectionSlot {
			HookConnectionRequest Request;
			std::array<HookHandle, MAX_DATA_FACTORY_HOOKS> Hooks{};
			size_t HookCount = 0;
			uint32_t Generation = 1;
			bool Used = false;
		};
		struct BatchSlot {
			ConnectionHandle Connection;
			DataCaptureRequest Request;
			DataCaptureTicket Ticket;
			RenderObservationContext Observation;
			HookBundle Bundle;
			uint32_t Generation = 1;
			bool Used = false;
			bool Submitted = false;
			bool Ready = false;
			bool Observed = false;
			uint16_t PollAttempts = 0;
			size_t AccountedBytes = 0;
		};
		std::array<HookSlot, MAX_DATA_FACTORY_HOOKS> Hooks{};
		std::array<ConnectionSlot, MAX_DATA_FACTORY_CONNECTIONS> Connections{};
		std::array<BatchSlot, MAX_DATA_FACTORY_BATCHES> Batches{};
		size_t ReadyBytes = 0;

		bool Valid(HookHandle handle) const {
			return handle.Slot < Hooks.size() && Hooks[handle.Slot].Used &&
				   Hooks[handle.Slot].Generation == handle.Generation;
		}
		bool Valid(ConnectionHandle handle) const {
			return handle.Slot < Connections.size() && Connections[handle.Slot].Used &&
				   Connections[handle.Slot].Generation == handle.Generation;
		}
		bool Valid(BatchHandle handle) const {
			return handle.Slot < Batches.size() && Batches[handle.Slot].Used &&
				   Batches[handle.Slot].Generation == handle.Generation;
		}
		void Release(BatchSlot &batch) {
			if (batch.Submitted) RendererRef.CancelDataCapture(batch.Ticket);
			ReadyBytes -= batch.AccountedBytes;
			const uint32_t generation = NextGeneration(batch.Generation);
			batch = {};
			batch.Generation = generation;
		}
		void Fail(BatchSlot &batch, DataCaptureStatus status) {
			if (batch.Submitted) RendererRef.CancelDataCapture(batch.Ticket);
			batch.Submitted = false;
			batch.Bundle.Capture = {};
			batch.Bundle.Capture.Status = status;
			batch.Bundle.Capture.SnapshotId = batch.Request.SnapshotId;
			batch.Bundle.Capture.Pipeline = batch.Observation.Pipeline;
			batch.Bundle.Capture.PipelineRevision = batch.Observation.PipelineRevision;
			batch.Bundle.Capture.WorldName = batch.Observation.WorldName;
			batch.Bundle.Capture.ViewSlot = batch.Observation.ViewSlot;
			for (const DataCaptureChannel channel : batch.Request.Channels) {
				DataCapturePlane plane;
				plane.Channel = channel;
				plane.Status = status;
				plane.CaptureNode = batch.Request.CaptureNode;
				batch.Bundle.Capture.Planes.push_back(std::move(plane));
			}
			batch.Ready = true;
		}
	};

	DataFactoryHookBind::DataFactoryHookBind(Renderer &renderer) : State(std::make_unique<Impl>(renderer)) {}
	DataFactoryHookBind::~DataFactoryHookBind() {
		for (auto &batch : State->Batches)
			if (batch.Used) State->Release(batch);
	}

	HookHandle DataFactoryHookBind::RegisterHook(const RenderHookSpec &spec) {
		if (!ValidSpec(spec)) return {};
		for (size_t index = 0; index < State->Hooks.size(); ++index) {
			auto &slot = State->Hooks[index];
			if (!slot.Used || slot.Spec.Name != spec.Name) continue;
			return SameSpec(slot.Spec, spec) ? HookHandle{static_cast<uint16_t>(index), slot.Generation}
											 : HookHandle{};
		}
		for (size_t index = 0; index < State->Hooks.size(); ++index) {
			auto &slot = State->Hooks[index];
			if (slot.Used || slot.Generation == 0) continue;
			slot.Spec = spec;
			slot.Used = true;
			return {static_cast<uint16_t>(index), slot.Generation};
		}
		return {};
	}

	HookHandle DataFactoryHookBind::FindHook(core::Name name) const {
		if (!name.IsValid()) return {};
		for (size_t index = 0; index < State->Hooks.size(); ++index) {
			const auto &slot = State->Hooks[index];
			if (slot.Used && slot.Spec.Name == name) return {static_cast<uint16_t>(index), slot.Generation};
		}
		return {};
	}

	std::vector<RenderHookCapability> DataFactoryHookBind::DescribeHooks() const {
		std::vector<RenderHookCapability> result;
		result.reserve(State->Hooks.size());
		for (const auto &slot : State->Hooks)
			if (slot.Used) result.push_back(slot.Spec);
		return result;
	}

	ConnectHooksResult DataFactoryHookBind::ConnectHooks(
		const HookConnectionRequest &request, std::span<const HookHandle> hooks
	) {
		if (request.Session.WorldName.empty() || request.Session.WorldName.size() > 256 ||
			request.Session.WorldName.find('\0') != std::string::npos || !request.Pipeline.IsValid() ||
			!request.CaptureNode.IsValid() || request.PipelineRevision == 0 || request.ViewWidth == 0 ||
			request.ViewHeight == 0 || hooks.empty() || hooks.size() > State->Hooks.size())
			return {};
		const auto pipeline =
			State->RendererRef.DescribePipeline(request.Pipeline, request.ViewWidth, request.ViewHeight);
		if (!pipeline || pipeline->Revision != request.PipelineRevision)
			return {.Status = HookBindStatus::Stale, .Connection = {}};
		for (size_t index = 0; index < hooks.size(); ++index) {
			if (!State->Valid(hooks[index]))
				return {.Status = HookBindStatus::UnknownHandle, .Connection = {}};
			for (size_t previous = 0; previous < index; ++previous)
				if (hooks[previous].Slot == hooks[index].Slot)
					return {.Status = HookBindStatus::Conflict, .Connection = {}};
			const auto &spec = State->Hooks[hooks[index].Slot].Spec;
			for (size_t channel = 0; channel < spec.ChannelCount; ++channel) {
				for (size_t previous = 0; previous < index; ++previous) {
					const auto &previousSpec = State->Hooks[hooks[previous].Slot].Spec;
					if (std::find(
							previousSpec.Channels.begin(),
							previousSpec.Channels.begin() + previousSpec.ChannelCount,
							spec.Channels[channel]
						) != previousSpec.Channels.begin() + previousSpec.ChannelCount)
						return {.Status = HookBindStatus::Conflict, .Connection = {}};
				}
				const auto derived = DataCaptureNode(request.CaptureNode, spec.Channels[channel]);
				bool found = false;
				for (uint32_t node = 1; node <= pipeline->Graph.Count(); ++node) {
					const auto *candidate = pipeline->Graph.Find(graph::NodeId{node});
					found = found || (candidate && candidate->Enabled && candidate->Kind == spec.NodeKind &&
									  candidate->Name == derived);
				}
				if (!found) return {.Status = HookBindStatus::Invalid, .Connection = {}};
			}
		}
		for (size_t index = 0; index < State->Connections.size(); ++index) {
			auto &slot = State->Connections[index];
			if (slot.Used || slot.Generation == 0) continue;
			slot.Request = request;
			std::copy(hooks.begin(), hooks.end(), slot.Hooks.begin());
			slot.HookCount = hooks.size();
			slot.Used = true;
			return {
				.Status = HookBindStatus::Ok, .Connection = {static_cast<uint16_t>(index), slot.Generation}
			};
		}
		return {.Status = HookBindStatus::Capacity, .Connection = {}};
	}

	void DataFactoryHookBind::DisconnectHooks(std::span<const ConnectionHandle> connections) {
		for (const ConnectionHandle connection : connections) {
			if (!State->Valid(connection)) continue;
			for (auto &batch : State->Batches)
				if (batch.Used && batch.Connection.Slot == connection.Slot &&
					batch.Connection.Generation == connection.Generation)
					State->Release(batch);
			auto &slot = State->Connections[connection.Slot];
			const uint32_t generation = NextGeneration(slot.Generation);
			slot = {};
			slot.Generation = generation;
		}
	}

	std::optional<BatchHandle>
	DataFactoryHookBind::ArmDataCapture(ConnectionHandle connection, DataCaptureRequest request) {
		if (!State->Valid(connection)) return {};
		const auto &owner = State->Connections[connection.Slot];
		if (request.SnapshotId.empty() || request.Pipeline != owner.Request.Pipeline ||
			request.CaptureNode != owner.Request.CaptureNode || request.ViewSlot != owner.Request.ViewSlot ||
			request.Channels.empty() || request.Channels.size() != owner.HookCount)
			return {};
		for (const DataCaptureChannel channel : request.Channels) {
			bool supported = false;
			for (size_t index = 0; index < owner.HookCount; ++index) {
				const auto &spec = State->Hooks[owner.Hooks[index].Slot].Spec;
				supported =
					supported ||
					std::find(spec.Channels.begin(), spec.Channels.begin() + spec.ChannelCount, channel) !=
						spec.Channels.begin() + spec.ChannelCount;
			}
			if (!supported) return {};
		}
		for (size_t index = 0; index < owner.HookCount; ++index) {
			const auto &spec = State->Hooks[owner.Hooks[index].Slot].Spec;
			for (size_t channel = 0; channel < spec.ChannelCount; ++channel)
				if (std::find(request.Channels.begin(), request.Channels.end(), spec.Channels[channel]) ==
					request.Channels.end())
					return {};
		}
		for (size_t index = 0; index < State->Batches.size(); ++index) {
			auto &slot = State->Batches[index];
			if (slot.Used || slot.Generation == 0) continue;
			slot.Connection = connection;
			slot.Request = std::move(request);
			slot.Used = true;
			return BatchHandle{static_cast<uint16_t>(index), slot.Generation};
		}
		return {};
	}

	CallHooksResult DataFactoryHookBind::CallHooks(
		ConnectionHandle connection, BatchHandle batch, const RenderObservationContext &context
	) {
		if (!State->Valid(connection) || !State->Valid(batch))
			return {.Status = HookBindStatus::Stale, .Batch = {}};
		auto &pending = State->Batches[batch.Slot];
		const auto current = State->RendererRef.DescribePipeline(
			State->Connections[connection.Slot].Request.Pipeline,
			State->Connections[connection.Slot].Request.ViewWidth,
			State->Connections[connection.Slot].Request.ViewHeight
		);
		if (pending.Connection.Slot != connection.Slot ||
			pending.Connection.Generation != connection.Generation || pending.Submitted || pending.Ready ||
			context.WorldName.Text() != State->Connections[connection.Slot].Request.Session.WorldName ||
			context.SnapshotId != pending.Request.SnapshotId ||
			context.Pipeline != pending.Request.Pipeline || context.ViewSlot != pending.Request.ViewSlot ||
			!current || current->Revision != State->Connections[connection.Slot].Request.PipelineRevision ||
			context.PipelineRevision != current->Revision)
			return {.Status = HookBindStatus::Invalid, .Batch = {}};
		if (!State->RendererRef.QueueDataCapture(pending.Request, pending.Ticket))
			return {.Status = HookBindStatus::Backpressured, .Batch = {}};
		pending.Observation = context;
		pending.Submitted = true;
		return {.Status = HookBindStatus::Ok, .Batch = batch};
	}

	void DataFactoryHookBind::Observe(const RenderObservationContext &context) {
		for (auto &batch : State->Batches) {
			if (!batch.Used || !batch.Submitted || batch.Ready || !State->Valid(batch.Connection)) continue;
			const auto &connection = State->Connections[batch.Connection.Slot];
			if (!batch.Observed && connection.Request.PipelineRevision == context.PipelineRevision &&
				connection.Request.Session.WorldName == context.WorldName.Text() &&
				batch.Request.SnapshotId == context.SnapshotId &&
				batch.Request.Pipeline == context.Pipeline && batch.Request.ViewSlot == context.ViewSlot) {
				bool expectedNode = false;
				for (const DataCaptureChannel channel : batch.Request.Channels) {
					const core::Name node = DataCaptureNode(batch.Request.CaptureNode, channel);
					expectedNode = expectedNode || context.Node == node;
				}
				if (expectedNode) {
					batch.Observation = context;
					batch.Observed = true;
				}
			}
		}
	}

	void DataFactoryHookBind::Cancel(BatchHandle batch) {
		if (State->Valid(batch)) State->Release(State->Batches[batch.Slot]);
	}

	void DataFactoryHookBind::Shutdown() {
		for (auto &batch : State->Batches)
			if (batch.Used && !batch.Ready) State->Fail(batch, DataCaptureStatus::Cancelled);
	}

	void DataFactoryHookBind::Pump() {
		for (auto &batch : State->Batches) {
			if (!batch.Used || !batch.Submitted || batch.Ready) continue;
			DataCapturePoll poll = State->RendererRef.PollDataCapture(batch.Ticket);
			if (poll.Status == DataCaptureStatus::Pending) {
				if (++batch.PollAttempts >= MAX_DATA_FACTORY_PENDING_PUMPS)
					State->Fail(batch, DataCaptureStatus::Failed);
				continue;
			}
			if (poll.Status == DataCaptureStatus::Partial) {
				State->Fail(batch, DataCaptureStatus::Failed);
				continue;
			}
			if (poll.SnapshotId != batch.Observation.SnapshotId ||
				poll.CaptureFrame != batch.Observation.Frame || poll.Pipeline != batch.Observation.Pipeline ||
				poll.PipelineRevision != batch.Observation.PipelineRevision ||
				poll.WorldName != batch.Observation.WorldName ||
				poll.ViewSlot != batch.Observation.ViewSlot) {
				State->Fail(batch, DataCaptureStatus::Invalid);
				continue;
			}
			size_t bytes = 0;
			for (const auto &plane : poll.Planes)
				bytes += plane.Bytes.size();
			if (bytes > MAX_DATA_FACTORY_RETAINED_BYTES - State->ReadyBytes) {
				State->Fail(batch, DataCaptureStatus::Failed);
				continue;
			}
			State->ReadyBytes += bytes;
			batch.AccountedBytes = bytes;
			batch.Bundle.Capture = std::move(poll);
			batch.Ready = true;
		}
	}

	std::optional<HookBundle> DataFactoryHookBind::TakeCompleted(BatchHandle batch) {
		if (!State->Valid(batch)) return {};
		auto &slot = State->Batches[batch.Slot];
		if (!slot.Ready) return {};
		HookBundle result = std::move(slot.Bundle);
		State->Release(slot);
		return result;
	}

	void DataFactoryHookBind::RegisterBuiltInDataCaptureHooks() {
		for (size_t index = 0; index <= static_cast<size_t>(DataCaptureChannel::SecondSurfaceValidity);
			 ++index) {
			const auto channel = static_cast<DataCaptureChannel>(index);
			const std::string name = "data_capture." + std::string(DataCaptureChannelName(channel));
			(void)RegisterHook({
				.Name = core::Name(name),
				.Kind = RenderHookKind::DataCapture,
				.NodeKind = core::Name("capture"),
				.SchemaVersion = 1,
				.Required = true,
				.Channels = {channel},
				.ChannelCount = 1,
			});
		}
	}
}
