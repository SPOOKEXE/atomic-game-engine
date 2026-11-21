// One process being the authority for another.
//
// `replication/tests/EndToEnd.cpp` stands both halves up inside one process
// over a loopback transport. That is the right shape for a suite and it is not
// a running game: the two stores share an allocator, a component registry and a
// clock, and none of the things that only cross a process boundary are crossed.
// **This is the case the roadmap said did not exist yet** - a real server
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

#include "fixtures/ReplicationRemote.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("server.replication")
TEST_DEPENDS("engine.scene.registration")

using namespace server_replication_test;

TEST_CASE("a server admits only clients proving an allowed play identity", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	const engine::assets::SigningKey allowed = Identity(0x71);
	const engine::assets::SigningKey other = Identity(0x72);
	const std::vector<std::string> restriction{
		"--transport", "datagram", "--admit-key", allowed.Public().ToHex()
	};

	SECTION("the admitted identity joins") {
		Remote remote;
		REQUIRE(remote.Start(4, {}, restriction, &allowed));
		REQUIRE(remote.Join(400));
	}

	SECTION("a different valid identity is rejected") {
		Remote remote;
		REQUIRE(remote.Start(4, {}, restriction, &other));
		CHECK_FALSE(remote.Join(100));
		CHECK_FALSE(remote.Link->Joined());
		CHECK(remote.Entities() == 0);
	}

	SECTION("an anonymous client is rejected") {
		Remote remote;
		REQUIRE(remote.Start(4, {}, restriction));
		CHECK_FALSE(remote.Join(100));
		CHECK_FALSE(remote.Link->Joined());
		CHECK(remote.Entities() == 0);
	}
}

TEST_CASE("a client joins a separate server and receives its live Part stream", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(64));
	// Polling admits the client without submitting movement input.
	REQUIRE(remote.Join(400));

	// Check the Parts within the authored field: Workspace also owns a
	// Transform, and Velocity lives in each Part's attribute table.
	const Entity workspace = remote.World.FindFirstRoot("Workspace");
	const Entity field = remote.World.FindFirstChild(workspace, "ServerDefault");
	REQUIRE(field != engine::ecs::NULL_ENTITY);
	const Name velocity("Velocity");
	size_t cubes = 0;
	remote.World.EachChild(field, [&](Entity entity) {
		REQUIRE(remote.World.ClassOf(entity) == engine::scene::PartClass());
		REQUIRE(remote.World.Get<Transform>(entity) != nullptr);
		REQUIRE(remote.World.Get<Bounds>(entity) != nullptr);
		REQUIRE(remote.World.Get<Visual>(entity) != nullptr);
		engine::ecs::AttributeValue value;
		REQUIRE(engine::ecs::GetAttribute(remote.World, entity, velocity, value));
		REQUIRE(value.Type == engine::ecs::PropertyType::Vector3);
		cubes++;
	});
	REQUIRE(cubes == 64);
	REQUIRE(remote.Link->Applied() > 0);

	const uint64_t joinedAt = remote.Link->Applied();
	uint64_t latestObservedTick = 0;
	uint64_t latestDeltaTick = 0;
	size_t rejectedObservations = 0;
	std::string rejectionDetails;
	const auto collectObservations = [&] {
		while (const auto record = remote.Observations.Poll()) {
			if (const auto *applied =
					std::get_if<engine::replication::ReplicaAppliedObservation>(&record->Context)) {
				if (applied->Identity.TickAvailable) {
					latestObservedTick = std::max(latestObservedTick, applied->Identity.Tick);
					if (applied->Message == engine::replication::MessageKind::Delta)
						latestDeltaTick = std::max(latestDeltaTick, applied->Identity.Tick);
				}
			}
			if (const auto *rejected =
					std::get_if<engine::replication::ReplicaRejectedObservation>(&record->Context)) {
				if (rejectedObservations++ < 8) {
					rejectionDetails +=
						" kind=" + std::to_string(static_cast<int>(rejected->Message)) +
						" status=" + engine::replication::Describe(rejected->Status) + " tick=" +
						(rejected->Identity.TickAvailable ? std::to_string(rejected->Identity.Tick)
														  : "unknown");
				}
			}
		}
	};
	collectObservations();

	// A joined snapshot alone does not prove the live delta stream works.
	const bool streamed = remote.Wait(
		[&] {
			collectObservations();
			return remote.Link->Applied() > joinedAt && latestDeltaTick > joinedAt;
		},
		120
	);

	// LinkStats supports both QUIC and datagram connections.
	const engine::net::ConnectionStats link = remote.Link->LinkStats();
	const engine::parallel::ProcessStatus child = remote.Child.Poll();
	const auto &replica = remote.Link->ReplicaStats();
	INFO(
		"child=" << engine::parallel::Describe(child.Reason) << " code=" << child.Code
				 << " signal=" << child.Signal << " live=" << remote.Link->Live()
				 << " joined=" << remote.Link->Joined() << " latestObservedTick=" << latestObservedTick
				 << " latestDeltaTick=" << latestDeltaTick << " deltas=" << replica.Deltas
				 << " incomplete=" << replica.Incomplete << " partial=" << replica.Partial
				 << " stale=" << replica.Stale << " malformed=" << replica.Malformed
				 << " observationDrops=" << remote.Observations.Dropped()
				 << " rejectedObservations=" << rejectedObservations << rejectionDetails
	);
	INFO(
		"applied=" << remote.Link->Applied() << " joinedAt=" << joinedAt << " refused="
				   << remote.Link->Stats().Refused << " appliedMsgs=" << remote.Link->Stats().Applied
				   << " wire=" << engine::net::Describe(remote.Link->Wire())
				   << " attempts=" << remote.Link->Attempts() << " lost=" << link.PacketsLost
				   << " recv=" << link.PacketsReceived
	);
	REQUIRE(streamed);
	CHECK(remote.Link->Applied() > joinedAt);
	CHECK(latestDeltaTick > joinedAt);
}

TEST_CASE("a replication rate thins the stream without slowing the world", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **The one thing only a real server proves: that the rate reaches the
	// wire.** `engine.world.universe` proves the clock and `server.host` proves
	// the option reaches the world's settings; what neither can see is
	// `Server::ServeClients` skipping the publish and pumping the link instead.
	//
	// **Measured as the gap between applied ticks, never as snapshots per
	// wall-second.** A count over a wall-clock window is a test that fails on a
	// busy machine. The gap is the ratio between the two rates and does not
	// depend on how long anything took: at 60 Hz simulating and 5 Hz
	// publishing, each snapshot carries about twelve ticks of movement where an
	// ungated stream carries one.
	Remote remote;
	REQUIRE(remote.Start(32, {}, {"--replication-tick-rate", "5"}));

	// The join is the first half of the claim. Every message the server queues
	// waits for a published tick, so a client joining through a throttled
	// stream is exactly the case the `Flush`-instead-of-`Publish` split could
	// have broken - it would hang here rather than fail an assertion below.
	REQUIRE(remote.Join(600));

	uint64_t previous = remote.Link->Applied();
	uint64_t advanced = 0;
	int snapshots = 0;

	for (int tick = 0; tick < 900 && snapshots < 6; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));

		const uint64_t applied = remote.Link->Applied();
		if (applied > previous) {
			advanced += applied - previous;
			previous = applied;
			snapshots++;
		}
	}

	REQUIRE(snapshots >= 3);

	const double ticksPerSnapshot = static_cast<double>(advanced) / snapshots;
	INFO("advanced=" << advanced << " snapshots=" << snapshots << " ratio=" << ticksPerSnapshot);

	// **A floor of six against an expected twelve, and the slack only ever
	// helps.** A snapshot lost to the loopback widens the gap rather than
	// narrowing it, so the failures this can have are the ones worth having: an
	// ungated stream sits at one tick per snapshot and misses by a factor of
	// six.
	CHECK(ticksPerSnapshot >= 6.0);

	// And the world did not slow down with the stream. Sixty ticks a second
	// against three snapshots means the simulation is still ahead of what is
	// being sent, which is the whole point of separating the two rates.
	CHECK(advanced >= 24);
}

TEST_CASE("a world too big for one datagram still streams", "[server][replication][slow]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **The other regression, and the reason 32 entities was not enough to
	// catch it by accident.** A tick's delta for this many entities is many
	// times a datagram, and it used to be built as one message that
	// `Link::Reserve` refused every tick - silently, because a refusal is
	// ordinary backpressure. The join worked and the world then never moved.
	Remote remote;
	REQUIRE(remote.Start(2000));
	// The full scene includes Part geometry and per-Part Velocity attributes.
	// Concurrent dev checks can spend over ten seconds joining this scene.
	// Allow 24 seconds for the full blob, within the child server's 30-second life.
	const auto joiningSince = std::chrono::steady_clock::now();
	const bool joined = remote.Join(6000);
	const auto joiningMilliseconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - joiningSince)
			.count();
	const engine::net::ConnectionStats link = remote.Link->LinkStats();
	const auto &replica = remote.Link->ReplicaStats();
	INFO(
		"join_ms=" << joiningMilliseconds << " admitted=" << remote.Link->Admitted()
				   << " rejected=" << remote.Link->Rejected() << " bytes=" << link.BytesReceived
				   << " packets=" << link.PacketsReceived << " lost=" << link.PacketsLost
				   << " applied=" << remote.Link->Applied() << " snapshots=" << replica.Snapshots
				   << " prefaces=" << replica.Prefaces << " malformed=" << replica.Malformed
				   << " entities=" << remote.Entities()
	);
	REQUIRE(joined);
	const Entity workspace = remote.World.FindFirstRoot("Workspace");
	const Entity field = remote.World.FindFirstChild(workspace, "ServerDefault");
	REQUIRE(field != engine::ecs::NULL_ENTITY);

	const uint64_t joinedAt = remote.Link->Applied();
	for (int tick = 0; tick < 240 && remote.Link->Applied() == joinedAt; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(remote.Link->Applied() > joinedAt);

	// Every entity carries what it was sent, not merely the first datagram's
	// worth. A split that dropped its tail would show as a short count here and
	// as nothing at all anywhere else.
	size_t cubes = 0;
	size_t transformed = 0;
	remote.World.EachChild(field, [&](Entity entity) {
		if (remote.World.ClassOf(entity) != engine::scene::PartClass()) return;
		cubes++;
		if (remote.World.Get<Transform>(entity) != nullptr) transformed++;
	});
	REQUIRE(cubes == 2000);
	REQUIRE(transformed == cubes);
}

TEST_CASE("what ReplicatedFirst holds arrives before the world it covers", "[server][replication]") {
	// **`D00122`, and it is only observable over a wire.** `ReplicatedFirst`
	// outranking every distance score orders the *stream*, and a client's first
	// view of a world is the join - one `ecs::Store::Save` chunked across ticks
	// in whatever order the store wrote its archetypes, which no score reaches
	// inside. So a loading screen was ranked first for every tick after the one
	// it was for.
	//
	// **What is asserted is *before* and not *eventually*.** At the moment the
	// preface lands this replica holds the screen and holds almost nothing else;
	// a case that only waited for the screen to turn up would pass against the
	// single-blob join this exists to replace.
	//
	// Identified by its size rather than by its name, because a name does not
	// cross: `ecs.InstanceName` and the class are not in
	// `DefaultReplicatedComponents`, so what a replica holds of the screen is
	// its `scene::Bounds` and its frame. Nine studs is nothing else in the
	// scene.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_replicatedfirst.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local screen = Instance.new(\"Part\")\n"
			 << "screen.Name = \"LoadingScreen\"\n"
			 << "screen.Anchored = true\n"
			 << "screen.Size = Vector3.new(9, 9, 9)\n"
			 << "screen.Parent = game:GetService(\"ReplicatedFirst\")\n"
			 // A world whose own snapshot takes tens of ticks. Without that
			 // there is no window for anything to be first in, and the case
			 // would be measuring the packet rate.
			 << "for index = 1, 1500 do\n"
			 << "\tlocal block = Instance.new(\"Part\")\n"
			 << "\tblock.Anchored = true\n"
			 << "\tblock.Position = Vector3.new(index % 40, 1, index // 40)\n"
			 << "\tblock.Parent = workspace\n"
			 << "end\n";
	}

	const auto holdsScreen = [](Store &world) {
		bool found = false;
		world.EachEntity([&](Entity entity) {
			const Bounds *bounds = world.Get<Bounds>(entity);
			found = found || (bounds != nullptr && bounds->HalfExtent.X == 4.5f);
		});
		return found;
	};

	Remote remote;
	REQUIRE(remote.Start(0, scene.string()));

	int prefaces = 0;
	bool joinedThen = true;
	bool screenThen = false;
	size_t entitiesThen = 0;

	// **Sampled from inside the apply rather than between polls, and that is
	// the difference between a check and a coin toss.** One `Connector::Poll`
	// drains the socket and applies every message that was in it, so a machine
	// with no spare core hands the preface and the world behind it to the same
	// call - and this case used to read its four facts *after* that call, by
	// which time the window it is about had closed. It failed six times in six
	// under a saturating build and passed on an idle one, which is a case
	// measuring the scheduler rather than the stream.
	//
	// `Replica::OnPreface` runs before anything of the world has been applied,
	// so what the closure sees is the preface and nothing else by construction.
	remote.Link->OnPreface([&](Store &world) {
		prefaces++;
		joinedThen = remote.Link->Joined();
		screenThen = holdsScreen(world);

		entitiesThen = 0;
		world.EachEntity([&](Entity) { entitiesThen++; });
	});

	REQUIRE(remote.Join(1200));

	// Once. A second would mean the server re-declared a preface mid-join,
	// which would put a loading screen back over a world already being drawn.
	REQUIRE(prefaces == 1);

	INFO("the preface landed with " << entitiesThen << " entities in the replica");
	CHECK(screenThen);

	// The ordering the stage enum is for, read where it is decided. `Joined`
	// is set by the world blob and this runs on the preface, so a build where
	// this is true is one where the two blobs have been merged back into one.
	CHECK_FALSE(joinedThen);

	// The container and what is under it, and none of the fifteen hundred parts
	// it was meant to be in front of.
	CHECK(entitiesThen >= 2);
	CHECK(entitiesThen < 32);

	// And the world blob behind it is still the complete picture, whose
	// authoritative sweep must leave what the preface put there alone.
	CHECK(remote.World.CountMatching<Transform>() > 1500);
	CHECK(holdsScreen(remote.World));

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

TEST_CASE("a connecting client becomes a player in the hosted world", "[server][replication]") {
	// **The thing that had to exist before ownership could be assigned to
	// anybody.** `scene::AddPlayer` had no production caller: every world this
	// engine ran had a `Players` service with nobody in it, so a server script
	// asking for the player to hand a body to had nothing to be handed.
	//
	// Observed from a *second* client's replica rather than from the server's
	// log, because a log line is not a fact a test can hold: client one joins, is
	// counted, and then client two joins - and what client one's world gains is
	// exactly the one entity that is client two's player.
	//
	// The leaving half is `engine.scene.ownership`'s and is asserted there
	// directly. It is not asserted here because it would mean waiting out a link
	// timeout, which is a slow test measuring the drop interval rather than the
	// behaviour.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **A scene with a part in it, which is not incidental.** An empty world
	// replicates only services, whose archetypes are small enough that the two
	// processes happen to agree about column order. A `Part` is nine components
	// wide and is what caught `Archetype::Read` reading columns in the reader's
	// id order rather than the writer's - the client could not join at all, and
	// the same mismatch on a narrower row loads silently wrong.
	//
	// Anchored, so it stays where it was put and the entity count below is not
	// racing a body falling out of the world.
	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_players.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local block = Instance.new(\"Part\")\n"
			 << "block.Anchored = true\n"
			 << "block.Parent = workspace\n";
	}

	Remote first;
	REQUIRE(first.Start(0, scene.string()));
	REQUIRE(first.Join(400));
	Settle(first);

	const size_t alone = first.Entities();
	REQUIRE(alone > 0);

	// **Measured twice, so what is asserted is the rule and not the number.** A
	// `Player` is not one entity - `AddPlayer` furnishes it with a `PlayerGui`
	// - and pinning the count here would be pinning that arrangement from the
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
