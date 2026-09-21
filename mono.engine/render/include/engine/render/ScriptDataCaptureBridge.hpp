#pragma once

#include <engine/render/DataCapture.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::render {
	// Adapts script capture tickets to renderer readbacks and the world snapshot barrier.
	class ScriptDataCaptureBridge final : public script::DataCaptureBridge {
	  public:
		// Renderer work armed for one prepared view.
		struct PreparedView {
			// Capture tickets to cancel if the view cannot be submitted.
			std::vector<uint64_t> Captures;
			// Camera mutation tickets tied to this view.
			std::vector<uint64_t> CameraMutations;
		};
		// Prepared views that must be aborted together on submission failure.
		struct PreparedBatch {
			// Per-view tickets in the coordinated capture group.
			std::vector<PreparedView> Views;
		};
		// Borrows a world session and its renderer for this bridge's lifetime.
		ScriptDataCaptureBridge(world::DataFactorySession &session, Renderer &renderer);
		~ScriptDataCaptureBridge() override;
		// Reports the channels and camera hooks this renderer can service.
		script::DataCaptureBridgeCapabilities Capabilities() const override;
		// Queues one copied capture request and returns its bridge ticket.
		bool
		Queue(std::string_view, const script::DataCaptureBridgeRequest &, uint64_t &, std::string &) override;
		// Queues a coordinated group whose views must share one completed frame.
		bool QueueGroup(
			std::string_view,
			std::span<const script::DataCaptureBridgeRequest>,
			std::span<uint64_t>,
			std::string &
		) override;
		// Polls a ticket without transferring its retained plane bytes.
		bool Poll(std::string_view, uint64_t, script::DataCaptureBridgePoll &, std::string &) override;
		// Copies a bounded range of one completed plane into caller-owned bytes.
		bool ReadPlane(
			std::string_view,
			uint64_t,
			std::string_view,
			size_t,
			size_t,
			std::vector<std::byte> &,
			std::string &
		) override;
		// Releases terminal ticket state and retained plane bytes.
		bool Release(std::string_view, uint64_t, std::string &) override;
		// Cancels every nonterminal member of a same-frame camera group. The owner
		// pump publishes each affected ticket as cancelled before it can block a later group.
		void Cancel(std::string_view, uint64_t) override;
		// Queues a named camera change for the next prepared capture view.
		bool QueueViewCameraMutation(
			std::string_view, const script::ViewCameraMutationRequest &, uint64_t &, std::string &
		) override;
		// Requests cancellation of a queued view-camera mutation.
		void CancelViewCameraMutation(std::string_view, uint64_t) override;
		// Polls the mutation ticket for an applied or terminal outcome.
		bool PollViewCameraMutation(
			std::string_view, uint64_t, script::ViewCameraMutationPoll &, std::string &
		) override;
		// Releases every terminal payload and cancels every renderer ticket for one world.
		bool TeardownInstance(std::string_view instanceId, std::string &detail);
		// Arms compatible capture work and returns true only when a named camera
		// replaced the supplied view camera.
		bool PrepareView(View &view, PreparedView *prepared = nullptr);
		// Builds one offscreen view per coordinated camera request. The caller owns
		// targets and world packets, then calls PrepareView for every returned view.
		bool PrepareBatch(const View &source, std::vector<View> &views, PreparedBatch *prepared = nullptr);
		// Cancels all capture and mutation tickets armed for a failed batch.
		void AbortPreparedBatch(const PreparedBatch &prepared);
		// Cancels capture and camera work prepared for this exact view. A host uses
		// this when it cannot bind the rebuilt world packet that the capture needs.
		void AbortPreparedView(const PreparedView &prepared);
		// Advances queued captures and publishes completed readbacks.
		void Pump();
		// Reports whether tickets still need renderer or session work.
		bool HasPending() const;

	  private:
		struct Entry {
			script::DataCaptureBridgeRequest Request;
			script::DataCaptureBridgePoll Reply;
			std::unordered_map<std::string, std::vector<std::byte>> PlaneBytes;
			std::optional<script::DataCaptureBridgeSceneSidecar> SceneSidecar;
			size_t SceneSidecarBytes = 0;
			std::string Detail;
			bool Preparing = false;
			bool CancelRequested = false;
			bool Terminal = false;
			uint64_t Group = 0;
			std::optional<size_t> PhysicalViewSlot;
		};
		struct PendingRequest {
			uint64_t Id = 0;
			script::DataCaptureBridgeRequest Request;
		};
		void RefreshCapabilities();
		world::DataFactorySession &Session;
		Renderer &RendererRef;
		mutable std::mutex Mutex;
		uint64_t NextTicket = 1;
		uint64_t NextGroup = 1;
		std::unordered_map<uint64_t, Entry> Entries;
		struct MutationEntry {
			script::ViewCameraMutationRequest Request;
			MutationHandle Handle;
			ViewMutationStatus Status = ViewMutationStatus::Pending;
			std::string Detail;
			bool CancelRequested = false;
		};
		std::unordered_map<uint64_t, MutationEntry> Mutations;
		size_t RetainedBytes = 0;
		bool CaptureAvailable = false;
		std::vector<script::DataCaptureBridgeHookCapability> HookCapabilities;
		struct HookState;
		// Accessed only by the renderer owner through PrepareView, Pump, and destruction.
		std::unique_ptr<HookState> Hooks;
	};
}
