#include "Wire.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.replication.endtoend")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;

using namespace replication_wire;

TEST_CASE("a client joins over a real transport", "[replication]") {
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 30; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(wire.Join());

	for (int index = 0; index < 30; index++) {
		const Entity entity = entities[static_cast<size_t>(index)];
		REQUIRE(wire.Client.Alive(entity));
		REQUIRE(wire.Client.Get<Spot>(entity)->X == static_cast<float>(index));
	}
}

TEST_CASE("a snapshot crosses in chunks that each fit a datagram", "[replication]") {
	Wire wire;
	for (int index = 0; index < 600; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(wire.Join(1024));
	REQUIRE(wire.ServerSide->Stats().Undeliverable == 0);
	REQUIRE(wire.ServerSide->Stats().Refused == 0);
}

TEST_CASE("a snapshot chunk the link refuses is sent again", "[replication]") {
	engine::replication::SessionSettings session;
	session.Link.PacketsPerTick = 4;

	engine::replication::AuthoritySettings authority;
	authority.ChunksPerTick = 8;

	Wire wire(session, authority);
	for (int index = 0; index < 200; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(wire.Join(512));

	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget > 0);

	REQUIRE(wire.Client.CountMatching<Spot>() == 200);
}

TEST_CASE("a refused chunk rewinds its own cursor and not the other one", "[replication]") {
	// **`D00122`'s second cursor, re-earning the lesson the first one taught.**
	// A refused snapshot chunk is a permanent hole unless the sender is told,
	// because the cursor moves when the chunk is *built* - `applied=184
	// refused=17865`, which read like a protocol error and was a cursor. With a
	// join in two blobs there are two of them, and a refusal that rewound the
	// wrong one would leave the same hole with a second way to reach it.
	//
	// The link is deliberately too small for the tick, so the refusals are real
	// rather than arranged.
	engine::replication::SessionSettings session;
	session.Link.PacketsPerTick = 4;

	engine::replication::AuthoritySettings authority;
	authority.ChunkBytes = 256;
	authority.ChunksPerTick = 8;

	Wire wire(session, authority);

	// A preface of two hundred rows, which is many chunks at 256 bytes - a
	// preface that fits inside one tick's packet budget could not have a
	// refusal land inside it at all.
	std::vector<Entity> front;
	for (int index = 0; index < 200; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), -1.0f});
		front.push_back(entity);
	}
	for (int index = 0; index < 200; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	wire.Authority_.SetPreface([](Entity entity, const Store &store) {
		const Spot *spot = store.Get<Spot>(entity);
		return spot != nullptr && spot->Y < 0.0f;
	});

	size_t rowsWhenPrefaced = 0;
	bool prefaced = false;
	for (int tick = 0; tick < 1024 && !wire.Replica_.Joined(); tick++) {
		wire.Tick();
		if (!prefaced && wire.Replica_.Prefaced()) {
			prefaced = true;
			rowsWhenPrefaced = wire.Client.CountMatching<Spot>();
		}
	}

	REQUIRE(wire.Replica_.Joined());
	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget > 0);

	// Whole, and whole exactly once: a rewind of the wrong cursor shows up as a
	// blob assembled twice or as one that never completes.
	CHECK(wire.Replica_.Stats().Prefaces == 1);
	CHECK(rowsWhenPrefaced == front.size());
	CHECK(wire.Client.CountMatching<Spot>() == 400);
	for (const Entity entity : front) {
		REQUIRE(wire.Client.Get<Spot>(entity) != nullptr);
	}
}

TEST_CASE("the listener's own send loop hands refusals back", "[replication]") {
	// **The datagram stack, named rather than defaulted.** `ListenerSettings`
	// serves QUIC by default as of v0.19; this suite is about the framing,
	// budgets and reliability window of the other one, and `QuicWire.cpp` is
	// its QUIC twin.
	engine::replication::ListenerSettings serving;
	serving.Wire = engine::net::WireMode::Datagram;
	serving.Session.Link.PacketsPerTick = 4;
	serving.Authority.ChunksPerTick = 8;

	engine::replication::ConnectorSettings connecting;
	connecting.Advertised = engine::net::WireMode::Datagram;

	RegisterTypes();

	std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(2);
	REQUIRE(transports.size() == 2);

	Store world("server");
	world.Observe<Spot>();
	for (int index = 0; index < 200; index++) {
		world.Set<Spot>(world.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	double now = 0.0;
	engine::replication::Listener listener(*transports[0], serving);
	listener.Authority().Replicate(Name("endtoend_test.Spot"));

	Store replica("client");
	engine::replication::Connector connector(*transports[1], transports[0]->Local(), now, connecting);

	for (int tick = 1; tick <= 512 && !connector.Joined(); tick++) {
		now += 1.0 / 60.0;

		connector.Poll(replica, now);
		listener.Poll(now);
		listener.Publish(world, static_cast<uint64_t>(tick), now);
		listener.Advance(now);
		connector.Poll(replica, now);
		connector.Advance(now);
	}

	REQUIRE(connector.Joined());
	REQUIRE(replica.CountMatching<Spot>() == 200);
}

namespace {
	// Runs one listener and one connector to a join, then measures how many
	// bytes the authority *produced* on the busiest steady-state tick.
	//
	// **Produced rather than sent, which is the whole question.** A packer that
	// ignores what the path will carry does not send more - `Link::Reserve`
	// refuses the excess and `Unsent` rolls it back - it spends the encode and
	// then hands the shortfall to the transport instead of to the priority
	// scheduler. So what tells the two apart is what came out of `Publish`, and
	// `Authority::Statistics::Bytes` is reset per `Publish` for that reason.
	size_t BusiestTick(size_t linkBytesPerTick) {
		engine::replication::ListenerSettings serving;
		serving.Wire = engine::net::WireMode::Datagram;
		serving.Session.Link.BytesPerTick = static_cast<uint32_t>(linkBytesPerTick);

		RegisterTypes();

		std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(2);
		REQUIRE(transports.size() == 2);

		Store world("server");
		world.Observe<Spot>();

		std::vector<Entity> all;
		for (int index = 0; index < 600; index++) {
			const Entity entity = world.Create();
			world.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
			all.push_back(entity);
		}

		double now = 0.0;
		engine::replication::Listener listener(*transports[0], serving);
		listener.Authority().Replicate(Name("endtoend_test.Spot"));

		Store replica("client");
		engine::replication::ConnectorSettings connecting;
		connecting.Advertised = engine::net::WireMode::Datagram;
		engine::replication::Connector connector(*transports[1], transports[0]->Local(), now, connecting);

		uint64_t tick = 0;
		const auto step = [&]() {
			now += 1.0 / 60.0;
			tick++;

			for (const Entity entity : all) {
				world.GetMutable<Spot>(entity)->X = static_cast<float>(tick);
			}

			connector.Poll(replica, now);
			listener.Poll(now);
			listener.Publish(world, tick, now);
			listener.Advance(now);
			connector.Poll(replica, now);
			connector.Advance(now);
		};

		for (int attempt = 0; attempt < 2048 && !connector.Joined(); attempt++) {
			step();
		}
		REQUIRE(connector.Joined());

		// Measured after the join, because a snapshot chunk is not a delta and
		// the packer this is about only ever sees the delta.
		size_t busiest = 0;
		for (int round = 0; round < 120; round++) {
			step();
			busiest = std::max(busiest, static_cast<size_t>(listener.Authority().Stats().Bytes));
		}

		REQUIRE(replica.CountMatching<Spot>() == 600);
		return busiest;
	}
}

TEST_CASE("a link that says it will carry less makes the authority build less", "[replication]") {
	// **The two byte budgets stopped being two independent opinions.**
	// `AuthoritySettings::BytesPerTick` is a number somebody typed and
	// `ConnectionStats::SendAllowanceBytes` is what the congestion controller
	// measured, and packing past the second buys nothing: `Link::Reserve` turns
	// the excess away and `Unsent` rebuilds it next tick, so the encode is spent
	// and the shortfall reaches the transport rather than the priority
	// scheduler - which is the thing that exists to choose what a client sees
	// when not all of it fits.
	//
	// **Asserted as a comparison rather than against a constant**, because a
	// case that only checked the `min` is computed would pass against a wiring
	// that never calls `SetAllowance`: the authority's own ceiling would still
	// be in force and the number would look plausible. Two runs of one world,
	// differing only in what the *link* says it will carry.
	const size_t narrow = BusiestTick(2 * 1024);
	const size_t wide = BusiestTick(64 * 1024);

	// The authority's own ceiling is 32 KB in both runs, so a packer reading
	// only its own settings produces the same number twice.
	REQUIRE(narrow < wide);

	// And it respects the number rather than merely noticing it. The link's cap
	// bounds the allowance - `Link::ResetBudget` takes the lower of it and what
	// the controller worked out - so nothing may be built past it.
	REQUIRE(narrow <= 2 * 1024);

	// The wide run is genuinely over the narrow cap, or the comparison above is
	// a world too small to say anything.
	REQUIRE(wide > 2 * 1024);
}

TEST_CASE("a creation the link refused is announced again", "[replication]") {
	engine::replication::SessionSettings session;
	session.Link.PacketsPerTick = 2;

	Wire wire(session);
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{0.0f, 0.0f});
	REQUIRE(wire.Join(256));

	std::vector<Entity> late;
	for (int index = 0; index < 300; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 1.0f});
		late.push_back(entity);
	}

	size_t restarts = 0;
	for (int tick = 0; tick < 400; tick++) {
		wire.Tick();
		restarts += wire.Authority_.Stats().Resnapshots;
	}

	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget > 0);
	for (const Entity entity : late) {
		REQUIRE(wire.Client.Alive(entity));
	}

	REQUIRE(restarts == 0);
}

TEST_CASE("the refusal counter moves only when the link is really over", "[replication]") {
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(wire.Join());

	for (int round = 0; round < 30; round++) {
		for (const Entity entity : entities) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget == 0);
	REQUIRE(wire.Authority_.Stats().Deferred == 0);
}

TEST_CASE("movement streams as deltas after the join", "[replication]") {
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 16; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(wire.Join());

	for (int round = 1; round <= 20; round++) {
		for (const Entity entity : entities) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	for (const Entity entity : entities) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 20.0f);
	}
	REQUIRE(wire.Replica_.Stats().Deltas > 0);
}

TEST_CASE("a value no delta message can hold crosses as a staged overlay", "[replication]") {
	Wire wire;

	// **Observed rather than signed, which is the only pairing a value like this
	// can have.** A signature hashes the object representation, and this one is
	// a pointer into the heap.
	wire.Authority_.Replicate(
		engine::core::Name("endtoend_test.Bulk"), engine::replication::ChangeDetection::Observed
	);

	const Entity entity = wire.Server.Create();
	wire.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
	wire.Server.Set<Bulk>(entity, Bulk{std::string(4096, 'a')});

	REQUIRE(wire.Join());

	// The join carries it whatever its size: a snapshot is chunked, offset
	// addressed and reassembled, which is exactly what a delta message is not.
	REQUIRE(wire.Client.Get<Bulk>(entity) != nullptr);
	REQUIRE(wire.Client.Get<Bulk>(entity)->Text == std::string(4096, 'a'));

	const size_t oversizedAtJoin = wire.Authority_.Stats().Oversized;

	// **Now the case the delta path cannot serve.** `Pack` has no unit smaller
	// than a row, so this one goes into a message larger than a datagram, the
	// link refuses it, `Unsent` puts it back, and the same row is rebuilt and
	// refused every tick for the life of the connection. What happens instead is
	// a blob of just this entity, applied as an overlay.
	wire.Server.Set<Bulk>(entity, Bulk{std::string(4096, 'b')});

	for (int step = 0; step < 60; step++) {
		wire.Tick();
	}

	CHECK(wire.Client.Get<Bulk>(entity)->Text == std::string(4096, 'b'));
	CHECK(wire.Authority_.Stats().Oversized > oversizedAtJoin);

	// **And it stops.** The unconfirmed entry is erased when the row is turned
	// away, so nothing chases a value no message can carry - the first version
	// of this left the entry in place, and the client spent the rest of the
	// connection being re-staged once a tick and never told about anything else.
	const size_t settled = wire.Authority_.Stats().Oversized;
	for (int step = 0; step < 60; step++) {
		wire.Tick();
	}
	CHECK(wire.Authority_.Stats().Oversized == settled);

	// The rest of the world still moves, which is the property the stall broke.
	wire.Server.GetMutable<Spot>(entity)->X = 7.0f;
	for (int step = 0; step < 10; step++) {
		wire.Tick();
	}
	CHECK(wire.Client.Get<Spot>(entity)->X == 7.0f);
}

TEST_CASE("an input reaches the server over the reliable channel", "[replication]") {
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	engine::core::ByteWriter writer;
	WriteMessage(writer, engine::replication::Input{wire.Tick_, {}});
	REQUIRE(wire.ClientSide->Send(writer.Bytes(), wire.Now));

	wire.Tick();
	REQUIRE(wire.Authority_.Inputs(wire.Handle).size() >= 1);
}

TEST_CASE("the acknowledgement gets back and the server sees it", "[replication]") {
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	for (int round = 0; round < 4; round++) {
		wire.Tick();
	}

	const auto status = wire.Authority_.StatusOf(wire.Handle);
	REQUIRE(status.Applied > 0);
	REQUIRE(status.Applied <= wire.Replica_.Applied());
}

TEST_CASE("nothing malformed crosses a healthy link", "[replication]") {
	Wire wire;
	for (int index = 0; index < 40; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}
	REQUIRE(wire.Join());

	for (int round = 0; round < 30; round++) {
		wire.Tick();
	}

	REQUIRE(wire.Replica_.Stats().Malformed == 0);
	REQUIRE(wire.Authority_.Stats().Refused == 0);
	REQUIRE(wire.ServerSide->Stats().Refused == 0);
	REQUIRE(wire.ClientSide->Stats().Refused == 0);
}

TEST_CASE("a real session writes the round trip it measured", "[replication]") {
	Wire wire;
	REQUIRE(wire.Join());

	wire.ServerSide->Link().RecordRoundTrip(9.0);
	REQUIRE(wire.ServerSide->Link().Stats().RoundTripMilliseconds > 8000.0f);

	for (int step = 0; step < 8; step++) {
		wire.Tick();
	}

	const float trip = wire.ServerSide->Link().Stats().RoundTripMilliseconds;

	CHECK(trip < 200.0f);
	CHECK(trip >= 0.0f);
}
