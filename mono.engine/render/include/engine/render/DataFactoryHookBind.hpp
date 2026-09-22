#pragma once

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/RenderObservation.hpp>
#include <engine/scene/Components.hpp>

#include <glm/mat4x4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	class Renderer;
	struct View;
	class ViewRecording;

	// Maximum registered hook capabilities owned by one renderer.
	inline constexpr size_t MAX_DATA_FACTORY_HOOKS = 22;
	// Maximum live client connections to hook capabilities.
	inline constexpr size_t MAX_DATA_FACTORY_CONNECTIONS = 6;
	// Maximum admitted readback batches awaiting completion.
	inline constexpr size_t MAX_DATA_FACTORY_BATCHES = 6;
	// Maximum capture channels one hook may expose.
	inline constexpr size_t MAX_DATA_FACTORY_HOOK_CHANNELS = 1;
	// Maximum graph nodes whose readback one batch may retain.
	inline constexpr size_t MAX_DATA_FACTORY_READBACK_NODES = 11;
	// Aggregate byte budget for retained capture payloads.
	inline constexpr size_t MAX_DATA_FACTORY_RETAINED_BYTES = 64 * 1024 * 1024;
	// Pump runs once per owner frame, so this bounds a lost completion to ten
	// seconds at a 60 Hz owner cadence without a blocking wall-clock dependency.
	inline constexpr uint16_t MAX_DATA_FACTORY_PENDING_PUMPS = 600;

	// Generation-checked renderer-local reference to a registered hook.
	struct HookHandle {
		// Reusable hook-table slot.
		uint16_t Slot = UINT16_MAX;
		// Slot generation that prevents stale handle reuse.
		uint32_t Generation = 0;
		// Whether this handle names a live hook-table entry.
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	// Generation-checked renderer-local reference to a hook connection.
	struct ConnectionHandle {
		// Reusable connection-table slot.
		uint16_t Slot = UINT16_MAX;
		// Slot generation that prevents stale handle reuse.
		uint32_t Generation = 0;
		// Whether this handle names a live connection-table entry.
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	// Generation-checked renderer-local reference to an admitted capture batch.
	struct BatchHandle {
		// Reusable batch-table slot.
		uint16_t Slot = UINT16_MAX;
		// Slot generation that prevents stale handle reuse.
		uint32_t Generation = 0;
		// Whether this handle names a live batch-table entry.
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	// Generation-checked renderer-local reference to a queued view mutation.
	struct MutationHandle {
		// Reusable mutation-table slot.
		uint16_t Slot = UINT16_MAX;
		// Slot generation that prevents stale handle reuse.
		uint32_t Generation = 0;
		// Whether this handle names a live mutation-table entry.
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};

	// A string, rather than a world id, is the session identity retained by the
	// renderer. Handles are process-local and never escape this API.
	struct DataFactorySessionKey {
		// Stable world name used instead of a process-local world handle.
		std::string WorldName;
	};
	// Identity and viewport dimensions used to admit a hook connection.
	struct HookConnectionRequest {
		// Renderer-retained session identity.
		DataFactorySessionKey Session;
		// Stable graph pipeline name.
		core::Name Pipeline;
		// Pipeline revision that connections must continue to match.
		uint64_t PipelineRevision = 0;
		// Renderer view slot that owns the connection.
		size_t ViewSlot = 0;
		// Graph node that produces the requested capture.
		core::Name CaptureNode;
		// View width in display pixels at admission.
		uint32_t ViewWidth = 0;
		// View height in display pixels at admission.
		uint32_t ViewHeight = 0;
	};
	// Hook execution mode.
	enum class RenderHookKind : uint8_t { DataCapture, ViewMutation };
	// Whether a hook only observes or synchronously mutates a copied view.
	enum class RenderHookAccess : uint8_t { Observation, SynchronousMutation };
	// View fields a mutation hook is permitted to patch.
	enum class RenderHookMutatedField : uint8_t { CameraFrame, Camera, Projection };
	// Admission or completion status returned by the hook registry.
	enum class HookBindStatus : uint8_t {
		Ok,
		Invalid,
		UnknownHandle,
		Conflict,
		Capacity,
		Backpressured,
		Stale,
		Cancelled,
		Failed,
	};

	// Registered hook capability and its graph-schema requirements.
	struct RenderHookSpec {
		// Stable manifest and discovery identifier.
		core::Name Name;
		// Capture or synchronous view-mutation hook mode.
		RenderHookKind Kind = RenderHookKind::DataCapture;
		// Permission level enforced when the hook is called.
		RenderHookAccess Access = RenderHookAccess::Observation;
		// Graph node kind that implements the hook.
		core::Name NodeKind;
		// Required node schema version.
		uint32_t SchemaVersion = 1;
		// Whether connection admission fails when this hook is absent.
		bool Required = true;
		// Capture channels declared by this hook.
		std::array<DataCaptureChannel, MAX_DATA_FACTORY_HOOK_CHANNELS> Channels{};
		// Number of populated Channels entries.
		uint8_t ChannelCount = 0;
		// View fields this mutation hook may alter.
		std::array<RenderHookMutatedField, 3> MutatedFields{};
		// Number of populated MutatedFields entries.
		uint8_t MutatedFieldCount = 0;
	};
	// Copyable capability record returned by hook discovery.
	using RenderHookCapability = RenderHookSpec;

	// Result of connecting one caller to a set of hooks.
	struct ConnectHooksResult {
		// Admission result for the requested connection.
		HookBindStatus Status = HookBindStatus::Invalid;
		// Owned connection slot when admission succeeds.
		ConnectionHandle Connection;
	};
	// Completed data captured for one hook batch.
	struct HookBundle {
		// Pollable immutable readback results for capture channels.
		DataCapturePoll Capture;
	};
	// Result of submitting one armed batch to graph observation points.
	struct CallHooksResult {
		// Submission status for the requested batch.
		HookBindStatus Status = HookBindStatus::Invalid;
		// Batch handle retained when the graph accepts the call.
		BatchHandle Batch;
	};
	// This identity is complete before admission. A patch cannot drift to a later
	// snapshot, a replaced pipeline, or another viewport while it is pending.
	struct ViewMutationIdentity {
		// Stable world identity frozen at mutation admission.
		std::string WorldName;
		// Snapshot identity frozen at mutation admission.
		std::string SnapshotId;
		// Pipeline identity frozen at mutation admission.
		core::Name Pipeline;
		// Pipeline revision frozen at mutation admission.
		uint64_t PipelineRevision = 0;
		// View slot frozen at mutation admission.
		size_t ViewSlot = 0;
	};
	// Optional camera changes applied to one private stack-local render view.
	struct ViewCameraPatch {
		// Complete identity that prevents a patch drifting to another view.
		ViewMutationIdentity Identity;
		// Replacement world-from-camera frame, if permitted.
		std::optional<core::CFrame> CameraFrame;
		// Replacement camera parameters, if permitted.
		std::optional<scene::Camera> Camera;
		// Replacement projection matrix, if permitted.
		std::optional<glm::mat4> Projection;
	};
	// Result of admitting an owned one-shot view patch.
	struct ArmViewMutationResult {
		// Admission status for the requested patch.
		HookBindStatus Status = HookBindStatus::Invalid;
		// Owned mutation slot when admission succeeds.
		MutationHandle Mutation;
	};
	// Lifecycle state of an admitted view mutation.
	enum class ViewMutationStatus : uint8_t {
		Pending,
		AppliedAwaitingRestore,
		Applied,
		Cancelled,
		Stale,
		Invalid
	};
	// Poll result for a queued view mutation.
	struct ViewMutationPoll {
		// Current mutation lifecycle status.
		ViewMutationStatus Status = ViewMutationStatus::Invalid;
		// Whether no further mutation state transition is possible.
		bool Terminal = true;
	};

	// The render-owned registry for fixed observation and synchronous mutation
	// hooks. It has no callback path. All methods run on the renderer owner thread.
	class DataFactoryHookBind {
	  public:
		// Binds this owner-thread registry to its renderer's private state.
		explicit DataFactoryHookBind(Renderer &renderer);
		~DataFactoryHookBind();
		DataFactoryHookBind(const DataFactoryHookBind &) = delete;
		DataFactoryHookBind &operator=(const DataFactoryHookBind &) = delete;

		// Registers a capability and returns its generation-checked local handle.
		HookHandle RegisterHook(const RenderHookSpec &spec);
		// Returns the current local handle for a stable hook name.
		HookHandle FindHook(core::Name name) const;
		// Returns copyable capabilities for discovery without exposing callback state.
		std::vector<RenderHookCapability> DescribeHooks() const;
		// Admits a caller only when its session, pipeline, view, and hooks agree.
		ConnectHooksResult
		ConnectHooks(const HookConnectionRequest &request, std::span<const HookHandle> hooks);
		// Retires caller connections and their pending capture work.
		void DisconnectHooks(std::span<const ConnectionHandle> connections);

		// Arms a bounded batch. CallHooks admits readback work before graph execution
		// and remains nonblocking.
		std::optional<BatchHandle> ArmDataCapture(ConnectionHandle connection, DataCaptureRequest request);
		// Runs one admitted batch against the current render observation context.
		CallHooksResult
		CallHooks(ConnectionHandle connection, BatchHandle batch, const RenderObservationContext &context);
		// Copies the bounded local-light selection for this render observation into
		// its recording before the local-light graph nodes run.
		bool ApplyLocalLightCapture(const ViewMutationIdentity &identity, ViewRecording &recording);
		// Retains the graph's per-slot selection result for the later readback poll.
		void CompleteLocalLightCapture(const ViewMutationIdentity &identity, const ViewRecording &recording);
		// Queues an owned one-shot patch for the built-in view.camera hook. Admission
		// validates the complete identity and payload before it claims a slot.
		ArmViewMutationResult ArmViewMutation(HookHandle hook, ViewCameraPatch patch);
		// Cancels an unapplied or restoration-pending mutation.
		void Cancel(MutationHandle mutation);
		// Returns lifecycle state without consuming the mutation handle.
		ViewMutationPoll PollViewMutation(MutationHandle mutation) const;
		// Releases a terminal mutation slot for generation-safe reuse.
		void ReleaseViewMutation(MutationHandle mutation);
		// Teardown owns no future unmodified view, so it may retire a patch that
		// was applied to an earlier stack-local render copy.
		void DiscardViewMutation(MutationHandle mutation);
		// Reports whether an exact admitted mutation is pending or applied.
		bool HasViewMutation(const ViewMutationIdentity &identity) const;
		// Reports whether an applied exact mutation still awaits restoration.
		bool HasViewMutationRestore(const ViewMutationIdentity &identity) const;
		// Marks an exact applied mutation restored by the owning view lifecycle.
		void CompleteViewMutationRestore(const ViewMutationIdentity &identity);
		// Consumes only an exact identity and mutates the renderer's private stack copy.
		bool ConsumeViewMutation(const ViewMutationIdentity &identity, View &view);
		// Observe is the graph-output seam. It records immutable graph context, never
		// GPU work, and is safe when no batch is armed.
		void Observe(const RenderObservationContext &context);
		// Cancels an armed capture batch and releases its retained readback state.
		void Cancel(BatchHandle batch);
		// Cancels all connections, batches, and mutations during renderer teardown.
		void Shutdown();
		// Advances bounded nonblocking completion polling on the renderer owner thread.
		void Pump();
		// Transfers completed capture data and retires the batch when available.
		std::optional<HookBundle> TakeCompleted(BatchHandle batch);

		// Registers the twelve concrete channels that currently have graph-backed
		// capture support. Names are stable manifest and capability identifiers.
		void RegisterBuiltInDataCaptureHooks();
		// Registers built-in hooks that synchronously patch a private view copy.
		void RegisterBuiltInViewMutationHooks();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
