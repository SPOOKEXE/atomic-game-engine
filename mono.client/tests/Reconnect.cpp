// A real `client::Client` against an in-process listener, disconnected and
// redialled several times. Each cycle must hand back what the last one took:
// the replicated world, the server's slot, and the GPU memory its content used.

#include <engine/assets/Signature.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/Client.hpp>
#include <client/Options.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace {
	using namespace engine;

	// One authority world served over loopback UDP, stepped by hand.
	struct Authority {
		ecs::Store World{"reconnect.authority"};
		std::unique_ptr<net::Transport> Socket;
		std::optional<assets::SigningKey> Identity;
		std::unique_ptr<replication::Listener> Listener;
		uint64_t Tick = 0;

		Authority() {
			scene::EnsureClassTree();
			scene::RegisterSceneComponents();
			scene::RegisterSceneClasses();
			const ecs::Entity workspace = scene::InstallServices(World);
			REQUIRE(workspace != ecs::NULL_ENTITY);
			for (int index = 0; index < 32; index++) {
				scene::PartDesc part;
				part.Frame = core::CFrame(core::Vector3{float(index) * 3.0f, 0, -10});
				part.Size = {2, 2, 2};
				const ecs::Entity made = scene::MakePart(World, part);
				REQUIRE(made != ecs::NULL_ENTITY);
				REQUIRE(World.SetParent(made, workspace));
			}

			std::array<std::byte, assets::SigningKey::SEED_BYTES> seed{};
			seed.fill(std::byte{71});
			Identity = assets::SigningKey::FromSeed(seed);
			REQUIRE(Identity);

			Socket = net::MakeUdpTransport(0);
			REQUIRE(Socket);
			replication::ListenerSettings settings;
			settings.Quic.Connection.Tls.Seed = seed;
			settings.Quic.Connection.Tls.HasSeed = true;
			Listener = std::make_unique<replication::Listener>(*Socket, settings);
			Listener->SetIdentity(&*Identity);
			for (const auto &component : replication::DefaultReplicatedComponents()) {
				Listener->Authority().Replicate(core::Name(component.Name), component.Detection, component.Resource);
				if (!component.Suppressor.empty())
					Listener->Authority().SuppressWhenTagged(
						core::Name(component.Name), core::Name(component.Suppressor)
					);
			}
		}

		void Pump() {
			const double now = core::Clock::Seconds();
			Listener->Poll(now);
			Listener->Advance(now);
			Listener->ClearInputs();
			World.ClearChanges();
			Listener->Publish(World, ++Tick, now);
			Listener->Flush(now);
		}

		std::string Address() const {
			return net::Endpoint::LoopbackIPv4(Socket->Local().Port).Text();
		}
	};

	// Points assets at the build tree's own copy, which other suites may have
	// redirected, and puts the previous choice back.
	struct BuildAssets {
		std::filesystem::path Previous = core::Paths::Assets();
		BuildAssets() {
			core::Paths::SetAssetsOverride(core::Paths::Base());
		}
		~BuildAssets() {
			core::Paths::SetAssetsOverride(Previous);
		}
	};

	// Steps both sides until `done` holds, at most `steps` times.
	bool StepUntil(client::Client &player, Authority &server, const std::function<bool()> &done, int steps) {
		for (int step = 0; step < steps; step++) {
			if (done()) return true;
			server.Pump();
			player.Step();
		}
		return done();
	}
}

TEST_CASE("[client] repeated disconnects release the session each time", "[client][reconnect]") {
	BuildAssets assets;
	Authority server;
	client::Client player;
	client::Options options;
	options.Headless = true;
	options.Width = 96;
	options.Height = 64;
	options.Uncapped = true;
	options.MaximumFrameRate = 0;
	options.ConnectAddress = server.Address();
	options.ServerKey = server.Identity->Public().ToHex();
	options.ReconnectAttempts = 0;
	REQUIRE(player.Initialise(options));

	const auto joined = [&] { return player.Server() != nullptr && player.Server()->Joined(); };
	const auto released = [&] { return server.Listener->Count() == 0; };

	REQUIRE(StepUntil(player, server, joined, 2000));
	const size_t connectedWorlds = player.WorldCount();

	// The renderer fills its three retained scene frames lazily, one per cycle
	// here, so memory is compared only once that fixed ring is full.
	constexpr int WARM_CYCLES = 3;
	constexpr int CYCLES = WARM_CYCLES + 4;
	uint64_t settledGpuBytes = 0;
	int64_t settledHeapBytes = 0;
	for (int cycle = 0; cycle < CYCLES; cycle++) {
		INFO("cycle " << cycle);
		player.Disconnect();
		CHECK(player.Server() == nullptr);
		CHECK(player.WorldCount() == connectedWorlds - 1);
		// The QUIC goodbye frees the slot without waiting out the idle timeout.
		CHECK(StepUntil(player, server, released, 200));
		const auto gpu = player.GpuMemory();
		INFO(
			"buffers " << gpu.Buffers << "/" << gpu.BufferBytes << " transfer " << gpu.TransferBuffers << "/"
					   << gpu.TransferBufferBytes << " textures " << gpu.Textures << "/" << gpu.TextureBytes
		);
		const int64_t heapBytes = core::HeapProfile::Totals().LiveBytes;
		if (cycle == WARM_CYCLES - 1) {
			settledGpuBytes = gpu.LiveBytes;
			settledHeapBytes = heapBytes;
		} else if (cycle >= WARM_CYCLES) {
			CHECK(gpu.LiveBytes == settledGpuBytes);
			// Log and metric buffers wobble by a few blocks; a leaked world is megabytes.
			if (core::HeapProfile::IsCompiledIn()) CHECK(heapBytes - settledHeapBytes < 16 * 1024);
		}

		REQUIRE(player.Connect(server.Address()));
		REQUIRE(StepUntil(player, server, joined, 2000));
		CHECK(player.WorldCount() == connectedWorlds);
		CHECK(server.Listener->Count() == 1);
	}

	// Shutdown says goodbye too, so the server does not hold a dead slot.
	player.Shutdown();
	for (int step = 0; step < 200 && !released(); step++)
		server.Pump();
	CHECK(released());
}
