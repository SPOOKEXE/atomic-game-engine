// One process being the authority for another.
//
// `replication/tests/EndToEnd.cpp` stands both halves up inside one process
// over a loopback transport. That is the right shape for a suite and it is not
// a running game: the two stores share an allocator, a component registry and a
// clock, and none of the things that only cross a process boundary are crossed.
// **This is the case the roadmap said did not exist yet** — a real server
// binary, started with `--listen`, streaming a world over a real UDP socket to
// something that has only ever seen its bytes.
//
// What that catches which the in-process suite cannot: a component name that
// resolves in one build and not the other, a snapshot that depends on
// registration order, a chunk that fits a loopback queue and not a datagram, and
// a flag that was parsed into a field nothing read.
//
// Skipped rather than failed when the server binary is not beside the test
// binary, for the reason `HostMode.cpp` gives: a preset that builds tests
// without the program is a legal configuration, and a suite that failed on it
// would be reporting the build rather than the code.

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

TEST_SUITE_ID("server.replication")
TEST_DEPENDS("engine.scene.registration")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::Endpoint;
using engine::net::MakeUdpTransport;
using engine::net::Transport;
using engine::parallel::Process;
using engine::replication::Connector;
using engine::scene::Bounds;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::scene::Visual;

namespace server_replication_test {
	// The components the server sends — `scene`'s, and the same call the server
	// itself makes.
	//
	// **This file used to declare its own `Position` and `Velocity` under the
	// server's wire names**, because a component crosses by name and there was
	// no shared set to name. That was a third copy beside the server's and the
	// client's, and it was worse than untidy: this binary already contained
	// `server::Position`, so registering a second type under the same string
	// aborted whenever both suites ran in one process. `testrunner` runs suites
	// separately and never saw it; `ctest` runs the binary whole and failed
	// every time. Registering the shared set is what removes the second
	// declaration rather than renaming around the collision.
	void RegisterTypes() {
		engine::scene::RegisterSceneComponents();
	}

	std::filesystem::path ServerProgram() {
		return engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	}

	bool ServerAvailable() {
		return std::filesystem::exists(ServerProgram());
	}

	// A port nothing is using, found by binding one and letting it go.
	//
	// There is a window between the release and the server's bind, and it is
	// accepted rather than closed: the alternative is the server reporting its
	// ephemeral port back over a pipe, which is a protocol invented for a test.
	// The caller retries, which turns a lost race into a slower test rather than
	// a failing one.
	uint16_t FreePort() {
		std::unique_ptr<Transport> probe = MakeUdpTransport(0);
		return probe == nullptr ? 0 : probe->Local().Port;
	}

	// A server process listening on a port, and a socket pointed at it.
	struct Remote {
		Process Child;
		std::unique_ptr<Transport> Socket;
		std::unique_ptr<Connector> Link;
		Store World{"replica"};
		double Now = 0.0;

		// The port the child bound, so a second client can be pointed at the
		// same server rather than starting one of its own.
		uint16_t Port = 0;

		bool Start(uint32_t entities, const std::string &game = {}) {
			RegisterTypes();

			for (int attempt = 0; attempt < 4 && !Child.Started(); attempt++) {
				const uint16_t port = FreePort();
				if (port == 0) {
					return false;
				}

				std::vector<std::string> arguments{
					"--listen",
					std::to_string(port),
					"--entities",
					std::to_string(entities),
					"--tick-rate",
					"60",
					"--seconds",
					"30",
				};
				if (!game.empty()) {
					arguments.emplace_back("--game");
					arguments.push_back(game);
				}

				if (!Child.Start(ServerProgram(), arguments)) {
					continue;
				}

				// The child may still exit immediately — the port went to
				// somebody else between the probe and the bind — so give it a
				// moment and check before committing to it.
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				if (!Child.Poll().Alive()) {
					Child = Process{};
					continue;
				}

				Socket = MakeUdpTransport(0);
				if (Socket == nullptr) {
					return false;
				}

				Port = port;
				Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now);
				return true;
			}

			return false;
		}

		// A second client on somebody else's server. No child process: this one
		// is a socket and a connector, which is what a second player is.
		bool Connect(uint16_t port) {
			RegisterTypes();
			Socket = MakeUdpTransport(0);
			if (Socket == nullptr) {
				return false;
			}
			Port = port;
			Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now);
			return true;
		}

		// One client tick: say something so the server knows where to send, then
		// take whatever came back.
		void Tick() {
			Now += 1.0 / 60.0;

			// **Deliberately nothing but `Poll`.** A server on a datagram socket
			// cannot stream to an address it has never heard from, so the client
			// has to speak first — and making the *test* speak first, with an
			// input the real client never sends, is how this suite passed while
			// the actual client sat silent and never joined. `Poll` announces
			// itself, and this is the case that says so.
			Link->Poll(World, Now);
			Link->Advance(Now);
		}

		bool Join(int ticks) {
			for (int attempt = 0; attempt < ticks && !Link->Joined(); attempt++) {
				Tick();

				// Real time, because the far side is a real process running at
				// its own rate rather than a function this loop calls.
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
			return Link->Joined();
		}

		size_t Entities() {
			size_t count = 0;
			World.EachEntity([&count](Entity) { count++; });
			return count;
		}

		~Remote() {
			Link.reset();
			if (Socket != nullptr) {
				Socket->Close();
			}
			if (Child.Started()) {
				Child.Kill();
			}
		}
	};
}

using namespace server_replication_test;

namespace server_replication_test {
	// Client ticks until whatever the server just did has reached this replica.
	// Real sleeps, because the far side is a real process running at its own
	// rate rather than a function this loop calls.
	void Settle(Remote &remote) {
		for (int tick = 0; tick < 200; tick++) {
			remote.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}
}

TEST_CASE("a client joins a server that is a separate process", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(64));

	// Sixteen hundred milliseconds of client ticks. Generous rather than tight:
	// the snapshot is spread across ticks on purpose, and a tight budget here
	// would make this suite fail on a loaded machine rather than on a bug.
	REQUIRE(remote.Join(400));

	// The world arrived, and it is the one that was asked for.
	REQUIRE(remote.Entities() == 64);
	REQUIRE(remote.Link->Applied() > 0);
}

TEST_CASE("the replicated world holds the components the server sent", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(32));
	REQUIRE(remote.Join(400));

	// Every replicated component resolved by name in a process that never saw
	// the server's header. A name that failed to resolve is a component that
	// silently does not arrive, which is why this is asserted rather than
	// assumed from the entity count.
	REQUIRE(remote.World.CountMatching<Transform>() == 32);
	REQUIRE(remote.World.CountMatching<Motion>() == 32);

	// The two that are sent once in the snapshot and never again, because
	// nothing in that world resizes or recolours anything. A client that
	// received a position and no size has a scene it cannot draw, and this is
	// the assertion that says they arrived rather than being assumed from the
	// two above.
	REQUIRE(remote.World.CountMatching<Bounds>() == 32);
	REQUIRE(remote.World.CountMatching<Visual>() == 32);
}

TEST_CASE("the world keeps moving after the join", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(32));
	REQUIRE(remote.Join(400));

	const uint64_t joinedAt = remote.Link->Applied();

	// A snapshot arriving and nothing after it is a join that worked and a
	// stream that did not, and the two look identical from the entity count
	// alone. What separates them is the applied tick advancing.
	for (int tick = 0; tick < 120 && remote.Link->Applied() == joinedAt; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	INFO(
		"applied=" << remote.Link->Applied() << " joinedAt=" << joinedAt << " refused="
				   << remote.Link->Stats().Refused << " appliedMsgs=" << remote.Link->Stats().Applied
				   << " linkState=" << static_cast<int>(remote.Link->Link().State())
				   << " stale=" << remote.Link->Link().Stats().PacketsStale
				   << " recv=" << remote.Link->Link().Stats().PacketsReceived
	);
	REQUIRE(remote.Link->Applied() > joinedAt);
}

TEST_CASE("a client announces itself without being told to", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// The regression this exists for: the connector only ever replied. On a
	// datagram socket a server cannot send to an address nobody has written
	// from, so a client that waited to be spoken to waited forever — and the
	// suite missed it by having the *test* send an input first, which the real
	// client never does. Nothing here calls `Submit`.
	Remote remote;
	REQUIRE(remote.Start(16));

	for (int attempt = 0; attempt < 400 && !remote.Link->Joined(); attempt++) {
		remote.Link->Poll(remote.World, remote.Now);
		remote.Link->Advance(remote.Now);
		remote.Now += 1.0 / 60.0;
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(remote.Link->Joined());
}

TEST_CASE("a world too big for one datagram still streams", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **The other regression, and the reason 32 entities was not enough to
	// catch it by accident.** A tick's delta for this many entities is many
	// times a datagram, and it used to be built as one message that
	// `Link::Reserve` refused every tick — silently, because a refusal is
	// ordinary backpressure. The join worked and the world then never moved.
	Remote remote;
	REQUIRE(remote.Start(2000));
	REQUIRE(remote.Join(1200));
	REQUIRE(remote.Entities() == 2000);

	const uint64_t joinedAt = remote.Link->Applied();
	for (int tick = 0; tick < 240 && remote.Link->Applied() == joinedAt; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(remote.Link->Applied() > joinedAt);

	// Every entity carries what it was sent, not merely the first datagram's
	// worth. A split that dropped its tail would show as a short count here and
	// as nothing at all anywhere else.
	REQUIRE(remote.World.CountMatching<Transform>() == 2000);
}

TEST_CASE("a connecting client becomes a player in the hosted world", "[server][replication]") {
	// **The thing that had to exist before ownership could be assigned to
	// anybody.** `scene::AddPlayer` had no production caller: every world this
	// engine ran had a `Players` service with nobody in it, so a server script
	// asking for the player to hand a body to had nothing to be handed.
	//
	// Observed from a *second* client's replica rather than from the server's
	// log, because a log line is not a fact a test can hold: client one joins, is
	// counted, and then client two joins — and what client one's world gains is
	// exactly the one entity that is client two's player.
	//
	// The leaving half is `engine.scene.ownership`'s and is asserted there
	// directly. It is not asserted here because it would mean waiting out a link
	// timeout, which is a slow test measuring the drop interval rather than the
	// behaviour.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// A scene with nothing in it. The services are still furnished — that is
	// `LoadScene`'s doing — so the world has a `Players` for people to arrive
	// in, and nothing else that could move underneath the count.
	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_players.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "-- deliberately empty: the fixtures are what this case is about\n";
	}

	Remote first;
	REQUIRE(first.Start(0, scene.string()));
	REQUIRE(first.Join(400));
	Settle(first);

	const size_t alone = first.Entities();
	REQUIRE(alone > 0);

	// **Measured twice, so what is asserted is the rule and not the number.** A
	// `Player` is not one entity — `AddPlayer` furnishes it with a `PlayerGui`
	// — and pinning the count here would be pinning that arrangement from the
	// far side of a wire. Two more clients say the useful thing instead: each
	// arrival costs the same, and it is not nothing.
	Remote second;
	REQUIRE(second.Connect(first.Port));
	REQUIRE(second.Join(400));
	Settle(first);
	const size_t withTwo = first.Entities();

	Remote third;
	REQUIRE(third.Connect(first.Port));
	REQUIRE(third.Join(400));
	Settle(first);
	const size_t withThree = first.Entities();

	CHECK(withTwo > alone);
	CHECK(withThree - withTwo == withTwo - alone);

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}
