// A server and a client in one process, with nothing between them.
//
// **These cases are about the seam, not about the protocol.** Everything below
// `Authority::Outgoing` is `engine.replication.*`'s to test and is tested there
// over a real loopback with real framing, real encryption and seeded loss. What
// is this program's own is that the two halves are joined correctly and that
// the report says which stage a failure is in — so a case here that started
// asserting things about deltas would be a copy of a suite that already exists.
//
// Headless, and one worker, so a run of this agrees with the run before it.

#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Demo.hpp>
#include <cstdint>
#include <unified/Harness.hpp>
#include <vector>

TEST_SUITE_ID("unified.harness")
TEST_DEPENDS("client.replicated")
TEST_DEPENDS("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("server.simulation")

using Catch::Approx;
using engine::scene::Transform;
using unified::Harness;
using unified::Report;
using unified::Settings;

namespace {
	Settings Small() {
		Settings settings;
		settings.Entities = 16;
		return settings;
	}
}

TEST_CASE("the world crosses with no network in the way", "[unified]") {
	// **The case the program exists for.** Every entity the server holds
	// reaches the client and is drawn, with no socket, no framing and no
	// cipher — so a client that draws nothing against a real server and draws
	// everything here has a problem in `net` or in the handshake, and one that
	// draws nothing here has a problem above them.
	Harness harness(Small());
	REQUIRE(harness.Join());

	const Report report = harness.Step();

	REQUIRE(report.ServerEntities == 16);
	REQUIRE(report.ClientEntities == report.ServerEntities);
	REQUIRE(report.Drawn == report.ClientEntities);
	REQUIRE(report.Applied > 0);
}

TEST_CASE("the drawn world moves between ticks and the store does not", "[unified]") {
	// The client half of `D00010`, measured end to end rather than against a
	// buffer driven by hand: four frames per received tick, and what is drawn
	// has to move on more than one of them.
	Harness harness(Small());
	REQUIRE(harness.Join());

	// Past the warm-up, where the tick rate is still a guess and the clock is
	// finding the delay.
	for (int step = 0; step < 90; step++) {
		harness.Step();
	}

	int moved = 0;
	int frames = 0;
	for (int step = 0; step < 30; step++) {
		const Report report = harness.Step();
		frames += 4;
		moved += 4 - report.FrozenFrames;

		// **Presentation only.** The store holds what the server sent, to the
		// bit, and never the interpolated value — the rule `world`'s
		// `ViewChannel` already follows, checked here because this is the one
		// place both numbers are in the same process at the same instant.
		REQUIRE(report.ClientX != Approx(report.DrawnX).epsilon(0.0));
	}

	// Every frame, not merely most of them. A world stepping once per tick
	// would move on one frame in four.
	REQUIRE(moved == frames);
}

TEST_CASE("the client is behind the server, and by the delay", "[unified]") {
	Harness harness(Small());
	REQUIRE(harness.Join());

	for (int step = 0; step < 150; step++) {
		harness.Step();
	}

	const Report report = harness.Step();

	// The three positions and the two lags between them. Server ahead of the
	// client's store is the round trip; the store ahead of what is drawn is the
	// jitter budget, and it is the one this feature bought.
	REQUIRE(report.DrawnX < report.ClientX);
	REQUIRE(report.Behind > 0.5);
}

TEST_CASE("a lost message is a stage the report names", "[unified]") {
	// **What the harness is for stated as a case.** A nominated message goes
	// missing and the authority is not told, which is loss rather than a
	// refusal — and the report says the tick was produced, says how much of it
	// was dropped, and shows the applied tick failing to keep up. None of those
	// three is visible from outside a process.
	Settings settings = Small();

	// The joining snapshot is the first few messages, so this lands well after
	// it and takes a delta rather than a chunk.
	settings.Drop = {40, 41, 42};

	Harness harness(settings);
	REQUIRE(harness.Join());

	uint64_t dropped = 0;
	for (int step = 0; step < 60; step++) {
		dropped += harness.Step().Dropped;
	}

	REQUIRE(dropped == 3);

	// And it recovered: a lost delta is a tick's worth of movement missing, not
	// a connection that stops.
	const Report report = harness.Step();
	REQUIRE(report.ClientEntities == report.ServerEntities);
	REQUIRE(report.Drawn == report.ClientEntities);
}

TEST_CASE("a tick would still fit in a datagram", "[unified]") {
	// **The one thing this harness can say about the wire without having
	// one.** There is no framing here, so a message near the limit crosses
	// happily — and this module has had four separate bugs from messages that
	// did not fit. `net::MAXIMUM_MESSAGE_BYTES` is 1159 and is what every
	// budget above `net` is sized against; a message produced here that is
	// larger is one a real link would refuse.
	Settings settings;
	settings.Entities = 512;

	Harness harness(settings);
	REQUIRE(harness.Join());

	size_t largest = 0;
	for (int step = 0; step < 40; step++) {
		largest = std::max(largest, harness.Step().LargestMessage);
	}

	REQUIRE(largest > 0);
	REQUIRE(largest <= 1159);
}

TEST_CASE("the probe is the same entity between two runs", "[unified]") {
	// The reports follow one entity, and a diagnostic that followed a different
	// one each run would make two runs incomparable — which is the whole point
	// of a deterministic harness.
	Harness first(Small());
	Harness second(Small());

	REQUIRE(first.Probe() == second.Probe());
	REQUIRE(first.Probe() != engine::ecs::NULL_ENTITY);

	REQUIRE(first.Join());
	REQUIRE(second.Join());

	for (int step = 0; step < 20; step++) {
		const Report left = first.Step();
		const Report right = second.Step();

		REQUIRE(left.Tick == right.Tick);
		REQUIRE(left.ServerX == right.ServerX);
		REQUIRE(left.DrawnX == right.DrawnX);
		REQUIRE(left.Bytes == right.Bytes);
	}
}
