// What replication does when a datagram leaves and never arrives.
//
// **Until `net::LossyTransport` existed, nothing in this tree had ever lost a
// datagram in flight.** Every transport either delivers or refuses locally, so
// the whole of `docs/DEFERRED.md` D00011 was an argument rather than a failing
// test — and the entry itself said the cheap thing to do first was to build a
// link that drops. This is what that link found.
//
// Same harness as `EndToEnd.cpp`: a real loopback, real framing, real budgets.
// The only difference is that one nominated datagram is discarded on the way in,
// which is stated rather than waited for — no clock, no percentage, no flake.

#include "Wire.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.loss")
TEST_DEPENDS("engine.core.types.cframe")
TEST_DEPENDS("engine.net.lossytransport")
TEST_DEPENDS("engine.replication.endtoend")
TEST_DEPENDS("engine.replication.snapshotbuffer")

using engine::ecs::Entity;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::Transport;
using engine::replication::ClientId;
using namespace replication_wire;

TEST_CASE("a creation whose datagram is lost still reaches the client", "[replication][loss]") {
	// **D00011, and the reason it needed a lossy link to state.** A creation is
	// said exactly once and the server's known set moves when it is said, so a
	// lost one leaves the server certain the client holds an entity it has never
	// heard of. Nothing above notices: the client is not behind, it is
	// acknowledging happily, and `ResnapshotAfterTicks` is measured against
	// something that is not moving. Before structure went on the reliable
	// channel this case ran four hundred ticks and the entity was still absent.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	const Entity late = wire.Server.Create();
	wire.Server.Set<Spot>(late, Spot{9.0f, 9.0f});

	// The structural message goes first, so the next datagram to arrive is the
	// one carrying the creation.
	wire.ClientEnd->DropNext(1);
	wire.Tick();
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE_FALSE(wire.Client.Alive(late));

	// A still world. Nothing about this entity will ever be said again by the
	// delta path, which is exactly what made the loss permanent.
	for (int tick = 0; tick < 60; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Client.Alive(late));
	REQUIRE(wire.Client.Get<Spot>(late) != nullptr);
	REQUIRE(wire.Client.Get<Spot>(late)->X == 9.0f);

	// **Repaired by being told again, not by being sent the world again.** A
	// re-snapshot would also fix it, two seconds later and at the cost of the
	// whole world, and would look like a working system from every angle except
	// the bandwidth graph.
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);
	REQUIRE(wire.Replica_.Stats().Snapshots == 1);
}

TEST_CASE("a destruction whose datagram is lost still reaches the client", "[replication][loss]") {
	// The other half of the same shape, and the one that leaves an entity on
	// screen rather than off it. A client that never hears the destroy draws a
	// corpse for the life of the connection.
	Wire wire;
	const Entity doomed = wire.Server.Create();
	wire.Server.Set<Spot>(doomed, Spot{1.0f, 0.0f});
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{2.0f, 0.0f});
	REQUIRE(wire.Join());
	REQUIRE(wire.Client.Alive(doomed));

	wire.Server.Destroy(doomed);
	wire.ClientEnd->DropNext(1);
	wire.Tick();
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);

	for (int tick = 0; tick < 60; tick++) {
		wire.Tick();
	}

	REQUIRE_FALSE(wire.Client.Alive(doomed));
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);
}

TEST_CASE("a lost value repairs itself without being told again", "[replication][loss]") {
	// **The case D00011 contrasts a lost creation against, and it holds.** The
	// entity is still in the client's known set, so the unconfirmed entry added
	// at v0.3 offers the value again next tick and keeps offering it until the
	// client acknowledges a tick at or after the one it went out on. Nothing
	// structural is needed and nothing is re-snapshotted.
	Wire wire;
	const Entity moving = wire.Server.Create();
	wire.Server.Set<Spot>(moving, Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	wire.Server.GetMutable<Spot>(moving)->X = 42.0f;
	wire.ClientEnd->DropNext(1);
	wire.Tick();

	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(wire.Client.Get<Spot>(moving)->X == 1.0f);

	// And then it stops moving, which is the half that makes this a real case
	// rather than a coincidence: a value that changed again would arrive on its
	// own merits.
	for (int tick = 0; tick < 20; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Client.Get<Spot>(moving)->X == 42.0f);
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);
}

TEST_CASE("a tick whose rows have not arrived is not acknowledged", "[replication][loss]") {
	// **The rule that makes the repair above complete rather than half done.**
	// A creation and the values for the entity it creates go on different
	// channels, so the creation can be lost while the values arrive — and the
	// values are then dropped for want of the row. Acknowledging that tick
	// anyway told the server every value in it had landed, and the entity turned
	// up some ticks later holding none of its components. `Applied` is
	// documented as the last tick applied *in full*, and this is where that
	// becomes true.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	const uint64_t before = wire.Replica_.Applied();

	const Entity late = wire.Server.Create();
	wire.Server.Set<Spot>(late, Spot{5.0f, 6.0f});
	wire.ClientEnd->DropNext(1);
	wire.Tick();

	// The delta arrived, named a row the client does not hold, and said so.
	REQUIRE(wire.Replica_.Stats().Partial > 0);
	REQUIRE(wire.Replica_.Applied() == before);

	for (int tick = 0; tick < 60; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Client.Get<Spot>(late) != nullptr);
	REQUIRE(wire.Client.Get<Spot>(late)->Y == 6.0f);
	REQUIRE(wire.Replica_.Applied() > before);
}

TEST_CASE("a forget arrives even when its tick's delta got there first", "[replication][loss]") {
	// **Found by the sweep, and it needed no packet loss at all.** A forget was
	// refused when its tick was not newer than the last one applied — and the
	// delta for that same tick is sent first, so any tick that both moved
	// something and dropped something out of view had its forget discarded on
	// arrival. The client went on drawing an entity the server had already
	// removed from its known set, which is D00011's shape in the one path that
	// was supposed to be protected against it.
	//
	// The existing suites missed it because their forgets happen on ticks where
	// nothing else moved.
	Wire wire;
	std::vector<Entity> all;
	for (int index = 0; index < 4; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join());

	const Entity leaving = all[2];
	wire.Authority_.SetInterest([leaving](ClientId, Entity entity) { return entity.Id != leaving.Id; });

	// Everything moves on the same tick the entity leaves view, so the tick
	// carries a delta and a structural message together.
	for (const Entity entity : all) {
		wire.Server.GetMutable<Spot>(entity)->X += 1.0f;
	}
	wire.Tick();

	REQUIRE(wire.Replica_.Forgotten().size() == 1);
	REQUIRE(wire.Replica_.Forgotten()[0] == leaving);

	// Still there, because a forget is not a destroy.
	REQUIRE(wire.Client.Alive(leaving));
	REQUIRE(wire.Server.Alive(leaving));
}

TEST_CASE("a forget whose datagram is lost still reaches the client", "[replication][loss]") {
	Wire wire;
	const Entity watched = wire.Server.Create();
	wire.Server.Set<Spot>(watched, Spot{1.0f, 0.0f});
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{2.0f, 0.0f});
	REQUIRE(wire.Join());

	wire.Authority_.SetInterest([watched](ClientId, Entity entity) { return entity.Id != watched.Id; });

	wire.ClientEnd->DropNext(1);
	wire.Tick();
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(wire.Replica_.Forgotten().empty());

	for (int tick = 0; tick < 60; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Replica_.Forgotten().size() == 1);
	REQUIRE(wire.Replica_.Forgotten()[0] == watched);
}

TEST_CASE("a value lost from a tick that took several messages is repaired", "[replication][loss]") {
	// **D00013, and this case was written to fail.** A tick's delta is split
	// into as many independently applicable messages as it takes; the client
	// used to apply whichever arrived and acknowledge the tick anyway, and the
	// server then retired every value that tick carried — including the ones in
	// the message that never came. Measured here before the fix: eighteen of
	// forty entities stranded, and still eighteen forty ticks later.
	//
	// What closed it is a part number and a final marker on `Delta`, and a
	// client that names a tick in `Applied` only once it holds every part the
	// sender emitted. Nothing else moved: the unconfirmed-entry resend from v0.3
	// is what actually carries the value back, and it works because the tick it
	// was lost from is never acknowledged.
	//
	// Same world and the same nominated datagram as the case that pinned the
	// hole, so a failure here is the hole reopening rather than a different
	// world being measured.
	engine::replication::AuthoritySettings authority;
	authority.ChunkBytes = 256;

	Wire wire({}, authority);
	std::vector<Entity> all;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	const uint64_t before = wire.Replica_.Applied();

	for (const Entity entity : all) {
		wire.Server.GetMutable<Spot>(entity)->X = 99.0f;
	}
	wire.ClientEnd->DropNext(1);
	wire.Tick();

	const uint64_t lostAt = wire.Tick_;
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);

	// The tick really did take several messages, so this is the split case and
	// not a single-datagram tick that would have healed anyway.
	REQUIRE(wire.Replica_.Stats().Deltas > 1);

	// And it was not acknowledged, which is the whole of the fix. Every value
	// it carried is therefore still unconfirmed on the server.
	REQUIRE(wire.Replica_.Applied() == before);

	// **The bound, asserted rather than described: one tick.** The very next
	// tick re-offers everything that was still unconfirmed, arrives whole, and
	// is acknowledged — so a part lost for ever costs one tick of
	// acknowledgement and nothing more. Nothing waits for the datagram that
	// never came, because nothing can: the unreliable channel does not resend.
	wire.Tick();
	REQUIRE(wire.Replica_.Applied() == lostAt + 1);
	REQUIRE(wire.Replica_.Stats().Incomplete == 1);

	// Nothing moves again, so nothing but the unconfirmed set can repair these.
	for (int tick = 0; tick < 40; tick++) {
		wire.Tick();
	}

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 99.0f);
	}

	// **Repaired by being told again, not by being sent the world again**, and
	// exactly one tick was ever left short of a part.
	REQUIRE(wire.Replica_.Stats().Incomplete == 1);
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);
	REQUIRE(wire.Replica_.Stats().Snapshots == 1);
}

TEST_CASE("a tick the budget trimmed is complete rather than short of a part", "[replication][loss]") {
	// **The case most likely to be got wrong, and the reason the final marker is
	// authored by the sender when the tick is packed.** The per-client cap
	// deliberately holds values back, so a marker meaning "nothing else changed"
	// would be false on every tick of a world larger than its link — and a
	// client waiting for parts that were never emitted would stop acknowledging
	// on precisely the servers the cap exists for, then be re-snapshotted for
	// it. What was held over was never part of this tick: it keeps its
	// unconfirmed entry and comes back on a later one.
	//
	// No loss at all here. The only thing trimming these ticks is the budget.
	engine::replication::AuthoritySettings authority;
	authority.MessagesPerTick = 1;
	authority.ChunkBytes = 256;
	authority.StarvationTicks = 5;

	Wire wire({}, authority);
	std::vector<Entity> all;
	for (int index = 0; index < 30; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(4096));

	for (int round = 1; round <= 120; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = 7.0f;
		}
		wire.Tick();

		// Acknowledged, tick after tick, with a message limit of one against a
		// world that needs several.
		//
		// **From the first tick, and it used to take ten.** `net::Link` kept one
		// high-water sequence for the whole link rather than one per channel, so
		// the first unreliable deltas after a join that spent several reliable
		// packets were discarded as stale before anything here saw them. The
		// warm-up this case carried was that bug's fingerprint; asserting from
		// round one is what stops it coming back unnoticed.
		REQUIRE(wire.Replica_.Applied() == wire.Tick_);
	}

	// The budget really was short, and not one trimmed tick read as a tick with
	// a part missing.
	REQUIRE(wire.Authority_.Stats().Deferred > 0);
	REQUIRE(wire.Replica_.Stats().Incomplete == 0);
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 7.0f);
	}
}

TEST_CASE("a part lost from every tick costs the acknowledgement and nothing else", "[replication][loss]") {
	// **The give-up rule under sustained loss, and the half that says it does
	// not stall.** An incomplete tick is passed over rather than waited for, so
	// twenty ticks in a row losing a part cost twenty acknowledgements and no
	// values: everything each missing part carried is still unconfirmed and
	// rides the next tick. Waiting instead would be a stall no resend ends —
	// the unreliable channel does not redeliver — and the server would give up
	// on a client whose link is merely lossy.
	engine::replication::AuthoritySettings authority;
	authority.ChunkBytes = 256;

	Wire wire({}, authority);
	std::vector<Entity> all;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	// A value that moves once and then never again, which is exactly what
	// D00013 stranded, against a link losing one datagram of every tick.
	for (const Entity entity : all) {
		wire.Server.GetMutable<Spot>(entity)->X = 71.0f;
	}
	for (int tick = 0; tick < 20; tick++) {
		wire.ClientEnd->DropNext(1);
		wire.Tick();
	}

	REQUIRE(wire.ClientEnd->Stats().Dropped == 20);
	REQUIRE(wire.Replica_.Stats().Incomplete >= 15);

	// Well inside `ResnapshotAfterTicks`, so the server has not given up — the
	// client is a tick behind over and over, not adrift.
	REQUIRE(wire.Authority_.Stats().Resnapshots == 0);
	REQUIRE(wire.Replica_.Stats().Snapshots == 1);

	// And the moment the link stops losing things, one whole tick repairs the
	// lot: nothing was ever retired, because nothing was ever acknowledged.
	const uint64_t before = wire.Replica_.Applied();
	for (int tick = 0; tick < 10; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Replica_.Applied() > before);
	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 71.0f);
	}
}

TEST_CASE("a client that can never complete a tick is re-snapshotted", "[replication][loss]") {
	// **The bound on the wait, and it is deliberately the one that already
	// existed.** A client which cannot receive a whole tick cannot honestly
	// acknowledge one, so `Applied` stops moving — and `ResnapshotAfterTicks`
	// is this module's answer to a client that cannot be caught up by deltas it
	// never got. That is what makes the wait bounded rather than open-ended,
	// and it is the price of never acknowledging a tick a value is missing
	// from.
	//
	// Thirty ticks rather than the default hundred and twenty, stated here so
	// the case is short and so the number being asserted is visible.
	engine::replication::AuthoritySettings authority;
	authority.ChunkBytes = 256;
	authority.ResnapshotAfterTicks = 30;

	Wire wire({}, authority);
	std::vector<Entity> all;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	for (const Entity entity : all) {
		wire.Server.GetMutable<Spot>(entity)->X = 88.0f;
	}

	// One datagram of every single tick, for four times the bound.
	size_t restarts = 0;
	for (int tick = 0; tick < 120; tick++) {
		wire.ClientEnd->DropNext(1);
		wire.Tick();
		restarts += wire.Authority_.Stats().Resnapshots;
	}

	REQUIRE(wire.Replica_.Stats().Incomplete > 0);
	REQUIRE(restarts > 0);

	// Expensive and correct, which is the trade this bound makes. The link
	// stops losing things and the world is right — by the snapshot if not by the
	// deltas.
	for (int tick = 0; tick < 40; tick++) {
		wire.Tick();
	}
	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 88.0f);
	}
}

TEST_CASE("the snapshot buffer never holds a tick that was short of a part", "[replication][loss]") {
	// **What `client::RecordReplicatedTick` does, in the module that owns the
	// rule.** The buffer is fed with `Replica::Applied`, so a tick held back for
	// want of a part produces no pose at all — which is the only honest answer:
	// the store at that moment holds the rows the surviving parts carried and
	// the previous values of the rows the lost one did, and a pose recorded from
	// it would be interpolated through and then contradicted by the repair a
	// tick later.
	engine::replication::AuthoritySettings authority;
	authority.ChunkBytes = 256;

	Wire wire({}, authority);
	std::vector<Entity> all;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	// Exactly what the client's own seam does: record the applied tick, once,
	// after every poll. `Spot` stands in for a transform — what is being
	// asserted is which ticks reach the buffer at all.
	engine::replication::SnapshotBuffer buffer;
	const auto record = [&]() {
		const uint64_t applied = wire.Replica_.Applied();
		if (applied == 0 || buffer.Holds(applied)) {
			return;
		}
		for (const Entity entity : all) {
			buffer.Record(applied, entity, engine::core::CFrame());
		}
	};

	for (const Entity entity : all) {
		wire.Server.GetMutable<Spot>(entity)->X = 99.0f;
	}
	wire.ClientEnd->DropNext(1);
	wire.Tick();
	record();

	const uint64_t lostAt = wire.Tick_;
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(buffer.Newest() < lostAt);

	// And the tick after it is recorded, so the buffer stepped over the
	// incomplete tick rather than stopping at it.
	wire.Tick();
	record();
	REQUIRE(buffer.Newest() == lostAt + 1);
}

TEST_CASE("a join survives a link that is dropping datagrams", "[replication][loss]") {
	// The snapshot rides the reliable channel, so this is the path that was
	// meant to cope and had never been asked to. Stated by seed rather than by
	// percentage alone: the case is reproducible from the two numbers below.
	LossSettings toClient;
	toClient.LossChance = 0.5f;
	toClient.Seed = 4;

	LossSettings toServer;
	toServer.LossChance = 0.5f;
	toServer.Seed = 104;

	Wire wire({}, {}, toClient, toServer);
	for (int index = 0; index < 200; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(wire.Join(4096));
	REQUIRE(wire.Client.CountMatching<Spot>() == 200);

	// The link really did lose things, so the case is testing the recovery
	// rather than a run that happened to be clean.
	REQUIRE(wire.ClientEnd->Stats().Dropped > 0);
	REQUIRE(wire.ServerSide->Stats().Retransmissions > 0);
}

TEST_CASE("a handshake survives a link that is dropping datagrams", "[replication][loss]") {
	// The exchange rides the *unreliable* channel and the responder deliberately
	// keeps nothing to resend from, so the initiator's repeat timer is the only
	// thing covering a lost hello, cookie or welcome. Nothing had ever lost one.
	RegisterTypes();

	LossSettings toServer;
	toServer.LossChance = 0.4f;
	toServer.Seed = 1;

	LossSettings toClient;
	toClient.LossChance = 0.4f;
	toClient.Seed = 51;

	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);
	auto serverEnd = std::make_unique<LossyTransport>(std::move(ends[0]), toServer);
	auto clientEnd = std::make_unique<LossyTransport>(std::move(ends[1]), toClient);

	engine::ecs::Store world("server");
	world.Observe<Spot>();
	for (int index = 0; index < 64; index++) {
		world.Set<Spot>(world.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	double now = 0.0;
	engine::replication::Listener listener(*serverEnd, {});
	listener.Authority().Replicate(engine::core::Name("endtoend_test.Spot"));

	engine::ecs::Store replica("client");
	engine::replication::Connector connector(*clientEnd, serverEnd->Local(), now, {});

	for (int tick = 1; tick <= 2048 && !connector.Joined(); tick++) {
		now += 1.0 / 60.0;
		connector.Poll(replica, now);
		listener.Poll(now);
		listener.Publish(world, static_cast<uint64_t>(tick), now);
		listener.Advance(now);
		connector.Poll(replica, now);
		connector.Advance(now);
	}

	REQUIRE(connector.Joined());
	REQUIRE(replica.CountMatching<Spot>() == 64);
	REQUIRE(serverEnd->Stats().Dropped > 0);
	REQUIRE(clientEnd->Stats().Dropped > 0);
}

TEST_CASE("the priority rotation still drains under loss", "[replication][loss]") {
	// The rotation bounds how long a value waits when the budget is short. Loss
	// is the case it was never asked about: a value that goes out and does not
	// arrive keeps its unconfirmed entry, so it comes back round with a longer
	// wait behind it and must not be held off by an entity that keeps changing.
	engine::replication::AuthoritySettings authority;
	authority.MessagesPerTick = 1;
	authority.ChunkBytes = 256;
	authority.StarvationTicks = 5;

	LossSettings toClient;
	toClient.LossChance = 0.25f;
	toClient.Seed = 3;

	Wire wire({}, authority, toClient);
	std::vector<Entity> all;
	for (int index = 0; index < 30; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(4096));

	for (int round = 1; round <= 400; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = 7.0f;
		}
		wire.Tick();
	}

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 7.0f);
	}

	// The budget really was short and the link really did lose things.
	REQUIRE(wire.Authority_.Stats().Deferred > 0);
	REQUIRE(wire.ClientEnd->Stats().Dropped > 0);
}

TEST_CASE("duplicated and reordered datagrams change nothing", "[replication][loss]") {
	// The two things a routed network does besides losing, and the two the
	// sequence arithmetic and the reliable receiver exist for. A duplicate must
	// not be acted on twice and a late unreliable packet must not overwrite a
	// newer one.
	LossSettings toClient;
	toClient.Duplicate = {3, 4, 5, 6, 7, 8};
	toClient.Reorder = {9, 11, 13, 15};

	Wire wire({}, {}, toClient);
	std::vector<Entity> all;
	for (int index = 0; index < 20; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	for (int round = 1; round <= 60; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 60.0f);
	}
	REQUIRE(wire.ClientEnd->Stats().Duplicated > 0);
	REQUIRE(wire.ClientEnd->Stats().Reordered > 0);
	REQUIRE(wire.Replica_.Stats().Malformed == 0);
	REQUIRE(wire.Client.CountMatching<Spot>() == 20);
}

TEST_CASE("an entity coming into view brings its components with it", "[replication][loss]") {
	// **Found while mutation-testing the case above, and it needs no loss
	// either.** A delta is built from the dirty bits, and an entity entering a
	// client's interest has not moved — it was always there and this client
	// could not see it. So the entity was created on the client and then held
	// none of its components until something changed one, which for anything
	// stationary is never.
	Wire wire;
	const Entity always = wire.Server.Create();
	wire.Server.Set<Spot>(always, Spot{1.0f, 0.0f});

	const Entity sometimes = wire.Server.Create();
	wire.Server.Set<Spot>(sometimes, Spot{77.0f, 88.0f});

	bool visible = false;
	wire.Authority_.SetInterest([sometimes, &visible](ClientId, Entity entity) {
		return entity.Id != sometimes.Id || visible;
	});
	REQUIRE(wire.Join());
	REQUIRE_FALSE(wire.Client.Alive(sometimes));

	// It never moves. Only the fact that it became visible can carry it.
	visible = true;
	for (int tick = 0; tick < 10; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Client.Alive(sometimes));
	REQUIRE(wire.Client.Get<Spot>(sometimes) != nullptr);
	REQUIRE(wire.Client.Get<Spot>(sometimes)->X == 77.0f);
	REQUIRE(wire.Client.Get<Spot>(sometimes)->Y == 88.0f);
}

TEST_CASE("a lost destruction arrives even after later ticks have been applied", "[replication][loss]") {
	// **A structural message carries a tick and is deliberately not judged by
	// it.** The reliable channel resends it a hundred milliseconds later, which
	// is six ticks of a world that has gone on moving — so by the time it lands
	// the replica has applied ticks well past the one it names. Refusing it as
	// stale, the way a delta is rightly refused, is how a destroy never happens.
	Wire wire;
	const Entity moving = wire.Server.Create();
	wire.Server.Set<Spot>(moving, Spot{0.0f, 0.0f});
	const Entity doomed = wire.Server.Create();
	wire.Server.Set<Spot>(doomed, Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	// The destroy goes out on a tick that also moves something, and the world
	// keeps moving afterwards — so the replica's applied tick runs on past the
	// one the lost message names.
	wire.Server.Destroy(doomed);
	wire.Server.GetMutable<Spot>(moving)->X = 1.0f;
	wire.ClientEnd->DropNext(1);
	wire.Tick();
	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(wire.Client.Alive(doomed));

	const uint64_t lostAt = wire.Tick_;
	for (int round = 2; round <= 40; round++) {
		wire.Server.GetMutable<Spot>(moving)->X = static_cast<float>(round);
		wire.Tick();
	}

	REQUIRE(wire.Replica_.Applied() > lostAt);
	REQUIRE_FALSE(wire.Client.Alive(doomed));
}

TEST_CASE("a structural message is not an applied tick", "[replication][loss]") {
	// A structure and the values of the same tick travel on different channels,
	// so one can arrive without the other. Treating the structural half as
	// "tick applied" acknowledges values that never came, and the server retires
	// them — which strands anything that then stops moving.
	Wire wire;
	const Entity moving = wire.Server.Create();
	wire.Server.Set<Spot>(moving, Spot{0.0f, 0.0f});
	REQUIRE(wire.Join());

	// One tick carrying both: a creation, and a value for an entity that will
	// never move again.
	const Entity late = wire.Server.Create();
	wire.Server.Set<Spot>(late, Spot{5.0f, 0.0f});
	wire.Server.GetMutable<Spot>(moving)->X = 33.0f;

	// The second arrival, which is the delta behind the structure.
	wire.ClientEnd->DropAt(wire.ClientEnd->Arrived() + 1);
	wire.Tick();

	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(wire.Client.Alive(late));
	REQUIRE(wire.Client.Get<Spot>(moving)->X == 0.0f);

	for (int tick = 0; tick < 20; tick++) {
		wire.Tick();
	}

	REQUIRE(wire.Client.Get<Spot>(moving)->X == 33.0f);
	REQUIRE(wire.Client.Get<Spot>(late) != nullptr);
	REQUIRE(wire.Client.Get<Spot>(late)->X == 5.0f);
}

TEST_CASE("a world where only structure changes is not a client falling behind", "[replication][loss]") {
	// The same argument as the quiet world `Publish` already handles, one step
	// further along. A tick that carries only a structural message produces
	// nothing for the client to apply and therefore no new acknowledgement, so
	// counting it as a tick that streamed leaves the server permanently
	// convinced the client is behind — and re-snapshotting the whole world, over
	// and over, to repair a client that is in perfect agreement.
	Wire wire;
	const Entity first = wire.Server.Create();
	wire.Server.Set<Spot>(first, Spot{1.0f, 0.0f});
	const Entity second = wire.Server.Create();
	wire.Server.Set<Spot>(second, Spot{2.0f, 0.0f});
	REQUIRE(wire.Join());

	// Out of view, which is a structural message and nothing else: the world is
	// still, so there is no delta on that tick or any tick after it.
	wire.Authority_.SetInterest([second](ClientId, Entity entity) { return entity.Id != second.Id; });

	engine::replication::AuthoritySettings defaults;
	size_t restarts = 0;
	for (uint64_t tick = 0; tick < defaults.ResnapshotAfterTicks * 2; tick++) {
		wire.Tick();
		restarts += wire.Authority_.Stats().Resnapshots;
	}

	REQUIRE(wire.Replica_.Forgotten().size() == 1);
	REQUIRE(restarts == 0);
	REQUIRE(wire.Replica_.Stats().Snapshots == 1);
}

TEST_CASE("a budget holding everything back is not a client falling behind", "[replication][loss]") {
	// `Streamed` is the tick a delta's *values* last actually went out on, and
	// it is what the re-snapshot decision is measured against. A tick where the
	// per-client byte budget refused every message put nothing on the wire, so
	// moving it would leave the server certain a client that has been told
	// nothing is a client that has fallen behind — and re-snapshotting it, which
	// cannot help, because the whole world is far more than the budget that just
	// refused a single delta. `Statistics::Deferred` is the number that says
	// what is really happening.
	engine::replication::AuthoritySettings authority;
	authority.BytesPerTick = 1;

	Wire wire({}, authority);
	const Entity moving = wire.Server.Create();
	wire.Server.Set<Spot>(moving, Spot{0.0f, 0.0f});
	REQUIRE(wire.Join());

	size_t restarts = 0;
	for (uint64_t round = 1; round <= authority.ResnapshotAfterTicks * 2; round++) {
		wire.Server.GetMutable<Spot>(moving)->X = static_cast<float>(round);
		wire.Tick();
		restarts += wire.Authority_.Stats().Resnapshots;
	}

	// The budget really did refuse everything, so this is the held-back case
	// rather than a link that happened to have room.
	REQUIRE(wire.Authority_.Stats().Deferred > 0);
	REQUIRE(wire.Client.Get<Spot>(moving)->X == 0.0f);
	REQUIRE(restarts == 0);
	REQUIRE(wire.Replica_.Stats().Snapshots == 1);
}
