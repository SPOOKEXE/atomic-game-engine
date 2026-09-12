#pragma once

#include <engine/render/DataCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <cstddef>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::render {
	class ScriptDataCaptureBridge final : public script::DataCaptureBridge {
	  public:
		ScriptDataCaptureBridge(world::DataFactorySession &session, Renderer &renderer)
			: Session(session), RendererRef(renderer) {}
		~ScriptDataCaptureBridge() override;
		script::DataCaptureBridgeCapabilities Capabilities() const override;
		bool
		Queue(std::string_view, const script::DataCaptureBridgeRequest &, uint64_t &, std::string &) override;
		bool Poll(std::string_view, uint64_t, script::DataCaptureBridgePoll &, std::string &) override;
		bool ReadPlane(
			std::string_view,
			uint64_t,
			std::string_view,
			size_t,
			size_t,
			std::vector<std::byte> &,
			std::string &
		) override;
		bool Release(std::string_view, uint64_t, std::string &) override;
		void Cancel(std::string_view, uint64_t) override;
		void PrepareView(View &view);
		void Pump();
		bool HasPending() const;

	  private:
		struct Entry {
			script::DataCaptureBridgeRequest Request;
			script::DataCaptureBridgePoll Reply;
			std::unordered_map<std::string, std::vector<std::byte>> PlaneBytes;
			std::string Detail;
			bool Preparing = false;
			bool CancelRequested = false;
			bool Terminal = false;
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
		std::unordered_map<uint64_t, Entry> Entries;
		size_t RetainedBytes = 0;
		bool CaptureAvailable = false;
		// Accessed only by the renderer owner through PrepareView, Pump, and destruction.
		std::unordered_map<uint64_t, DataCaptureTicket> OwnerTickets;
	};
}
