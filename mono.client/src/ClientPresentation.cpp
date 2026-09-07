#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <client/Client.hpp>
#include <limits>

namespace client {
	void Client::ClearPlayPresentationRoutes() {
		if (!PortalImages || PlayRoutesRevision == std::numeric_limits<uint64_t>::max()) return;
		(void)PortalImages->AcceptDriverRoutes({1, ++PlayRoutesRevision, {}});
	}
	void Client::ResetPlayPresentation() {
		if (Settings.PresentationSession != 0) return;
		ClearPlayPresentationRoutes();
		PlayPresentation = std::make_unique<engine::world::PresentationStream>();
		PlayDirectorySession = PlayDirectoryRevision = 0;
	}
	bool Client::ReceivePlayPresentation(std::span<const std::byte> bytes) {
		using namespace engine::world;
		if (!PlayPresentation || !PresentationStream::Recognizes(bytes)) return false;
		if (PlayPresentation->Receive(bytes) == PresentationStreamReceive::Refused) {
			ClearPlayPresentationRoutes();
			return true;
		}
		for (auto &frame : PlayPresentation->Take()) {
			if (frame.Kind == PresentationStreamKind::Routes &&
				PlayRoutesRevision != std::numeric_limits<uint64_t>::max()) {
				// Connection replacement is ordered here. Route-directory revision belongs
				// to this Client lifetime; producer endpoint incarnations stay unchanged.
				frame.Directory.Session = 1;
				frame.Directory.Revision = ++PlayRoutesRevision;
				if (PortalImages->AcceptDriverRoutes(frame.Directory) == PresentationStatus::Ok) continue;
			} else if (frame.Kind == PresentationStreamKind::Message) {
				if (Universe_->AcceptPresentationFromDriver(frame.Message) != PresentationStatus::Ok)
					engine::core::Metrics::Count("client.presentation.refused", 1);
				continue;
			}
			PlayPresentation->Close();
			ClearPlayPresentationRoutes();
			break;
		}
		return true;
	}
	void Client::PumpPlayPresentation() {
		using namespace engine::world;
		if (!PlayPresentation || !PlayPresentation->Open() || !Connection) return;
		if (Connection->Admitted() && !Connection->Live()) {
			PlayPresentation->Close();
			ClearPlayPresentationRoutes();
			return;
		}
		if (!Connection->Admitted()) return;
		auto directory = Universe_->LocalPresentationDirectory();
		if (directory.Session != PlayDirectorySession || directory.Revision != PlayDirectoryRevision) {
			std::erase_if(directory.Endpoints, [](const auto &endpoint) {
				return endpoint.Channel != "portal-image-replies" &&
					   endpoint.Channel != "portal-topology-replies" &&
					   !endpoint.Channel.starts_with("portal-image-replies/");
			});
			PresentationStreamFrame frame;
			frame.Kind = PresentationStreamKind::Directory;
			frame.Directory = directory;
			if (PlayPresentation->Queue(frame) == PresentationStatus::Ok) {
				PlayDirectorySession = directory.Session;
				PlayDirectoryRevision = directory.Revision;
			} else {
				PlayPresentation->Flush([&](auto packet) {
					return Connection->SendUser(packet, engine::core::Clock::Seconds());
				});
				return;
			}
		}
		for (auto &outbound : Universe_->TakePresentationOutbound()) {
			PresentationStreamFrame frame;
			frame.Message = std::move(outbound.Message);
			if (PlayPresentation->Queue(frame) != PresentationStatus::Ok)
				engine::core::Metrics::Count("client.presentation.request-drops", 1);
		}
		PlayPresentation->Flush([&](auto packet) {
			return Connection->SendUser(packet, engine::core::Clock::Seconds());
		});
	}
}
