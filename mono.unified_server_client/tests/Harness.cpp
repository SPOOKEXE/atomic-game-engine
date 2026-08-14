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

#include <engine/core/types/Color3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <cstdint>
#include <unified/Harness.hpp>
#include <vector>

TEST_SUITE_ID("unified.harness")
TEST_DEPENDS("client.replicated")
TEST_DEPENDS("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.wire")
TEST_DEPENDS("server.host")

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
	//
	// **The two gaps are compared by sign, not by coordinate**, because "ahead"
	// means along the direction of travel and the probe's velocity is drawn from
	// `core::Random`. Written as `DrawnX < ClientX` this case pinned the probe
	// happening to move in +X, and it went red the day the generator changed —
	// which said nothing about lag. Both gaps pointing the same way is the claim
	// that was always meant, and it holds whichever way the probe is going.
	const float networkLag = report.ServerX - report.ClientX;
	const float bufferLag = report.ClientX - report.DrawnX;
	REQUIRE(networkLag * bufferLag > 0.0f);
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

TEST_CASE("the world crosses quantised and the server's copy does not", "[unified]") {
	// **The whole of D00015(a), end to end, with the real components and no
	// network in the way.** `engine.replication.quantisation` proves the seam
	// with a component of its own and `engine.scene.wire` proves the grid; this
	// is the only place the two meet over `scene::Transform` itself, which is
	// the thing the change was for.
	//
	// Both halves are asserted, because each alone is satisfied by the bug the
	// other one catches: a client agreeing exactly would mean nothing was
	// quantised, and a server that had drifted onto the grid would mean the
	// authority had been quantised along with the wire.
	Settings settings;
	settings.Entities = 64;

	Harness harness(settings);
	REQUIRE(harness.Join());

	for (int step = 0; step < 30; step++) {
		harness.Step();
	}

	size_t compared = 0;
	size_t differing = 0;
	float worst = 0.0f;
	bool serverOffGrid = false;

	const float step =
		engine::scene::WIRE_POSITION_HALF_EXTENT_METRES / static_cast<float>(engine::scene::WIRE_STEPS);

	harness.ServerWorld().Each<const Transform>([&](engine::ecs::Entity entity,
													const Transform &authoritative) {
		const Transform *replicated = harness.ClientWorld().Get<Transform>(entity);
		if (replicated == nullptr) {
			return;
		}

		compared++;
		const float difference = std::abs(replicated->Frame.Position.X - authoritative.Frame.Position.X);
		worst = std::max(worst, difference);
		if (difference > 0.0f) {
			differing++;
		}

		// The server's own value sits between grid points, which it could
		// not do if anything had round-tripped it.
		const float offset = std::abs(std::remainder(authoritative.Frame.Position.X, step));
		if (offset > step * 0.1f) {
			serverOffGrid = true;
		}
	});

	REQUIRE(compared == 64);
	CHECK(worst <= engine::scene::WIRE_POSITION_ERROR_METRES);

	// And something really was rounded, or the bound above is a bound on
	// nothing happening.
	CHECK(differing > compared / 2);
	CHECK(serverOffGrid);
}

// --- what changes after the join, and what does not -------------------------
//
// **The gap these close is stated at the top of `Report::Drawn`**: `Bounds` and
// `Visual` used to cross once, in the join snapshot, and never again. They are
// not observed — nothing in this world resizes or recolours anything per tick,
// so a dirty column for them would be paid for every tick and read never — and
// a delta is built from those bits. So a part recoloured after the join kept its
// old colour on every client for ever, and nothing anywhere said so.
//
// `replication::ChangeDetection::Signature` is what closes it: the authority
// hashes the value itself once per `Publish` and sends what differs. These cases
// are here rather than in `engine.replication.*` because what they check is the
// pairing between a *world's* components and their detectors — which is this
// program's seam, not the protocol's.

namespace {
	using engine::scene::Bounds;
	using engine::scene::Visual;

	// Steps until a condition holds, so a case says how long it waited rather
	// than assuming a number.
	//
	// A value keeps being resent until the client acknowledges the tick it went
	// out on, so "it arrived" is a few ticks after "it changed" even with
	// nothing between the two halves.
	template <class Predicate> bool StepUntil(Harness &harness, int limit, Predicate held) {
		for (int step = 0; step < limit; step++) {
			harness.Step();
			if (held()) {
				return true;
			}
		}
		return false;
	}

	// The bytes the authority built on the last tick.
	size_t LastBytes(const Harness &harness) {
		return harness.Authority().Stats().Bytes;
	}

	// Steps until the stream settles, and returns what a quiet tick costs.
	//
	// **Not zero.** Every transform is marked changed every tick by
	// `Integrate`, so a quiet tick still carries the whole world's positions.
	// What it does not carry is a `Bounds` or a `Visual`, and that is the
	// difference the cases below measure against.
	size_t SettledBytes(Harness &harness) {
		size_t bytes = 0;
		size_t same = 0;

		// The join seeds every entity as owed its values, so the first ticks
		// after it carry them whether or not anything moved. Waiting for two
		// consecutive equal ticks is what says that has drained.
		for (int step = 0; step < 120 && same < 2; step++) {
			harness.Step();
			const size_t now = LastBytes(harness);
			same = now == bytes ? same + 1 : 0;
			bytes = now;
		}
		return bytes;
	}
}

TEST_CASE("a colour changed after the join reaches the client", "[unified]") {
	Harness harness(Small());
	REQUIRE(harness.Join());
	SettledBytes(harness);

	const engine::ecs::Entity probe = harness.Probe();
	REQUIRE(harness.ClientWorld().Get<Visual>(probe) != nullptr);

	const engine::core::Color3 wanted{0.9f, 0.1f, 0.2f};
	REQUIRE(harness.ClientWorld().Get<Visual>(probe)->Tint.R != Approx(wanted.R));

	// Written the way a script writes it. `Visual` is not observed here, so
	// this sets no dirty bit and no delta could have carried it before v0.7 —
	// the value is noticed because its hash differs, not because the write
	// announced itself.
	harness.ServerWorld().GetMutable<Visual>(probe)->Tint = wanted;

	const bool arrived = StepUntil(harness, 60, [&] {
		const Visual *replicated = harness.ClientWorld().Get<Visual>(probe);
		return replicated != nullptr && replicated->Tint.R == Approx(wanted.R) &&
			   replicated->Tint.G == Approx(wanted.G) && replicated->Tint.B == Approx(wanted.B);
	});

	CHECK(arrived);
}

TEST_CASE("a size changed after the join reaches what is drawn", "[unified]") {
	// `Bounds` is the other half of the pair, and it is the one whose loss is
	// visible: a client that received a position and no size has nothing to
	// draw, and one that received a *stale* size draws the wrong shape.
	Harness harness(Small());
	REQUIRE(harness.Join());
	SettledBytes(harness);

	const engine::ecs::Entity probe = harness.Probe();
	const engine::core::Vector3 wanted{4.0f, 0.25f, 4.0f};

	harness.ServerWorld().GetMutable<Bounds>(probe)->HalfExtent = wanted;

	const bool arrived = StepUntil(harness, 60, [&] {
		const Bounds *replicated = harness.ClientWorld().Get<Bounds>(probe);
		return replicated != nullptr && replicated->HalfExtent.X == Approx(wanted.X);
	});
	REQUIRE(arrived);

	// And it reached the draw list rather than only the store, which is the
	// half `Report::Drawn` exists to separate.
	const auto *drawList = harness.ClientWorld().Resource<client::DrawList>();
	REQUIRE(drawList != nullptr);

	bool drawn = false;
	for (const engine::scene::DrawInstance &instance : drawList->Instances) {
		if (instance.HalfExtent.X == Approx(wanted.X) && instance.HalfExtent.Y == Approx(wanted.Y)) {
			drawn = true;
		}
	}
	CHECK(drawn);
}

TEST_CASE("a value written in bulk is noticed", "[unified]") {
	// **The hole a dirty bit cannot cover, and the strongest reason a signature
	// exists.** `EachBatch` hands out raw column pointers and sets no bit —
	// deliberately, because checking per row is the cost that path exists to
	// avoid — so a system writing in bulk is invisible to `ChangeDetection::
	// Observed` however carefully it was observed. `ecs/ChangeChannel.hpp` says
	// so in its own words, and `scene::QuickHash` is the same answer one layer
	// up.
	Harness harness(Small());
	REQUIRE(harness.Join());
	SettledBytes(harness);

	const float wanted = 0.75f;
	harness.ServerWorld().EachBatch<Visual>([wanted](size_t rows, Visual *visuals) {
		for (size_t row = 0; row < rows; row++) {
			visuals[row].Transparency = wanted;
		}
	});

	const engine::ecs::Entity probe = harness.Probe();
	const bool arrived = StepUntil(harness, 90, [&] {
		const Visual *replicated = harness.ClientWorld().Get<Visual>(probe);
		return replicated != nullptr && replicated->Transparency == Approx(wanted);
	});
	REQUIRE(arrived);

	// Every row, not only the one the reports follow — a bulk write changed the
	// whole column and the detector has to have seen all of it.
	size_t matching = 0;
	size_t total = 0;
	harness.ServerWorld().Each<const Visual>([&](engine::ecs::Entity entity, const Visual &) {
		total++;
		const Visual *replicated = harness.ClientWorld().Get<Visual>(entity);
		if (replicated != nullptr && replicated->Transparency == Approx(wanted)) {
			matching++;
		}
	});
	CHECK(total == 16);
	CHECK(matching == total);
}

TEST_CASE("a component nothing wrote is not sent again", "[unified]") {
	// **The other half of the promise, and the one a naive fix breaks.** A
	// detector that resent every value every tick would close the gap above and
	// cost the whole world's colours per tick to do it — so "sends what
	// changed" has to mean "and nothing else".
	Harness harness(Small());
	REQUIRE(harness.Join());

	const size_t settled = SettledBytes(harness);
	REQUIRE(settled > 0);

	// A quiet tick stays quiet. Transforms are marked every tick by `Integrate`
	// so this is not zero, but it does not grow.
	harness.Step();
	CHECK(LastBytes(harness) == settled);

	// One colour changes, and the tick it lands on is bigger.
	harness.ServerWorld().GetMutable<Visual>(harness.Probe())->Tint = engine::core::Color3{0.1f, 0.9f, 0.4f};

	harness.Step();
	const size_t one = LastBytes(harness);
	CHECK(one > settled);

	// And it goes back down once the client has confirmed it, rather than the
	// value being offered for ever.
	CHECK(StepUntil(harness, 60, [&] { return LastBytes(harness) == settled; }));

	// **The assertion that a detector resending everything would fail**, and
	// the reason the two above are not enough on their own: both of them hold
	// just as well for an authority that puts every colour in every tick, which
	// closes the gap this feature exists for and pays the whole world's
	// bandwidth to do it. What separates the two is how much a tick grows when
	// *every* value changes — a real detector was carrying none of them and
	// grows by all sixteen, and a resend-everything one was already carrying
	// them and cannot grow at all.
	harness.ServerWorld().EachBatch<Visual>([](size_t rows, Visual *visuals) {
		for (size_t row = 0; row < rows; row++) {
			visuals[row].Tint = engine::core::Color3{0.2f, 0.3f, 0.4f};
		}
	});

	harness.Step();
	const size_t all = LastBytes(harness);

	// Sixteen entities' worth rather than one. Stated as a wide margin rather
	// than an exact figure: what is being asserted is that the cost scales with
	// what changed, not what one `Visual` happens to encode to today.
	CHECK(all > settled + (one - settled) * 8);
}

TEST_CASE("only the entity that changed is sent", "[unified]") {
	// Granularity, which is what makes a per-entity signature worth keeping
	// over a per-component one: a world where one part is recoloured must not
	// pay to resend the other fifteen.
	Harness harness(Small());
	REQUIRE(harness.Join());

	const size_t settled = SettledBytes(harness);

	// Two entities that are not the probe, so the case says something about
	// rows the reports do not follow.
	std::vector<engine::ecs::Entity> rows;
	harness.ServerWorld().Each<const Visual>([&](engine::ecs::Entity entity, const Visual &) {
		rows.push_back(entity);
	});
	REQUIRE(rows.size() == 16);

	const engine::ecs::Entity first = rows[3];
	const engine::ecs::Entity second = rows[9];

	harness.ServerWorld().GetMutable<Visual>(first)->Tint = engine::core::Color3{1.0f, 0.0f, 0.0f};
	harness.Step();
	const size_t one = LastBytes(harness);

	REQUIRE(StepUntil(harness, 60, [&] { return LastBytes(harness) == settled; }));

	// The second entity's value on the client, before anything touches it.
	const Visual before = *harness.ClientWorld().Get<Visual>(second);

	harness.ServerWorld().GetMutable<Visual>(second)->Tint = engine::core::Color3{0.0f, 0.0f, 1.0f};
	harness.Step();
	const size_t other = LastBytes(harness);

	// One entity's worth each time, not one and then two: the first change was
	// confirmed and forgotten before the second was made.
	CHECK(one == other);
	CHECK(one > settled);

	REQUIRE(StepUntil(harness, 60, [&] {
		const Visual *replicated = harness.ClientWorld().Get<Visual>(second);
		return replicated != nullptr && replicated->Tint.B == Approx(1.0f);
	}));

	// And the first entity kept the colour it was given rather than being
	// reverted by a delta that carried the whole column.
	CHECK(harness.ClientWorld().Get<Visual>(first)->Tint.R == Approx(1.0f));
	CHECK(before.Tint.B != Approx(1.0f));
}
