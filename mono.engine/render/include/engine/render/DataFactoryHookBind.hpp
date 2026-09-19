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

	inline constexpr size_t MAX_DATA_FACTORY_HOOKS = 17;
	inline constexpr size_t MAX_DATA_FACTORY_CONNECTIONS = 6;
	inline constexpr size_t MAX_DATA_FACTORY_BATCHES = 6;
	inline constexpr size_t MAX_DATA_FACTORY_HOOK_CHANNELS = 1;
	inline constexpr size_t MAX_DATA_FACTORY_READBACK_NODES = 10;
	inline constexpr size_t MAX_DATA_FACTORY_RETAINED_BYTES = 64 * 1024 * 1024;
	// Pump runs once per owner frame, so this bounds a lost completion to ten
	// seconds at a 60 Hz owner cadence without a blocking wall-clock dependency.
	inline constexpr uint16_t MAX_DATA_FACTORY_PENDING_PUMPS = 600;

	struct HookHandle {
		uint16_t Slot = UINT16_MAX;
		uint32_t Generation = 0;
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	struct ConnectionHandle {
		uint16_t Slot = UINT16_MAX;
		uint32_t Generation = 0;
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	struct BatchHandle {
		uint16_t Slot = UINT16_MAX;
		uint32_t Generation = 0;
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};
	struct MutationHandle {
		uint16_t Slot = UINT16_MAX;
		uint32_t Generation = 0;
		constexpr bool IsValid() const {
			return Slot != UINT16_MAX && Generation != 0;
		}
	};

	// A string, rather than a world id, is the session identity retained by the
	// renderer. Handles are process-local and never escape this API.
	struct DataFactorySessionKey {
		std::string WorldName;
	};
	struct HookConnectionRequest {
		DataFactorySessionKey Session;
		core::Name Pipeline;
		uint64_t PipelineRevision = 0;
		size_t ViewSlot = 0;
		core::Name CaptureNode;
		uint32_t ViewWidth = 0;
		uint32_t ViewHeight = 0;
	};
	enum class RenderHookKind : uint8_t { DataCapture, ViewMutation };
	enum class RenderHookAccess : uint8_t { Observation, SynchronousMutation };
	enum class RenderHookMutatedField : uint8_t { CameraFrame, Camera, Projection };
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

	struct RenderHookSpec {
		core::Name Name;
		RenderHookKind Kind = RenderHookKind::DataCapture;
		RenderHookAccess Access = RenderHookAccess::Observation;
		core::Name NodeKind;
		uint32_t SchemaVersion = 1;
		bool Required = true;
		std::array<DataCaptureChannel, MAX_DATA_FACTORY_HOOK_CHANNELS> Channels{};
		uint8_t ChannelCount = 0;
		std::array<RenderHookMutatedField, 3> MutatedFields{};
		uint8_t MutatedFieldCount = 0;
	};
	using RenderHookCapability = RenderHookSpec;

	struct ConnectHooksResult {
		HookBindStatus Status = HookBindStatus::Invalid;
		ConnectionHandle Connection;
	};
	struct HookBundle {
		DataCapturePoll Capture;
	};
	struct CallHooksResult {
		HookBindStatus Status = HookBindStatus::Invalid;
		BatchHandle Batch;
	};
	// This identity is complete before admission. A patch cannot drift to a later
	// snapshot, a replaced pipeline, or another viewport while it is pending.
	struct ViewMutationIdentity {
		std::string WorldName;
		std::string SnapshotId;
		core::Name Pipeline;
		uint64_t PipelineRevision = 0;
		size_t ViewSlot = 0;
	};
	struct ViewCameraPatch {
		ViewMutationIdentity Identity;
		std::optional<core::CFrame> CameraFrame;
		std::optional<scene::Camera> Camera;
		std::optional<glm::mat4> Projection;
	};
	struct ArmViewMutationResult {
		HookBindStatus Status = HookBindStatus::Invalid;
		MutationHandle Mutation;
	};
	enum class ViewMutationStatus : uint8_t {
		Pending,
		AppliedAwaitingRestore,
		Applied,
		Cancelled,
		Stale,
		Invalid
	};
	struct ViewMutationPoll {
		ViewMutationStatus Status = ViewMutationStatus::Invalid;
		bool Terminal = true;
	};

	// The render-owned registry for fixed observation and synchronous mutation
	// hooks. It has no callback path. All methods run on the renderer owner thread.
	class DataFactoryHookBind {
	  public:
		explicit DataFactoryHookBind(Renderer &renderer);
		~DataFactoryHookBind();
		DataFactoryHookBind(const DataFactoryHookBind &) = delete;
		DataFactoryHookBind &operator=(const DataFactoryHookBind &) = delete;

		HookHandle RegisterHook(const RenderHookSpec &spec);
		HookHandle FindHook(core::Name name) const;
		std::vector<RenderHookCapability> DescribeHooks() const;
		ConnectHooksResult
		ConnectHooks(const HookConnectionRequest &request, std::span<const HookHandle> hooks);
		void DisconnectHooks(std::span<const ConnectionHandle> connections);

		// Arms a bounded batch. CallHooks admits readback work before graph execution
		// and remains nonblocking.
		std::optional<BatchHandle> ArmDataCapture(ConnectionHandle connection, DataCaptureRequest request);
		CallHooksResult
		CallHooks(ConnectionHandle connection, BatchHandle batch, const RenderObservationContext &context);
		// Queues an owned one-shot patch for the built-in view.camera hook. Admission
		// validates the complete identity and payload before it claims a slot.
		ArmViewMutationResult ArmViewMutation(HookHandle hook, ViewCameraPatch patch);
		void Cancel(MutationHandle mutation);
		ViewMutationPoll PollViewMutation(MutationHandle mutation) const;
		void ReleaseViewMutation(MutationHandle mutation);
		// Teardown owns no future unmodified view, so it may retire a patch that
		// was applied to an earlier stack-local render copy.
		void DiscardViewMutation(MutationHandle mutation);
		bool HasViewMutation(const ViewMutationIdentity &identity) const;
		bool HasViewMutationRestore(const ViewMutationIdentity &identity) const;
		void CompleteViewMutationRestore(const ViewMutationIdentity &identity);
		// Consumes only an exact identity and mutates the renderer's private stack copy.
		bool ConsumeViewMutation(const ViewMutationIdentity &identity, View &view);
		// Observe is the graph-output seam. It records immutable graph context, never
		// GPU work, and is safe when no batch is armed.
		void Observe(const RenderObservationContext &context);
		void Cancel(BatchHandle batch);
		void Shutdown();
		void Pump();
		std::optional<HookBundle> TakeCompleted(BatchHandle batch);

		// Registers the twelve concrete channels that currently have graph-backed
		// capture support. Names are stable manifest and capability identifiers.
		void RegisterBuiltInDataCaptureHooks();
		void RegisterBuiltInViewMutationHooks();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
