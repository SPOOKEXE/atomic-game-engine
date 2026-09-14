#pragma once

#include <engine/core/Name.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/RenderObservation.hpp>

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

	inline constexpr size_t MAX_DATA_FACTORY_HOOKS = 14;
	inline constexpr size_t MAX_DATA_FACTORY_CONNECTIONS = 6;
	inline constexpr size_t MAX_DATA_FACTORY_BATCHES = 6;
	inline constexpr size_t MAX_DATA_FACTORY_HOOK_CHANNELS = 1;
	inline constexpr size_t MAX_DATA_FACTORY_READBACK_NODES = 12;
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
	enum class RenderHookKind : uint8_t { DataCapture };
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
		core::Name NodeKind;
		uint32_t SchemaVersion = 1;
		bool Required = true;
		std::array<DataCaptureChannel, MAX_DATA_FACTORY_HOOK_CHANNELS> Channels{};
		uint8_t ChannelCount = 0;
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

	// The render-owned registry for fixed observation hooks. It has no callback
	// path: typed data capture is the only supported work kind at this seam. All
	// methods run on the renderer owner thread.
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

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
