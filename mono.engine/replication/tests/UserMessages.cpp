// Messages this module carries and does not read, over a real listener and a
// real connector.
//
// **The point of the seam is that it is the same link.** A connected, admitted,
// encrypted, reliable session between two processes is expensive to build and
// this module already has one - so what is checked here is that a caller with
// something else to say gets that link's guarantees without standing up a
// fourth session type beside it.
//
// Over the loopback with real encoding, like every other case in this module:
// the same `Session`, the same reliable channel, the same framing.
//
// **The datagram stack, named by every case here.** A listener serves QUIC by
// default as of v0.19, and what these cases reach for is that stack's own
// machinery: `net::Link`'s keep-alive, its acknowledgement window and its
// resend limit, none of which a QUIC session has. `QuicWire.cpp` carries the
// same seam over the other transport.

#include "Wire.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/net/Enums.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.replication.usermessages")
TEST_DEPENDS("engine.replication.endtoend")

using engine::core::Name;
using engine::ecs::Store;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::replication::ClientId;
using engine::replication::Connector;
using engine::replication::Listener;
using engine::replication::MessageKind;

using namespace replication_wire;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes;
		bytes.reserve(text.size());
		for (const char character : text) {
			bytes.push_back(static_cast<std::byte>(character));
		}
		return bytes;
	}

	std::string Text(std::span<const std::byte> bytes) {
		std::string text;
		text.reserve(bytes.size());
		for (const std::byte value : bytes) {
			text.push_back(static_cast<char>(value));
		}
		return text;
	}

	// A listener and one connector, driven to admission.
	//
	// Deliberately *not* to `Joined`: a user message must work as soon as the
	// link carries anything, and a suite that waited for a world to arrive
	// would be asserting on a dependency the feature does not have.
	struct Pair {
		std::vector<std::unique_ptr<Transport>> Transports;
		Store World{"server"};
		Store Replica{"client"};
		std::unique_ptr<Listener> Server;
		std::unique_ptr<Connector> Client;
		double Now = 0.0;

		Pair() {
			RegisterTypes();
			Transports = MakeLoopbackTransport(2);
			REQUIRE(Transports.size() == 2);

			engine::replication::ListenerSettings serving;
			serving.Wire = engine::net::WireMode::Datagram;
			Server = std::make_unique<Listener>(*Transports[0], serving);
			Server->Authority().Replicate(Name("endtoend_test.Spot"));

			engine::replication::ConnectorSettings connecting;
			connecting.Advertised = engine::net::WireMode::Datagram;
			Client = std::make_unique<Connector>(*Transports[1], Transports[0]->Local(), Now, connecting);
		}

		// One barrier's worth, in the order a program runs them.
		void Tick(uint64_t tick) {
			Now += 1.0 / 60.0;
			Client->Poll(Replica, Now);
			Server->Poll(Now);
			Server->Publish(World, tick, Now);
			Server->Advance(Now);
			Client->Poll(Replica, Now);
			Client->Advance(Now);
		}

		// Runs until the connector is in, or gives up.
		bool Admit() {
			for (uint64_t tick = 1; tick <= 128 && !Client->Admitted(); ++tick) {
				Tick(tick);
			}
			return Client->Admitted();
		}

		// **Several, and not one.** A reliable message is queued by `Send` and
		// put on the wire by the `Advance` at the end of the tick, so the
		// earliest the far end can see it is the *next* tick's poll. A suite
		// that pumped once would be asserting that a send is synchronous, which
		// it is not and must not become.
		void Settle(uint64_t from, int ticks = 4) {
			for (int step = 0; step < ticks; ++step) {
				Tick(from + static_cast<uint64_t>(step));
			}
		}
	};
}

TEST_CASE("a message crosses from a client to its server", "[replication][user]") {
	Pair pair;
	REQUIRE(pair.Admit());

	std::vector<std::string> heard;
	ClientId from;
	pair.Server->OnUserMessage([&](ClientId client, std::span<const std::byte> payload) {
		from = client;
		heard.push_back(Text(payload));
	});

	REQUIRE(pair.Client->SendUser(Bytes("an edit"), pair.Now));
	pair.Settle(200);

	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == "an edit");
	CHECK(from.IsValid());

	// **And the authority counted no refusal**, which is the half that would
	// have been missed by only checking the payload arrived: routing a user
	// message into `Authority::Receive` parses fine and then falls off the end
	// of the switch, so the message would look delivered and the counter would
	// climb.
	CHECK(pair.Server->Authority().Stats().Refused == 0);
}

TEST_CASE("a message crosses from a server to its client", "[replication][user]") {
	Pair pair;
	REQUIRE(pair.Admit());

	std::vector<std::string> heard;
	pair.Client->OnUserMessage([&](std::span<const std::byte> payload) { heard.push_back(Text(payload)); });

	// Whoever the listener admitted. One client, so the first submission's id
	// is the one to send to.
	ClientId who;
	pair.Server->OnUserMessage([&](ClientId client, std::span<const std::byte>) { who = client; });
	REQUIRE(pair.Client->SendUser(Bytes("hello"), pair.Now));
	pair.Settle(201);
	REQUIRE(who.IsValid());

	REQUIRE(pair.Server->SendTo(who, Bytes("a waypoint"), pair.Now));
	pair.Settle(210);

	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == "a waypoint");

	// The connector counted nothing it had to refuse, for the reason above: a
	// user message routed into the replica parses fine and then applies as
	// nothing, so the payload would look delivered while the counter climbed.
	CHECK(pair.Client->Stats().Refused == 0);
}

TEST_CASE("a message is reliable and ordered", "[replication][user]") {
	Pair pair;
	REQUIRE(pair.Admit());

	std::vector<std::string> heard;
	pair.Server->OnUserMessage([&](ClientId, std::span<const std::byte> payload) {
		heard.push_back(Text(payload));
	});

	// **The only promise worth making about a message nobody here
	// understands.** An unreliable one would be a recovery every caller has to
	// re-implement, and this module already has a reliable channel - so a
	// shared document does not need a sequence number of its own.
	for (int index = 0; index < 16; index++) {
		REQUIRE(pair.Client->SendUser(Bytes("edit " + std::to_string(index)), pair.Now));
	}
	for (uint64_t tick = 300; tick < 320; ++tick) {
		pair.Tick(tick);
	}

	REQUIRE(heard.size() == 16);
	for (int index = 0; index < 16; index++) {
		CHECK(heard[static_cast<size_t>(index)] == "edit " + std::to_string(index));
	}
}

TEST_CASE("a broadcast reaches everybody except who it came from", "[replication][user]") {
	RegisterTypes();

	// Three ends: a host and two guests, which is the smallest arrangement in
	// which 'except' means anything.
	std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(3);
	REQUIRE(transports.size() == 3);

	engine::replication::ListenerSettings serving;
	serving.Wire = engine::net::WireMode::Datagram;

	engine::replication::ConnectorSettings connecting;
	connecting.Advertised = engine::net::WireMode::Datagram;

	double now = 0.0;
	Store world("server");
	Listener host(*transports[0], serving);
	host.Authority().Replicate(Name("endtoend_test.Spot"));

	Store firstReplica("first");
	Store secondReplica("second");
	Connector first(*transports[1], transports[0]->Local(), now, connecting);
	Connector second(*transports[2], transports[0]->Local(), now, connecting);

	const auto tick = [&](uint64_t at) {
		now += 1.0 / 60.0;
		first.Poll(firstReplica, now);
		second.Poll(secondReplica, now);
		host.Poll(now);
		host.Publish(world, at, now);
		host.Advance(now);
		first.Poll(firstReplica, now);
		second.Poll(secondReplica, now);
		first.Advance(now);
		second.Advance(now);
	};

	for (uint64_t at = 1; at <= 128 && !(first.Admitted() && second.Admitted()); ++at) {
		tick(at);
	}
	REQUIRE(first.Admitted());
	REQUIRE(second.Admitted());

	std::vector<std::string> toFirst;
	std::vector<std::string> toSecond;
	first.OnUserMessage([&](std::span<const std::byte> payload) { toFirst.push_back(Text(payload)); });
	second.OnUserMessage([&](std::span<const std::byte> payload) { toSecond.push_back(Text(payload)); });

	ClientId sender;
	host.OnUserMessage([&](ClientId client, std::span<const std::byte> payload) {
		sender = client;
		// The relay a shared document is: one editor's change reaches everybody
		// else, and not the person who made it.
		host.Broadcast(payload, now, client);
	});

	REQUIRE(first.SendUser(Bytes("first's edit"), now));
	for (uint64_t at = 200; at < 216; ++at) {
		tick(at);
	}

	REQUIRE(sender.IsValid());

	// **The sender is not echoed to.** A host that sent it back would have them
	// apply their own change twice, which for a create is two instances.
	CHECK(toFirst.empty());
	REQUIRE(toSecond.size() == 1);
	CHECK(toSecond[0] == "first's edit");

	// And with nobody excepted, both hear it.
	CHECK(host.Broadcast(Bytes("from the host"), now) == 2);
	for (uint64_t at = 300; at < 316; ++at) {
		tick(at);
	}
	REQUIRE(toFirst.size() == 1);
	CHECK(toFirst[0] == "from the host");
	CHECK(toSecond.size() == 2);
}

TEST_CASE("a message before admission is refused rather than queued", "[replication][user]") {
	Pair pair;

	// There is no session to carry it on yet. An outbox here would hold
	// payloads this module is not allowed to understand, which is the same
	// reason `net` keeps none.
	CHECK_FALSE(pair.Client->SendUser(Bytes("too early"), pair.Now));

	REQUIRE(pair.Admit());
	CHECK(pair.Client->SendUser(Bytes("now"), pair.Now));
}

TEST_CASE("a send to a client that is not there answers false", "[replication][user]") {
	Pair pair;
	REQUIRE(pair.Admit());

	CHECK_FALSE(pair.Server->SendTo(ClientId{}, Bytes("nobody"), pair.Now));
	CHECK_FALSE(pair.Server->SendTo(ClientId{9999, 1}, Bytes("nobody"), pair.Now));
}

TEST_CASE("an unhooked user message is dropped rather than misread", "[replication][user]") {
	Pair pair;
	REQUIRE(pair.Admit());

	// No handler. The message still has to be routed away from the authority,
	// or it lands as a refusal on a counter an operator reads.
	REQUIRE(pair.Client->SendUser(Bytes("nobody is listening"), pair.Now));
	pair.Settle(400);

	CHECK(pair.Server->Authority().Stats().Refused == 0);
}

TEST_CASE("the kind can be read without parsing the rest", "[replication][user]") {
	engine::core::ByteWriter writer;
	engine::replication::User payload;
	payload.Bytes = Bytes("something");
	engine::replication::WriteMessage(writer, payload);

	const auto kind = engine::replication::PeekMessageKind(writer.Bytes());
	REQUIRE(kind.has_value());
	CHECK(*kind == MessageKind::User);

	// An input is not a user message, which is what makes the peek a router
	// rather than a formality.
	engine::core::ByteWriter other;
	engine::replication::Input input;
	input.Tick = 7;
	engine::replication::WriteMessage(other, input);
	REQUIRE(engine::replication::PeekMessageKind(other.Bytes()) == MessageKind::Input);

	// And rubbish is nothing at all rather than the first kind in the list.
	const std::vector<std::byte> rubbish(8, std::byte{0xAB});
	CHECK_FALSE(engine::replication::PeekMessageKind(rubbish).has_value());
	CHECK_FALSE(engine::replication::PeekMessageKind({}).has_value());
}

TEST_CASE("a user message round-trips through its own encoding", "[replication][user]") {
	engine::core::ByteWriter writer;
	engine::replication::User payload;
	payload.Bytes = Bytes("a waypoint's worth of commands");
	engine::replication::WriteMessage(writer, payload);

	engine::core::ByteReader reader(writer.Bytes());
	engine::replication::Message read;
	REQUIRE(engine::replication::ReadMessage(reader, read));
	CHECK(read.Kind == MessageKind::User);
	CHECK(Text(read.User.Bytes) == "a waypoint's worth of commands");

	// An empty payload is legal and stays empty. A caller that sends nothing
	// has said nothing, which is different from having said it badly.
	engine::core::ByteWriter empty;
	engine::replication::WriteMessage(empty, engine::replication::User{});
	engine::core::ByteReader back(empty.Bytes());
	engine::replication::Message nothing;
	REQUIRE(engine::replication::ReadMessage(back, nothing));
	CHECK(nothing.Kind == MessageKind::User);
	CHECK(nothing.User.Bytes.empty());
}

TEST_CASE("a quiet link still acknowledges, so its window never stalls", "[replication][user]") {
	// **The gap the studio's edit stream found, and it was invisible until
	// something went quiet.** An acknowledgement rides on an outgoing packet.
	// Every caller before v0.13 published a world every tick, so one always
	// went. A session that carries occasional messages and nothing else sends
	// nothing between them - and without a keep-alive the far side's reliable
	// window fills, its payloads are resent to the limit, and a link that is
	// working perfectly gives up.
	Pair pair;
	REQUIRE(pair.Admit());

	size_t heard = 0;
	pair.Server->OnUserMessage([&](ClientId, std::span<const std::byte>) { heard++; });

	// Nothing published, ever. Only the poll, the flush and the advance a
	// document session runs.
	const auto quiet = [&](int ticks) {
		for (int step = 0; step < ticks; ++step) {
			pair.Now += 1.0 / 60.0;
			pair.Client->Poll(pair.Replica, pair.Now);
			pair.Server->Poll(pair.Now);
			pair.Server->Flush(pair.Now);
			pair.Server->Advance(pair.Now);
			pair.Client->Poll(pair.Replica, pair.Now);
			pair.Client->Advance(pair.Now);
		}
	};

	// Well past the default keep-alive, and past the resend limit that would
	// have been reached if nothing acknowledged.
	for (int round = 0; round < 12; ++round) {
		INFO("round " << round << " state " << engine::net::Describe(pair.Client->Link()->State()));
		REQUIRE(pair.Client->SendUser(Bytes("edit " + std::to_string(round)), pair.Now));
		quiet(90);
	}

	CHECK(heard == 12);

	// The link is still up rather than timed out, which is the other half of
	// what a keep-alive is for.
	CHECK(pair.Client->Link()->State() == engine::net::ConnectionState::Connected);
}
