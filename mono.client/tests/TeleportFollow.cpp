// A real client following a teleport from one server process to another.
//
// The server program hosts a source world and spawns a host for the
// destination. A script teleports the joining player; the client must drop
// its session, dial the destination, claim its player, and hold no more worlds
// than before.

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <client/Client.hpp>
#include <client/Options.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.teleport-follow")
TEST_DEPENDS("engine.scripthost.teleportrequest")
TEST_DEPENDS("server.teleport-handoff")
TEST_DEPENDS("engine.parallel.process")

namespace {
	using namespace engine;

	// Points assets at the build tree's own copy and puts the previous choice back.
	struct BuildAssets {
		std::filesystem::path Previous = core::Paths::Assets();
		BuildAssets() {
			core::Paths::SetAssetsOverride(core::Paths::Base());
		}
		~BuildAssets() {
			core::Paths::SetAssetsOverride(Previous);
		}
	};

	// Counts log lines containing one phrase while it is attached.
	struct LogWatch {
		std::atomic<int> Teleporting{0};
		std::atomic<int> Joined{0};
		spdlog::sink_ptr Sink;
		LogWatch() {
			Sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
				[this](const spdlog::details::log_msg &entry) {
					const std::string_view message(entry.payload.data(), entry.payload.size());
					if (message.find("client: teleporting to 'walk.destination'") != std::string_view::npos)
						Teleporting++;
					if (message.find("client: this is player") != std::string_view::npos) Joined++;
				}
			);
			core::Log::Logger().sinks().push_back(Sink);
		}
		~LogWatch() {
			std::erase(core::Log::Logger().sinks(), Sink);
		}
	};
}

TEST_CASE("[client] a teleport to another server moves the client there", "[client][teleport]") {
	const auto programs = core::Paths::Base().parent_path();
	const auto serverProgram = programs / "server" / core::Paths::Program("server");
	if (!std::filesystem::exists(serverProgram)) SKIP("the server program was not built");

	BuildAssets assets;
	const std::string sceneSource = R"(
if game.JobId ~= "server.world" then return end
local Players = game:GetService("Players")
local TeleportService = game:GetService("TeleportService")
Players.PlayerAdded:Connect(function(player)
	task.wait(0.5)
	TeleportService:Teleport("walk.destination", player, { Gold = 7 })
end)
)";
	scene::RegisterSceneClasses();
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
	const auto scenePath = core::Paths::Base() / "client-teleport-follow.agame";
	std::string saveError;
	REQUIRE(game::SaveGame(authored, core::Name("teleport follow"), scenePath, saveError));
	const auto configPath = core::Paths::Base() / "client-teleport-follow.ini";
	std::ofstream(configPath).close();
	const auto storeRoot = core::Paths::Base() / "client-teleport-follow-store";
	std::error_code ignored;
	std::filesystem::remove_all(storeRoot, ignored);

	auto reservation = net::MakeUdpTransport(0);
	REQUIRE(reservation);
	const auto port = reservation->Local().Port;
	reservation->Close();
	parallel::Process server;
	REQUIRE(server.Start(
		serverProgram,
		{"--listen",
		 std::to_string(port),
		 "--game",
		 scenePath.string(),
		 "--remote-world",
		 "walk.destination",
		 "--config",
		 configPath.string(),
		 "--datastore-root",
		 storeRoot.string(),
		 "--mcp-port",
		 "-1"}
	));

	LogWatch watch;
	client::Client player;
	client::Options options;
	options.Headless = true;
	options.Width = 96;
	options.Height = 64;
	options.Uncapped = true;
	options.MaximumFrameRate = 0;
	options.ConnectAddress = net::Endpoint::LoopbackIPv4(port).Text();
	options.ReconnectAttempts = 0;
	REQUIRE(player.Initialise(options));

	const auto until = [&](const auto &done, std::chrono::seconds limit) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!done() && std::chrono::steady_clock::now() < deadline)
			player.Step();
		return done();
	};

	REQUIRE(until(
		[&] { return player.Server() != nullptr && player.Server()->Joined(); }, std::chrono::seconds(30)
	));
	const size_t worlds = player.WorldCount();

	REQUIRE(until([&] { return watch.Teleporting.load() == 1; }, std::chrono::seconds(30)));
	// The claim answers with the destination's player, which is the second
	// "this is player" the client logs.
	CHECK(until(
		[&] { return watch.Joined.load() >= 2 && player.Server() != nullptr && player.Server()->Joined(); },
		std::chrono::seconds(30)
	));
	CHECK(player.WorldCount() == worlds);
	CHECK(watch.Teleporting.load() == 1);

	player.Shutdown();
}
