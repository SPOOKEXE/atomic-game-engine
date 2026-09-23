#include "ViewRecording.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>

namespace engine::render {
	RenderObservationContext DataFactoryObservation(
		const ViewRecording &recording,
		const graph::RunContext &context,
		core::Name pipeline,
		size_t viewSlot,
		const DataCaptureSource &captureSource
	) {
		RenderObservationContext observation;
		observation.Pipeline = pipeline;
		observation.Node = context.Name;
		observation.WorldName = captureSource.WorldName;
		observation.ViewSlot = viewSlot;
		observation.SnapshotId = captureSource.SnapshotId;
		observation.Frame = recording.State->FrameCounter;
		observation.PipelineRevision = recording.Pipeline != nullptr ? recording.Pipeline->Revision : 0;
		const glm::mat4 camera = captureSource.CameraFrame.ToMatrix();
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				observation.Camera.WorldFromCamera[column * 4 + row] = camera[column][row];
		observation.Camera.ProjectionAvailable = captureSource.ProjectionAvailable;
		observation.Camera.Projection = captureSource.Projection;
		observation.Camera.FieldOfViewRadians = captureSource.Camera.FieldOfViewRadians;
		observation.Camera.NearPlane = captureSource.Camera.NearPlane;
		observation.Camera.FarPlane = captureSource.Camera.FarPlane;
		observation.Camera.Width = captureSource.Width;
		observation.Camera.Height = captureSource.Height;
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
			return left.Name == right.Name && left.Kind == right.Kind && left.Access == right.Access &&
				   left.NodeKind == right.NodeKind && left.SchemaVersion == right.SchemaVersion &&
				   left.Required == right.Required && left.ChannelCount == right.ChannelCount &&
				   left.Channels == right.Channels && left.MutatedFieldCount == right.MutatedFieldCount &&
				   left.MutatedFields == right.MutatedFields;
		}

		bool ValidSpec(const RenderHookSpec &spec) {
			if (!spec.Name.IsValid() || spec.Name.Text().empty() || spec.Name.Text().size() > 256 ||
				!spec.NodeKind.IsValid() || spec.NodeKind.Text().size() > 256 || !spec.Required ||
				spec.SchemaVersion == 0)
				return false;
			if (spec.Kind == RenderHookKind::ViewMutation) {
				if (spec.Access != RenderHookAccess::SynchronousMutation ||
					spec.NodeKind != core::Name("view") || spec.ChannelCount != 0 ||
					spec.MutatedFieldCount == 0 || spec.MutatedFieldCount > spec.MutatedFields.size())
					return false;
				for (size_t index = 0; index < spec.MutatedFieldCount; ++index) {
					if (static_cast<size_t>(spec.MutatedFields[index]) >
						static_cast<size_t>(RenderHookMutatedField::Projection))
						return false;
					for (size_t previous = 0; previous < index; ++previous)
						if (spec.MutatedFields[previous] == spec.MutatedFields[index]) return false;
				}
				return true;
			}
			if (spec.Kind != RenderHookKind::DataCapture || spec.Access != RenderHookAccess::Observation ||
				spec.ChannelCount == 0 || spec.ChannelCount > spec.Channels.size() ||
				spec.MutatedFieldCount != 0)
				return false;
			for (size_t index = 0; index < spec.ChannelCount; ++index) {
				if (static_cast<size_t>(spec.Channels[index]) >
					static_cast<size_t>(DataCaptureChannel::PackedGpu))
					return false;
				for (size_t previous = 0; previous < index; ++previous)
					if (spec.Channels[previous] == spec.Channels[index]) return false;
			}
			return true;
		}

		uint32_t NextGeneration(uint32_t generation) {
			return generation == UINT32_MAX ? 0 : generation + 1;
		}

		bool Text(std::string_view value) {
			return !value.empty() && value.size() <= 256 && value.find('\0') == std::string_view::npos;
		}
		bool ValidIdentity(const ViewMutationIdentity &identity) {
			return Text(identity.WorldName) && Text(identity.SnapshotId) && identity.Pipeline.IsValid() &&
				   !identity.Pipeline.Text().empty() && identity.Pipeline.Text().size() <= 128 &&
				   identity.PipelineRevision != 0;
		}
		bool ValidFrame(const core::CFrame &frame) {
			const auto &position = frame.Position;
			const double norm = double(frame.QuaternionX) * frame.QuaternionX +
								double(frame.QuaternionY) * frame.QuaternionY +
								double(frame.QuaternionZ) * frame.QuaternionZ +
								double(frame.QuaternionW) * frame.QuaternionW;
			return std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z) &&
				   std::isfinite(frame.QuaternionX) && std::isfinite(frame.QuaternionY) &&
				   std::isfinite(frame.QuaternionZ) && std::isfinite(frame.QuaternionW) &&
				   std::abs(norm - 1.0) <= .001;
		}
		bool ValidCamera(const scene::Camera &camera) {
			constexpr uint32_t MAX_EDGE = 16384;
			if (!std::isfinite(camera.FieldOfViewRadians) || !std::isfinite(camera.NearPlane) ||
				!std::isfinite(camera.FarPlane) || camera.FieldOfViewRadians <= 0.0f ||
				camera.FieldOfViewRadians >= std::numbers::pi_v<float> || camera.NearPlane <= 0.0f ||
				camera.FarPlane <= camera.NearPlane || camera.MaxImageWidth > MAX_EDGE ||
				camera.MaxImageHeight > MAX_EDGE)
				return false;
			const uint32_t maxWidth = camera.MaxImageWidth == 0 ? MAX_EDGE : camera.MaxImageWidth;
			const uint32_t maxHeight = camera.MaxImageHeight == 0 ? MAX_EDGE : camera.MaxImageHeight;
			return (camera.ImageWidth == 0 && camera.ImageHeight == 0) ||
				   (camera.ImageWidth > 0 && camera.ImageHeight > 0 && camera.ImageWidth <= maxWidth &&
					camera.ImageHeight <= maxHeight);
		}
		bool ValidProjection(const glm::mat4 &projection) {
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					if (!std::isfinite(projection[column][row])) return false;
			const float determinant = glm::determinant(projection);
			if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f) return false;
			const glm::mat4 inverse = glm::inverse(projection);
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					if (!std::isfinite(inverse[column][row])) return false;
			return true;
		}
		bool SameIdentity(const ViewMutationIdentity &left, const ViewMutationIdentity &right) {
			return left.WorldName == right.WorldName && left.SnapshotId == right.SnapshotId &&
				   left.Pipeline == right.Pipeline && left.PipelineRevision == right.PipelineRevision &&
				   left.ViewSlot == right.ViewSlot;
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
			std::array<bool, MAX_DATA_CAPTURE_LOCAL_LIGHT_IDS> LocalLightCaptureMatched{};
			uint16_t PollAttempts = 0;
			size_t AccountedBytes = 0;
			uint64_t SubmittedNanoseconds = 0;
		};
		struct MutationSlot {
			HookHandle Hook;
			ViewCameraPatch Patch;
			uint32_t Generation = 1;
			bool Used = false;
			ViewMutationStatus Status = ViewMutationStatus::Pending;
			bool CancelRequested = false;
		};
		std::array<HookSlot, MAX_DATA_FACTORY_HOOKS> Hooks{};
		std::array<ConnectionSlot, MAX_DATA_FACTORY_CONNECTIONS> Connections{};
		std::array<BatchSlot, MAX_DATA_FACTORY_BATCHES> Batches{};
		std::array<MutationSlot, MAX_DATA_FACTORY_BATCHES> Mutations{};
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
		bool Valid(MutationHandle handle) const {
			return handle.Slot < Mutations.size() && Mutations[handle.Slot].Used &&
				   Mutations[handle.Slot].Generation == handle.Generation;
		}
		void Release(MutationSlot &mutation) {
			const uint32_t generation = NextGeneration(mutation.Generation);
			mutation = {};
			mutation.Generation = generation;
		}
		static bool Terminal(ViewMutationStatus status) {
			return status != ViewMutationStatus::Pending &&
				   status != ViewMutationStatus::AppliedAwaitingRestore;
		}
		void Release(BatchSlot &batch) {
			if (batch.Submitted) RendererRef.CancelDataCapture(batch.Ticket);
			ReadyBytes -= batch.AccountedBytes;
			if (batch.AccountedBytes != 0)
				core::Metrics::Count("render.data_capture_hook.released_bytes", batch.AccountedBytes);
			const uint32_t generation = NextGeneration(batch.Generation);
			batch = {};
			batch.Generation = generation;
		}
		void RecordReadback(const BatchSlot &batch) const {
			if (batch.SubmittedNanoseconds == 0) return;
			core::Metrics::ObserveTime(
				"render.data_capture_hook.readback_latency",
				core::Clock::Nanoseconds() - batch.SubmittedNanoseconds
			);
			core::Metrics::Observe("render.data_capture_hook.readback_polls", batch.PollAttempts);
		}
		void Fail(BatchSlot &batch, DataCaptureStatus status) {
			if (batch.Submitted) RendererRef.CancelDataCapture(batch.Ticket);
			if (status != DataCaptureStatus::Cancelled) RecordReadback(batch);
			if (status != DataCaptureStatus::Cancelled)
				core::Metrics::Count("render.data_capture_hook.drops", 1);
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
		for (auto &mutation : State->Mutations)
			if (mutation.Used) mutation.Status = ViewMutationStatus::Cancelled;
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

	ArmViewMutationResult DataFactoryHookBind::ArmViewMutation(HookHandle hook, ViewCameraPatch patch) {
		if (!State->Valid(hook)) return {.Status = HookBindStatus::UnknownHandle, .Mutation = {}};
		const RenderHookSpec &spec = State->Hooks[hook.Slot].Spec;
		if (spec.Kind != RenderHookKind::ViewMutation ||
			spec.Access != RenderHookAccess::SynchronousMutation || spec.Name != core::Name("view.camera") ||
			!ValidIdentity(patch.Identity) || (!patch.CameraFrame && !patch.Camera && !patch.Projection) ||
			(patch.CameraFrame && !ValidFrame(*patch.CameraFrame)) ||
			(patch.Camera && !ValidCamera(*patch.Camera)) ||
			(patch.Projection && !ValidProjection(*patch.Projection)))
			return {.Status = HookBindStatus::Invalid, .Mutation = {}};
		if (!State->RendererRef.HasPipelineRevision(patch.Identity.Pipeline, patch.Identity.PipelineRevision))
			return {.Status = HookBindStatus::Stale, .Mutation = {}};
		for (const auto &mutation : State->Mutations)
			if (mutation.Used && SameIdentity(mutation.Patch.Identity, patch.Identity))
				return {.Status = HookBindStatus::Conflict, .Mutation = {}};
		for (size_t index = 0; index < State->Mutations.size(); ++index) {
			auto &slot = State->Mutations[index];
			if (slot.Used || slot.Generation == 0) continue;
			slot.Hook = hook;
			slot.Patch = std::move(patch);
			slot.Used = true;
			return {
				.Status = HookBindStatus::Ok, .Mutation = {static_cast<uint16_t>(index), slot.Generation}
			};
		}
		return {.Status = HookBindStatus::Capacity, .Mutation = {}};
	}

	void DataFactoryHookBind::Cancel(MutationHandle mutation) {
		if (!State->Valid(mutation)) return;
		auto &slot = State->Mutations[mutation.Slot];
		if (slot.Status == ViewMutationStatus::AppliedAwaitingRestore)
			slot.CancelRequested = true;
		else if (slot.Status == ViewMutationStatus::Pending)
			slot.Status = ViewMutationStatus::Cancelled;
	}

	ViewMutationPoll DataFactoryHookBind::PollViewMutation(MutationHandle mutation) const {
		if (!State->Valid(mutation)) return {.Status = ViewMutationStatus::Invalid, .Terminal = true};
		const ViewMutationStatus status = State->Mutations[mutation.Slot].Status;
		return {.Status = status, .Terminal = Impl::Terminal(status)};
	}

	void DataFactoryHookBind::ReleaseViewMutation(MutationHandle mutation) {
		if (State->Valid(mutation) && Impl::Terminal(State->Mutations[mutation.Slot].Status))
			State->Release(State->Mutations[mutation.Slot]);
	}

	void DataFactoryHookBind::DiscardViewMutation(MutationHandle mutation) {
		if (!State->Valid(mutation)) return;
		auto &slot = State->Mutations[mutation.Slot];
		slot.Status = ViewMutationStatus::Cancelled;
		State->Release(slot);
	}

	bool DataFactoryHookBind::HasViewMutation(const ViewMutationIdentity &identity) const {
		return std::any_of(State->Mutations.begin(), State->Mutations.end(), [&](const auto &mutation) {
			return mutation.Used && mutation.Status == ViewMutationStatus::Pending &&
				   SameIdentity(mutation.Patch.Identity, identity);
		});
	}

	bool DataFactoryHookBind::HasViewMutationRestore(const ViewMutationIdentity &identity) const {
		return std::any_of(State->Mutations.begin(), State->Mutations.end(), [&](const auto &mutation) {
			return mutation.Used && mutation.Status == ViewMutationStatus::AppliedAwaitingRestore &&
				   SameIdentity(mutation.Patch.Identity, identity);
		});
	}

	void DataFactoryHookBind::CompleteViewMutationRestore(const ViewMutationIdentity &identity) {
		for (auto &mutation : State->Mutations)
			if (mutation.Used && mutation.Status == ViewMutationStatus::AppliedAwaitingRestore &&
				SameIdentity(mutation.Patch.Identity, identity))
				mutation.Status =
					mutation.CancelRequested ? ViewMutationStatus::Cancelled : ViewMutationStatus::Applied;
	}

	bool DataFactoryHookBind::ConsumeViewMutation(const ViewMutationIdentity &identity, View &view) {
		for (auto &mutation : State->Mutations) {
			if (!mutation.Used || mutation.Status != ViewMutationStatus::Pending ||
				!SameIdentity(mutation.Patch.Identity, identity))
				continue;
			if (!State->RendererRef.HasPipelineRevision(identity.Pipeline, identity.PipelineRevision)) {
				mutation.Status = ViewMutationStatus::Stale;
				return false;
			}
			const ViewCameraPatch &patch = mutation.Patch;
			if (patch.CameraFrame) view.CameraFrame = *patch.CameraFrame;
			if (patch.Camera) view.Camera = *patch.Camera;
			if (patch.Projection) view.Projection = *patch.Projection;
			view.Damage.Scene = true;
			view.Damage.Viewport = true;
			view.Damage.Environment = true;
			view.Damage.Portals = true;
			mutation.Status = ViewMutationStatus::AppliedAwaitingRestore;
			return true;
		}
		return false;
	}

	CallHooksResult DataFactoryHookBind::CallHooks(
		ConnectionHandle connection, BatchHandle batch, const RenderObservationContext &context
	) {
		ENGINE_PROFILE_CAT("data capture hook dispatch", core::ProfileCategory::Render);
		if (!State->Valid(connection) || !State->Valid(batch))
			return {.Status = HookBindStatus::Stale, .Batch = {}};
		auto &pending = State->Batches[batch.Slot];
		const auto &request = State->Connections[connection.Slot].Request;
		if (pending.Connection.Slot != connection.Slot ||
			pending.Connection.Generation != connection.Generation || pending.Submitted || pending.Ready ||
			context.WorldName.Text() != request.Session.WorldName ||
			context.SnapshotId != pending.Request.SnapshotId ||
			context.Pipeline != pending.Request.Pipeline || context.ViewSlot != pending.Request.ViewSlot ||
			!State->RendererRef.HasPipelineRevision(request.Pipeline, request.PipelineRevision) ||
			context.PipelineRevision != request.PipelineRevision)
			return {.Status = HookBindStatus::Invalid, .Batch = {}};
		core::Metrics::Count("render.data_capture_hook.dispatches", 1);
		if (!State->RendererRef.QueueDataCapture(pending.Request, pending.Ticket)) {
			core::Metrics::Count("render.data_capture_hook.backpressure", 1);
			return {.Status = HookBindStatus::Backpressured, .Batch = {}};
		}
		pending.Observation = context;
		pending.Submitted = true;
		pending.SubmittedNanoseconds = core::Clock::Nanoseconds();
		return {.Status = HookBindStatus::Ok, .Batch = batch};
	}

	bool DataFactoryHookBind::ApplyLocalLightCapture(
		const ViewMutationIdentity &identity, ViewRecording &recording
	) {
		std::fill(recording.LocalLightCaptureIds.begin(), recording.LocalLightCaptureIds.end(), core::Name{});
		std::fill(
			recording.LocalLightCaptureMatched.begin(), recording.LocalLightCaptureMatched.end(), false
		);
		std::fill(
			recording.LocalLightShadowCaptureAvailable.begin(),
			recording.LocalLightShadowCaptureAvailable.end(),
			false
		);
		for (const auto &batch : State->Batches) {
			if (!batch.Used || !batch.Submitted || batch.Ready || !State->Valid(batch.Connection)) continue;
			const auto &connection = State->Connections[batch.Connection.Slot];
			if (connection.Request.PipelineRevision != identity.PipelineRevision ||
				connection.Request.Session.WorldName != identity.WorldName ||
				batch.Request.SnapshotId != identity.SnapshotId ||
				batch.Request.Pipeline != identity.Pipeline || batch.Request.ViewSlot != identity.ViewSlot ||
				(std::find(
					 batch.Request.Channels.begin(),
					 batch.Request.Channels.end(),
					 DataCaptureChannel::LocalLightContribution
				 ) == batch.Request.Channels.end() &&
				 std::find(
					 batch.Request.Channels.begin(),
					 batch.Request.Channels.end(),
					 DataCaptureChannel::LocalLightShadowVisibility
				 ) == batch.Request.Channels.end()))
				continue;
			for (size_t index = 0; index < batch.Request.LocalLightIds.size(); ++index)
				recording.LocalLightCaptureIds[index] = core::Name(batch.Request.LocalLightIds[index]);
			return true;
		}
		return false;
	}

	void DataFactoryHookBind::CompleteLocalLightCapture(
		const ViewMutationIdentity &identity, const ViewRecording &recording
	) {
		for (auto &batch : State->Batches) {
			if (!batch.Used || !batch.Submitted || batch.Ready || !State->Valid(batch.Connection)) continue;
			const auto &connection = State->Connections[batch.Connection.Slot];
			if (connection.Request.PipelineRevision != identity.PipelineRevision ||
				connection.Request.Session.WorldName != identity.WorldName ||
				batch.Request.SnapshotId != identity.SnapshotId ||
				batch.Request.Pipeline != identity.Pipeline || batch.Request.ViewSlot != identity.ViewSlot ||
				(std::find(
					 batch.Request.Channels.begin(),
					 batch.Request.Channels.end(),
					 DataCaptureChannel::LocalLightContribution
				 ) == batch.Request.Channels.end() &&
				 std::find(
					 batch.Request.Channels.begin(),
					 batch.Request.Channels.end(),
					 DataCaptureChannel::LocalLightShadowVisibility
				 ) == batch.Request.Channels.end()))
				continue;
			batch.LocalLightCaptureMatched = recording.LocalLightCaptureMatched;
			for (size_t index = 0; index < batch.Ticket.Channels.size(); ++index) {
				const DataCaptureChannel channel = batch.Ticket.Channels[index];
				if (channel != DataCaptureChannel::LocalLightContribution &&
					channel != DataCaptureChannel::LocalLightShadowVisibility)
					continue;
				size_t localSlot = 0;
				for (size_t prior = 0; prior < index; ++prior)
					if (batch.Ticket.Channels[prior] == channel) ++localSlot;
				if (index < batch.Ticket.LocalLightMatched.size() &&
					localSlot < recording.LocalLightCaptureMatched.size()) {
					batch.Ticket.LocalLightMatched[index] =
						recording.LocalLightCaptureMatched[localSlot] ? 1 : 0;
					batch.Ticket.LocalLightShadowAvailable[index] =
						recording.LocalLightShadowCaptureAvailable[localSlot] ? 1 : 0;
				}
			}
			return;
		}
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
		for (auto &mutation : State->Mutations)
			if (mutation.Used) mutation.Status = ViewMutationStatus::Cancelled;
	}

	void DataFactoryHookBind::Pump() {
		ENGINE_PROFILE_CAT("data capture hook pump", core::ProfileCategory::Render);
		for (auto &mutation : State->Mutations)
			if (mutation.Used &&
				(mutation.Status == ViewMutationStatus::Pending ||
				 mutation.Status == ViewMutationStatus::AppliedAwaitingRestore) &&
				!State->RendererRef.HasPipelineRevision(
					mutation.Patch.Identity.Pipeline, mutation.Patch.Identity.PipelineRevision
				))
				mutation.Status = ViewMutationStatus::Stale;
		for (auto &batch : State->Batches) {
			if (!batch.Used || !batch.Submitted || batch.Ready) continue;
			++batch.PollAttempts;
			core::Metrics::Count("render.data_capture_hook.readback_poll_calls", 1);
			DataCapturePoll poll = State->RendererRef.PollDataCapture(batch.Ticket);
			if (poll.Status == DataCaptureStatus::Pending) {
				if (batch.PollAttempts >= MAX_DATA_FACTORY_PENDING_PUMPS)
					State->Fail(batch, DataCaptureStatus::Failed);
				continue;
			}
			if (poll.Status == DataCaptureStatus::Unsupported) {
				// Virtual unavailable channels have no readback image, so the hook
				// supplies the observation identity captured at dispatch.
				poll.CaptureFrame = batch.Observation.Frame;
				poll.Pipeline = batch.Observation.Pipeline;
				poll.PipelineRevision = batch.Observation.PipelineRevision;
				poll.WorldName = batch.Observation.WorldName;
				poll.ViewSlot = batch.Observation.ViewSlot;
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
			core::Metrics::Count("render.data_capture_hook.package_bytes", bytes);
			core::Metrics::Count("render.data_capture_hook.retained_bytes", bytes);
			State->RecordReadback(batch);
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
		for (size_t index = 0; index <= static_cast<size_t>(DataCaptureChannel::PackedGpu); ++index) {
			const auto channel = static_cast<DataCaptureChannel>(index);
			if (channel == DataCaptureChannel::OpticalFlow) continue;
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

	void DataFactoryHookBind::RegisterBuiltInViewMutationHooks() {
		(void)RegisterHook({
			.Name = core::Name("view.camera"),
			.Kind = RenderHookKind::ViewMutation,
			.Access = RenderHookAccess::SynchronousMutation,
			.NodeKind = core::Name("view"),
			.SchemaVersion = 1,
			.Required = true,
			.Channels = {},
			.ChannelCount = 0,
			.MutatedFields =
				{
					RenderHookMutatedField::CameraFrame,
					RenderHookMutatedField::Camera,
					RenderHookMutatedField::Projection,
				},
			.MutatedFieldCount = 3,
		});
	}
}
