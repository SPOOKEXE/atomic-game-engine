// A teleport between two server processes, end to end.
//
// The source world teleports its player to a place another host holds. The
// destination rebuilds the player, answers over the world bus, and the source
// hands the client a token. A fresh connection to the destination claims the
// player with it, once.

#include <engine/assets/Signature.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <server/Server.hpp>
#include <thread>

TEST_SUITE_ID("server.teleport-handoff")
TEST_DEPENDS("server.host")

TEST_CASE("a teleport to another server hands the client over with a single-use token", "[server][teleport]") {
	using namespace engine;
	const auto program = core::Paths::Base().parent_path() / "server" / core::Paths::Program("server");
	if (!std::filesystem::exists(program)) SKIP("the server program was not built");

	const std::string sceneSource = R"(
if game.JobId ~= "server.world" then return end
local Players = game:GetService("Players")
local TeleportService = game:GetService("TeleportService")
local function mark(name, text)
	local value = Instance.new("StringValue")
	value.Name = name
	value.Value = text
	value.Parent = workspace
end
Players.PlayerAdded:Connect(function(player)
	mark("Added", player.Name)
	task.wait(0.25)
	TeleportService:Teleport("walk.destination", player, { Gold = 7 })
end)
TeleportService.Teleported:Connect(function(player, place)
	mark("Teleported", place)
end)
Players.PlayerRemoving:Connect(function(player)
	mark("Removed", player.Name)
end)
)";
	scene::RegisterSceneClasses();
	script::RegisterScriptComponents();
	world::Universe authored;
	for (const auto *name : {"server.world", "walk.destination"}) {
		const auto id = authored.Create({.Name = core::Name(name)});
		authored.Enter(id, [&](ecs::Store &store) {
			scene::InstallServices(store);
			script::SourceCache programs;
			programs.Set(core::Name("teleport.luau"), sceneSource);
			store.SetResource(programs);
			REQUIRE(store.SetParent(
				script::MakeScript(store, "teleport.luau", "Teleport"),
				store.FindFirstRoot("ServerScriptService")
			));
		});
	}
	const auto path = core::Paths::Base() / "teleport-handoff.agame";
	std::string saveError;
	REQUIRE(game::SaveGame(authored, core::Name("teleport handoff"), path, saveError));

	server::Options options;
	options.GamePath = path.string();
	options.HostProgram = program;
	options.RemoteWorlds = {"walk.destination"};
	options.Listening = true;
	options.DataStoreRoot = core::Paths::Base() / "teleport-handoff-store";
	std::error_code ignored;
	std::filesystem::remove_all(options.DataStoreRoot, ignored);
	options.MaximumTicks = 1;
	options.Unpaced = true;
	server::Server host;
	REQUIRE(host.Initialise(options));

	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed{};
	seed.fill(std::byte{53});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity);

	struct Peer {
		std::unique_ptr<net::Transport> Socket;
		std::unique_ptr<replication::Connector> Link;
		ecs::Store Replica{"teleport-handoff-replica"};
		std::vector<game::PortalSessionMessage> Replies;
		std::vector<game::TeleportHandoff> Handoffs;
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
			game::TeleportHandoff handoff;
			if (game::DecodePortalSession(bytes, message)) received->Replies.push_back(std::move(message));
			if (game::DecodeTeleportHandoff(bytes, handoff)) received->Handoffs.push_back(std::move(handoff));
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
		for (int tick = 0; tick < 600 && !peer.Link->Admitted(); ++tick)
			pump();
		REQUIRE(peer.Link->Admitted());
	};
	// Sends one admission message and waits for its `PortalSession` answer.
	const auto admit = [&](Peer &peer, std::vector<std::byte> request) {
		bool sent = false;
		for (int tick = 0; tick < 300 && !sent; ++tick) {
			sent = peer.Link->SendUser(request, now);
			pump();
		}
		REQUIRE(sent);
		for (int tick = 0; tick < 600 && peer.Replies.empty(); ++tick)
			pump();
		REQUIRE_FALSE(peer.Replies.empty());
		const auto reply = peer.Replies.back();
		peer.Replies.clear();
		return reply;
	};

	auto &source = connect(host.ListeningOn().Port);
	admitted(source);
	game::PortalSessionMessage fresh;
	fresh.Attempt = 1;
	const auto joined = admit(source, game::EncodePortalSession(fresh));
	REQUIRE(joined.Kind == game::PortalSessionKind::Ready);

	for (int tick = 0; tick < 3000 && source.Handoffs.empty(); ++tick)
		pump();
	REQUIRE(source.Handoffs.size() == 1);
	const auto handoff = source.Handoffs.front();
	CHECK(handoff.Place == "walk.destination");
	CHECK(handoff.Port != 0);
	CHECK(handoff.Port != host.ListeningOn().Port);
	// The player stays on the source until its client disconnects, and the
	// source has said where it went.
	const auto marks = [&](std::string_view name) {
		size_t count = 0;
		host.Enter([&](ecs::Store &store) {
			const ecs::Entity workspace = store.FindFirstRoot("Workspace");
			store.EachChild(workspace, [&](ecs::Entity child) {
				if (store.InstanceNameOf(child).Text() == name) count++;
			});
		});
		return count;
	};
	for (int tick = 0; tick < 300 && marks("Teleported") == 0; ++tick)
		pump();
	CHECK(marks("Teleported") == 1);
	CHECK(marks("Removed") == 0);
	REQUIRE(host.Enter([&](ecs::Store &store) { CHECK(scene::PlayerCount(store) == 1); }));

	auto &arrived = connect(handoff.Port, handoff.Identity);
	admitted(arrived);
	const auto claimed = admit(arrived, game::EncodeTeleportClaim({handoff.Token}));
	INFO(claimed.Diagnostic);
	REQUIRE(claimed.Kind == game::PortalSessionKind::Ready);
	CHECK(claimed.World == "walk.destination");

	// The arrival replicates to its claimant, carrying the teleport data.
	const auto carried = [&] {
		const ecs::Entity player = claimed.Player;
		return arrived.Replica.Alive(player) && arrived.Replica.FindFirstChild(player, "TeleportData") !=
													   ecs::NULL_ENTITY;
	};
	for (int tick = 0; tick < 600 && !carried(); ++tick)
		pump();
	CHECK(carried());
	CHECK(scene::PlayerCount(arrived.Replica) == 1);

	// The token is spent: a second claim of it is refused.
	auto &replay = connect(handoff.Port, handoff.Identity);
	admitted(replay);
	const auto refused = admit(replay, game::EncodeTeleportClaim({handoff.Token}));
	CHECK(refused.Kind == game::PortalSessionKind::Refused);

	// The client leaves the source the way a real one does, and only then is
	// the player removed there. Nothing fires a second PlayerAdded.
	source.Link->Disconnect(now);
	for (int tick = 0; tick < 600 && marks("Removed") == 0; ++tick)
		pump();
	CHECK(marks("Removed") == 1);
	CHECK(marks("Added") == 1);
	REQUIRE(host.Enter([&](ecs::Store &store) { CHECK(scene::PlayerCount(store) == 0); }));

	host.Shutdown();
}
