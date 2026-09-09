#include <engine/assets/Signature.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/parallel/ProcessChannel.hpp>

#include <algorithm>
#include "RetainedBodyGrant.hpp"

#include <network/SessionKey.hpp>
#include <server/Server.hpp>

namespace server {
	bool Server::ReceivePlayerPresentation(
		engine::replication::ClientId client, std::span<const std::byte> bytes
	) {
		using namespace engine::world;
		if (!PresentationStream::Recognizes(bytes)) return false;
		auto found =
			std::find_if(PlayerPresentations.begin(), PlayerPresentations.end(), [client](const auto &peer) {
				return peer->Client == client;
			});
		if (found == PlayerPresentations.end()) {
			if (PlayerPresentations.size() >= 16 || client.Index >= 0x03ffffffu) return true;
			PlayerPresentations.push_back(
				std::make_unique<PlayerPresentation>(Worlds(), PrimaryWorld, client)
			);
			found = std::prev(PlayerPresentations.end());
		}
		auto &peer = **found;
		if (peer.Stream.Receive(bytes) == PresentationStreamReceive::Refused) {
			peer.Grant.Close();
			if (RetainedBodyGrantState) RetainedBodyGrantState->Drop(client);
			return true;
		}
		for (const auto &frame : peer.Stream.Take()) {
			const auto status =
				frame.Kind == PresentationStreamKind::Directory ? peer.Grant.Apply(frame.Directory)
				: frame.Kind == PresentationStreamKind::Message ? peer.Grant.Accept(frame.Message)
																: PresentationStatus::WrongHost;
			if (status == PresentationStatus::Ok || status == PresentationStatus::StaleEndpoint) continue;
			engine::core::Metrics::Count("server.presentation.refused", 1);
			if (frame.Kind == PresentationStreamKind::Message && status != PresentationStatus::WrongHost &&
				status != PresentationStatus::Invalid)
				continue;
			peer.Stream.Close();
			peer.Grant.Close();
			if (RetainedBodyGrantState) RetainedBodyGrantState->Drop(client);
			break;
		}
		return true;
	}
	bool Server::RetainedBodyAuthorized(
		const engine::world::PresentationAddress &requester, std::string_view player
	) {
		if (!RetainedBodyGrantState || !Replication) return false;
		PruneRetainedBodyGrants();
		for (const auto &entry : PlayerPresentations) {
			const auto &presentation = *entry;
			if (!presentation.Grant.OwnsReceipt(requester)) continue;
			return RetainedBodyGrantState->Authorizes(
				presentation.Client,
				presentation.Grant,
				requester,
				player,
				[this](const RetainedBodyGrants::Grant &grant) {
					const auto found = Players.find(grant.Client.Index);
					if (found == Players.end() || found->second.Generation != grant.Client.Generation)
						return false;
					bool current = false;
					Worlds().Enter(PrimaryWorld, [&](engine::ecs::Store &store) {
						if (engine::script::PortalTransferIncarnation(store) != grant.DestinationIncarnation)
							return;
						const auto committed = engine::script::PortalTransferPlayer(store, grant.Transfer);
						const auto *identity = store.Get<engine::scene::PlayerIdentity>(committed);
						current = committed != engine::ecs::NULL_ENTITY && committed == found->second.Instance &&
								  identity && identity->UserId == grant.UserId;
					});
					return current;
				}
			);
		}
		return false;
	}

	void Server::PumpPlayerPresentation(double now) {
		using namespace engine::world;
		if (!Replication) return;
		PruneRetainedBodyGrants();
		size_t queuedPackets = 0;
		for (const auto &entry : PlayerPresentations) {
			auto &peer = *entry;
			if (!peer.Stream.Open()) continue;
			const auto routes = peer.Grant.Routes();
			if (routes.Session != peer.PublishedSession || routes.Revision != peer.PublishedRevision) {
				PresentationStreamFrame frame;
				frame.Kind = PresentationStreamKind::Routes;
				frame.Directory = routes;
				if (peer.Stream.Queue(frame) == PresentationStatus::Ok) {
					peer.PublishedSession = routes.Session;
					peer.PublishedRevision = routes.Revision;
				}
			}
			for (auto &message : peer.Grant.Take()) {
				PresentationStreamFrame frame;
				frame.Message = std::move(message);
				if (peer.Stream.Queue(frame) != PresentationStatus::Ok)
					engine::core::Metrics::Count("server.presentation.reply-drops", 1);
			}
			queuedPackets +=
				peer.Stream.Flush([&](auto packet) { return Replication->SendTo(peer.Client, packet, now); });
		}
		if (queuedPackets != 0) {
			// Publish already flushed at the tick timestamp. Use elapsed wall time
			// for transport pacing before sleeping with newly queued image replies.
			const auto activeWires = Replication->Flush(engine::core::Clock::Seconds());
			engine::core::Metrics::Count("server.presentation.flushed-wires", activeWires);
		}
	}
	bool Server::BeginPresentationProducer() {
		if (Settings.PresentationProgram.empty()) return true;
		if (!Identity || !Socket || !Replication || Worlds().IsRemote(PrimaryWorld)) return false;
		if (ImageRelay || ImageProcess.Started()) return false;
		ImageRetryAt = 0;
		const auto entropy = network::SessionKey::Draw();
		const auto incarnation = network::SessionKey::Draw();
		if (!entropy || !incarnation) return false;
		const auto seed = entropy->Tag({});
		const auto identity = engine::assets::SigningKey::FromSeed(seed);
		if (!identity) return false;
		ImageIdentity = identity->Public();
		std::string key;
		key.reserve(seed.size() * 2);
		constexpr char HEX[] = "0123456789abcdef";
		for (const auto byte : seed) {
			const auto value = std::to_integer<unsigned>(byte);
			key.push_back(HEX[value >> 4]);
			key.push_back(HEX[value & 15]);
		}
		uint64_t session = 0;
		const auto sessionBytes = incarnation->Tag({});
		for (size_t i = 0; i < sizeof(session); ++i)
			session = (session << 8) | std::to_integer<unsigned>(sessionBytes[i]);
		session &= 0x7fffffffffffffffull;
		if (!session) session = 1;
		auto channel = engine::parallel::MakeProcessChannel();
		if (!channel.Valid()) {
			ImageIdentity.reset();
			return false;
		}
		std::vector<std::string> arguments{
			"--headless",
			"--frames",
			"-1",
			"--mcp-port",
			"-1",
			"--connect",
			engine::net::Endpoint::LoopbackIPv4(Socket->Local().Port).Text(),
			"--server-key",
			Identity->Public().ToHex(),
			"--play-key",
			key,
			"--presentation-world",
			std::string(Worlds().NameOf(PrimaryWorld).Text()),
			"--presentation-session",
			std::to_string(session)
		};
		if (!Settings.AssetsDirectory.empty())
			arguments.insert(
				arguments.end(), {"--override-assets-directory", Settings.AssetsDirectory.string()}
			);
		if (!ImageProcess.Start(Settings.PresentationProgram, arguments, std::move(channel.Remote))) {
			ENGINE_ERROR("could not start the configured portal image producer");
			ImageIdentity.reset();
			return false;
		}
		ImageLink = std::make_unique<engine::world::HostLink>(std::move(channel.Local));
		std::vector<std::string> channels{"portal-image-requests", "portal-topology-requests"};
		// The producer has sixteen bounded pending captures. Each can request
		// child images without sharing its parent's reply correlation channel.
		for (size_t slot = 0; slot < 16; ++slot)
			channels.push_back("portal-image-replies/nested/" + std::to_string(slot));
		ImageRelay = std::make_unique<engine::world::PresentationRelay>(
			Worlds(), PrimaryWorld, session, std::move(channels)
		);
		ImageStartedAt = engine::core::Clock::Seconds();
		engine::core::Metrics::Count("server.presentation.producer-starts", 1);
		return true;
	}

	void Server::PumpPresentationProducer() {
		if (!ImageRelay && ImageRetryAt == 0) return;
		// Endpoint creation and retirement require a closed frame, including process failure cleanup.
		if (Worlds().TickExchangeFrameOpen()) return;
		const double now = engine::core::Clock::Seconds();
		if (!ImageRelay && now < ImageRetryAt) return;
		ENGINE_PROFILE("server presentation producer");
		if (!ImageRelay) {
			engine::core::Metrics::Count("server.presentation.producer-retries", 1);
			if (BeginPresentationProducer()) return;
		} else {
			const auto process = ImageProcess.Poll();
			if (process.Alive() && ImageRelay->Pump(*ImageLink)) {
				if (now - ImageStartedAt >= 30) ImageRetryDelay = 1;
				return;
			}
			ENGINE_WARN(
				"portal image producer lost: world {} process {} code {} signal {} connected {} refused {} "
				"malformed {} dropped {}",
				Worlds().NameOf(PrimaryWorld).Text(),
				engine::parallel::Describe(process.Reason),
				process.Code,
				process.Signal,
				ImageLink->Connected(),
				ImageRelay->Refused(),
				ImageLink->Malformed(),
				ImageLink->Dropped()
			);
		}
		const double delay = ImageRetryDelay;
		StopPresentationProducer();
		ImageRetryAt = now + delay;
		ImageRetryDelay = std::min(delay * 2, 30.0);
		ENGINE_WARN("portal image producer ended; endpoints withdrawn, retrying in {}s", delay);
	}

	void Server::StopPresentationProducer() {
		ImageRelay.reset();
		if (ImageLink) ImageLink->Close();
		ImageLink.reset();
		ImageProcess = {};
		ImageIdentity.reset();
		ImageRetryAt = ImageStartedAt = 0;
		ImageRetryDelay = 1;
	}
}
