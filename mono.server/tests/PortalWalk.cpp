#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <server/Server.hpp>
#include <thread>

TEST_SUITE_ID("server.portalwalk")
TEST_DEPENDS("server.host")

TEST_CASE(
	"player walks between discovered product servers and returns with its transferred rig",
	"[server][portal-product-walk]"
) {
	using namespace engine;
	const double worldTickRate = GENERATE(30.0, 60.0);
	CAPTURE(worldTickRate);
	const auto program = core::Paths::Base().parent_path() / "server" / core::Paths::Program("server");
	if (!std::filesystem::exists(program)) SKIP("the server program was not built");
	const auto path = core::Paths::Base() / "portal-product-walk.agame";
	const std::string sceneSource = R"(
local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = Vector3.new(100, 2, 100)
floor.Position = Vector3.new(0, -1, 0)
floor.Parent = workspace
local pane = Instance.new("Part")
pane.Anchored = true
pane.Size = Vector3.new(10, 10, 0.4)
pane.Position = Vector3.new(0, 3, -3)
pane.Parent = workspace
local beyond = Instance.new("Part")
beyond.Anchored = true
beyond.CanCollide = false
beyond.Transparency = 1
beyond.Size = pane.Size
beyond.CFrame = CFrame.new(0, 3, -3.4) * CFrame.Angles(0, math.pi, 0)
beyond.Parent = workspace
local portal = Instance.new("Portal")
portal.Destination = beyond
portal.DestinationWorld = game.JobId == "walk.destination" and "server.world" or "walk.destination"
portal.Parent = pane
)";
	scene::RegisterSceneClasses();
	script::RegisterScriptComponents();
	world::Universe authored;
	for (const auto *name : {"server.world", "walk.destination"}) {
		const auto id = authored.Create({.Name = core::Name(name), .TickRate = worldTickRate});
		authored.Enter(id, [&](ecs::Store &store) {
			scene::InstallServices(store);
			script::SourceCache programs;
			programs.Set(core::Name("walk.luau"), sceneSource);
			store.SetResource(programs);
			REQUIRE(store.SetParent(
				script::MakeScript(store, "walk.luau", "Walk"), store.FindFirstRoot("ServerScriptService")
			));
		});
	}
	std::string saveError;
	REQUIRE(game::SaveGame(authored, core::Name("portal walk"), path, saveError));
	server::Options options;
	options.GamePath = path.string();
	options.HostProgram = program;
	options.RemoteWorlds = {"walk.destination"};
	options.Listening = true;
	options.Transport = GENERATE(net::WireMode::Datagram, net::WireMode::Quic);
	CAPTURE(options.Transport);
	options.MaximumTicks = 1;
	options.Unpaced = true;
	server::Server host;
	REQUIRE(host.Initialise(options));
	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed{};
	seed.fill(std::byte{31});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity);
	struct Peer {
		std::unique_ptr<net::Transport> Socket;
		std::unique_ptr<replication::Connector> Link;
		ecs::Store Replica{"portal-walk-replica"};
		std::vector<game::PortalSessionMessage> Replies;
	};
	std::vector<std::unique_ptr<Peer>> peers;
	double now = core::Clock::Seconds();
	const auto connect = [&](uint16_t port, std::optional<assets::PublicKey> pinned = {}) -> Peer & {
		auto peer = std::make_unique<Peer>();
		peer->Socket = net::MakeUdpTransport(0);
		REQUIRE(peer->Socket);
		replication::ConnectorSettings settings;
		settings.ClientIdentity = &*identity;
		settings.ServerIdentity = pinned;
		peer->Link = std::make_unique<replication::Connector>(
			*peer->Socket, net::Endpoint::LoopbackIPv4(port), now, settings
		);
		peer->Link->OnUserMessage([received = peer.get()](std::span<const std::byte> bytes) {
			game::PortalSessionMessage message;
			if (game::DecodePortalSession(bytes, message)) received->Replies.push_back(std::move(message));
		});
		peers.push_back(std::move(peer));
		return *peers.back();
	};
	const auto pump = [&] {
		now = core::Clock::Seconds();
		for (auto &peer : peers) {
			peer->Link->Poll(peer->Replica, now);
			peer->Link->Advance(now);
		}
		(void)host.Run();
		for (auto &peer : peers)
			peer->Link->Poll(peer->Replica, now);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	};
	const auto admitted = [&](Peer &peer) {
		for (int tick = 0; tick < 300 && !peer.Link->Admitted(); ++tick)
			pump();
		REQUIRE(peer.Link->Admitted());
	};
	const auto exchange =
		[&](Peer &peer, game::PortalSessionMessage request, game::PortalSessionKind expected) {
			const auto bytes = game::EncodePortalSession(request);
			bool sent = false;
			for (int tick = 0; tick < 300 && !sent; ++tick) {
				sent = peer.Link->SendUser(bytes, now);
				pump();
			}
			REQUIRE(sent);
			for (int tick = 0; tick < 300; ++tick) {
				for (const auto &reply : peer.Replies) {
					if (reply.Attempt != request.Attempt) continue;
					if (reply.Kind == game::PortalSessionKind::Refused) {
						INFO(reply.Diagnostic);
						REQUIRE(reply.Kind == expected);
					}
					if (reply.Kind == expected) return reply;
				}
				pump();
			}
			FAIL("product server did not answer the player admission request");
			return game::PortalSessionMessage{};
		};
	auto &source = connect(host.ListeningOn().Port);
	admitted(source);
	game::PortalSessionMessage fresh;
	fresh.Attempt = 1;
	const auto joined = exchange(source, fresh, game::PortalSessionKind::Ready);
	REQUIRE(host.Enter([&](ecs::Store &store) {
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, joined.Player));
		REQUIRE(rig);
		store.GetMutable<scene::Humanoid>(rig->Humanoid)->Health = 23;
	}));
	game::PortalSessionMessage offer;
	const auto walk = game::EncodeMoveInput({{0, 0, -1}, false});
	for (int tick = 0; tick < 300 && offer.Kind != game::PortalSessionKind::Transfer; ++tick) {
		(void)source.Link->Submit(source.Link->Applied(), walk, now);
		pump();
		for (const auto &reply : source.Replies)
			if (reply.Kind == game::PortalSessionKind::Transfer) offer = reply;
	}
	host.Enter([&](ecs::Store &store) {
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, joined.Player));
		REQUIRE(rig);
		const auto position = store.Get<scene::Transform>(rig->Root)->Frame.Position;
		const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
		const auto receipt = script::PortalTransferOfPlayer(store, joined.Player);
		CAPTURE(position.X, position.Y, position.Z, humanoid->MoveDirection.Z);
		INFO((receipt ? receipt->Diagnostic : "no transfer receipt"));
		const auto stage = receipt ? static_cast<int>(receipt->Stage) : -1;
		CAPTURE(stage);
		for (const auto &reply : source.Replies)
			if (reply.Kind == game::PortalSessionKind::Refused) FAIL(reply.Diagnostic);
		REQUIRE(offer.Kind == game::PortalSessionKind::Transfer);
	});
	CHECK(offer.Claim.Destination == "walk.destination");
	CHECK(offer.Port != 0);
	CHECK_FALSE(offer.Identity.IsZero());
	game::PortalSessionMessage proceed;
	proceed.Kind = game::PortalSessionKind::Proceed;
	proceed.Attempt = offer.Attempt;
	proceed.Claim = offer.Claim;
	auto &destination = connect(offer.Port, offer.Identity);
	admitted(destination);
	for (int tick = 0; tick < 300 && !destination.Link->Joined(); ++tick)
		pump();
	REQUIRE(destination.Link->Joined());
	(void)exchange(source, proceed, game::PortalSessionKind::Crossed);
	REQUIRE(host.Enter([&](ecs::Store &store) { CHECK_FALSE(store.Alive(joined.Player)); }));
	constexpr uint64_t retainedInputTick = 900000;
	REQUIRE(source.Link->Submit(retainedInputTick, game::EncodeMoveInput({{}, false}), now));
	bool stampAcknowledged = false;
	for (int tick = 0; tick < 300 && !stampAcknowledged; ++tick) {
		pump();
		host.Enter([&](ecs::Store &store) {
			const auto receipt = script::PortalTransferOfPlayer(store, joined.Player);
			stampAcknowledged =
				receipt && receipt->AcknowledgedInputTick == retainedInputTick && receipt->Motion &&
				receipt->Motion->InputTick == retainedInputTick &&
				receipt->Motion->DestinationIncarnation == offer.Claim.DestinationIncarnation &&
				receipt->Motion->DestinationTick != 0;
		});
	}
	REQUIRE(stampAcknowledged);
	const auto receivedMotion = [&] {
		return std::any_of(source.Replies.begin(), source.Replies.end(), [&](const auto &message) {
			return message.Kind == game::PortalSessionKind::Motion && message.Attempt == offer.Attempt &&
				   message.Claim == offer.Claim && message.Motion &&
				   message.Motion->InputTick == retainedInputTick;
		});
	};
	for (int tick = 0; tick < 300 && !receivedMotion(); ++tick)
		pump();
	REQUIRE(receivedMotion());

	game::PortalSessionMessage resume = proceed;
	resume.Kind = game::PortalSessionKind::Resume;
	const auto ready = exchange(destination, resume, game::PortalSessionKind::Ready);
	CHECK(ready.World == "walk.destination");
	for (int tick = 0;
		 tick < 300 &&
		 !destination.Replica.Has<scene::Character>(scene::CharacterOf(destination.Replica, ready.Player));
		 ++tick)
		pump();
	const auto *rig =
		destination.Replica.Get<scene::Character>(scene::CharacterOf(destination.Replica, ready.Player));
	REQUIRE(rig);
	const auto *humanoid = destination.Replica.Get<scene::Humanoid>(rig->Humanoid);
	REQUIRE(humanoid);
	CHECK(humanoid->Health == 23);
	resume.Kind = game::PortalSessionKind::Commit;
	const auto committed = exchange(destination, resume, game::PortalSessionKind::Committed);
	CHECK(committed.Player == ready.Player);
	CHECK(scene::PlayerCount(destination.Replica) == 1);
	game::PortalSessionMessage returnOffer;
	const auto returnWalk = game::EncodeMoveInput({{0, 0, 1}, false});
	for (int tick = 0; tick < 300 && returnOffer.Kind != game::PortalSessionKind::Transfer; ++tick) {
		(void)destination.Link->Submit(destination.Link->Applied(), returnWalk, now);
		pump();
		for (const auto &reply : destination.Replies) {
			if (reply.Kind == game::PortalSessionKind::Refused) FAIL(reply.Diagnostic);
			if (reply.Kind == game::PortalSessionKind::Transfer) returnOffer = reply;
		}
	}
	REQUIRE(returnOffer.Kind == game::PortalSessionKind::Transfer);
	CHECK(returnOffer.Claim.Destination == "server.world");
	CHECK(returnOffer.Port == host.ListeningOn().Port);
	proceed.Attempt = returnOffer.Attempt;
	proceed.Claim = returnOffer.Claim;
	auto &returned = connect(returnOffer.Port, returnOffer.Identity);
	admitted(returned);
	for (int tick = 0; tick < 300 && !returned.Link->Joined(); ++tick)
		pump();
	REQUIRE(returned.Link->Joined());
	(void)exchange(destination, proceed, game::PortalSessionKind::Crossed);
	resume = proceed;
	resume.Kind = game::PortalSessionKind::Resume;
	const auto returnedReady = exchange(returned, resume, game::PortalSessionKind::Ready);
	CHECK(returnedReady.World == "server.world");
	for (int tick = 0;
		 tick < 300 &&
		 !returned.Replica.Has<scene::Character>(scene::CharacterOf(returned.Replica, returnedReady.Player));
		 ++tick)
		pump();
	ecs::Entity authorityCharacter{};
	host.Enter([&](ecs::Store &store) {
		authorityCharacter = scene::CharacterOf(store, returnedReady.Player);
		REQUIRE(store.Has<scene::Character>(authorityCharacter));
	});
	const auto replicaCharacter = scene::CharacterOf(returned.Replica, returnedReady.Player);
	CAPTURE(
		authorityCharacter.Id,
		replicaCharacter.Id,
		returnedReady.Player.Id,
		returned.Replica.Alive(authorityCharacter),
		returned.Replica.ComponentsOf(authorityCharacter).size()
	);
	REQUIRE(returned.Replica.Has<scene::Character>(replicaCharacter));
	resume.Kind = game::PortalSessionKind::Commit;
	const auto returnedCommit = exchange(returned, resume, game::PortalSessionKind::Committed);
	CHECK(returnedCommit.Player == returnedReady.Player);
	for (int tick = 0; tick < 300 && destination.Replica.Alive(ready.Player); ++tick)
		pump();
	CHECK_FALSE(destination.Replica.Alive(ready.Player));
	CHECK(scene::PlayerCount(destination.Replica) == 0);
	REQUIRE(host.Enter([&](ecs::Store &store) {
		const auto *returnedRig =
			store.Get<scene::Character>(scene::CharacterOf(store, returnedReady.Player));
		REQUIRE(returnedRig);
		CHECK(store.Get<scene::Humanoid>(returnedRig->Humanoid)->Health == 23);
		CHECK(scene::PlayerCount(store) == 1);
	}));
	host.Shutdown();
}
