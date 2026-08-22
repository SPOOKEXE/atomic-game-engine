// Which stack a connection ends up on, and who decided.
//
// **The rule under test is that the server decides and the client has no
// vote.** A connector opens with QUIC whatever it was configured with; a
// listener that does not serve QUIC says so in one datagram; and the fallback
// that follows is the client's own, bounded, and logged.
//
// The three modes are `net::WireMode`'s three, and each is checked from the
// outside - what a peer that speaks the wrong stack gets back, and whether it
// then joins - rather than by reading a flag off either end.

#include "Wire.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/net/quic/Tls.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

TEST_SUITE_ID("engine.replication.negotiation")
TEST_DEPENDS("engine.replication.quicwire")
TEST_DEPENDS("engine.net.wire")

using engine::core::Name;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::net::WireKind;
using engine::net::WireMode;
using engine::replication::Admission;
using engine::replication::AdmissionKind;
using engine::replication::Connector;
using engine::replication::ConnectorSettings;
using engine::replication::Listener;
using engine::replication::ListenerSettings;

using namespace replication_wire;

namespace {
	// The seed a listener's identity is derived from. Stated rather than drawn,
	// so a failure reproduces from the file alone.
	std::array<std::byte, 32> Seed() {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index * 5 + 17);
		}
		return seed;
	}

	// A listener on one side of a loopback pair, and whatever connectors a case
	// puts on the other.
	//
	// **Neither end is pinned**, deliberately: what is under test is the
	// transport decision and not the identity check, and a pin that had to be
	// installed differently per wire would be a second thing each case gets
	// wrong.
	struct Field {
		std::vector<std::unique_ptr<Transport>> Ends;
		std::unique_ptr<Transport> ServerWire;
		std::unique_ptr<Transport> ClientWire;
		engine::ecs::Store World{"server"};
		engine::ecs::Store Replica{"client"};
		std::unique_ptr<Listener> Server;
		std::unique_ptr<Connector> Client;
		double Now = 0.0;

		explicit Field(WireMode mode, const LossSettings &clientLoss = {}) {
			RegisterTypes();
			Ends = MakeLoopbackTransport(2);
			REQUIRE(Ends.size() == 2);

			// Both ends wrapped whether or not they lose anything, for
			// `Wire.hpp`'s reason: a harness with the wrapper only on the cases
			// that use it is two harnesses.
			ServerWire = std::make_unique<LossyTransport>(std::move(Ends[0]));
			ClientWire = std::make_unique<LossyTransport>(std::move(Ends[1]), clientLoss);

			ListenerSettings serving;
			serving.Wire = mode;
			serving.Quic.Connection.Tls.Seed = Seed();
			serving.Quic.Connection.Tls.HasSeed = true;
			Server = std::make_unique<Listener>(*ServerWire, serving);
			Server->Authority().Replicate(Name("endtoend_test.Spot"));
		}

		void Connect(const ConnectorSettings &settings) {
			Client = std::make_unique<Connector>(*ClientWire, ServerWire->Local(), Now, settings);
		}

		// One barrier's worth, in the order a program runs them.
		void Tick(uint64_t tick) {
			Now += 1.0 / 60.0;
			if (Client != nullptr) {
				Client->Poll(Replica, Now);
			}
			Server->Poll(Now);
			Server->Publish(World, tick, Now);
			Server->Advance(Now);
			if (Client != nullptr) {
				Client->Poll(Replica, Now);
				Client->Advance(Now);
			}
		}

		bool Admit(uint64_t ticks = 400) {
			for (uint64_t tick = 1; tick <= ticks; ++tick) {
				if (Client->Admitted() || Client->Rejected()) {
					break;
				}
				Tick(tick);
			}
			// **Two more, because the two ends do not finish on the same tick.**
			// A TLS client is done when it has verified the server's Finished; a
			// server is done when the client's arrives, which is one flight
			// later.
			Tick(ticks + 1);
			Tick(ticks + 2);
			return Client->Admitted();
		}
	};

	// The first datagram a client on the old stack sends, framed as that stack
	// frames it.
	std::vector<std::byte> DatagramHello() {
		engine::replication::Hello hello;
		hello.PublicKey.fill(std::byte{0x11});

		engine::core::ByteWriter body;
		WriteAdmission(body, hello);

		engine::core::ByteWriter datagram;
		REQUIRE(engine::replication::FrameAdmission(datagram, body.Bytes()));
		return {datagram.Bytes().begin(), datagram.Bytes().end()};
	}

	// Whatever the listener sent back, parsed as an admission message.
	std::optional<Admission> AnswerTo(Field &field, const std::vector<std::byte> &sent) {
		field.ClientWire->Send(field.ServerWire->Local(), sent);
		field.Now += 1.0 / 60.0;
		field.Server->Poll(field.Now);

		std::vector<std::byte> reply;
		if (field.ClientWire->Receive(reply).Status != engine::net::TransportStatus::Ok) {
			return std::nullopt;
		}

		engine::core::ByteReader reader(reply);
		const std::optional<engine::net::Packet::Inbound> packet = engine::net::Packet::Read(reader);
		if (!packet.has_value()) {
			return std::nullopt;
		}

		engine::core::ByteReader body(packet->Payload);
		Admission message;
		if (!ReadAdmission(body, message)) {
			return std::nullopt;
		}
		return message;
	}
}

// --- quic only --------------------------------------------------------------

TEST_CASE("a quic-only server admits a quic client", "[replication][negotiation]") {
	Field field(WireMode::Quic);
	field.Connect({});
	REQUIRE(field.Admit());

	// **One attempt, because the first one is QUIC and there was nothing to
	// fall back from.**
	CHECK(field.Client->Wire() == WireKind::Quic);
	CHECK(field.Client->Attempts() == 1);
	CHECK(field.Server->Count() == 1);
	CHECK(field.Server->Stats().Mismatched == 0);
}

TEST_CASE("a quic-only server refuses a datagram hello by name", "[replication][negotiation]") {
	Field field(WireMode::Quic);
	const std::vector<std::byte> hello = DatagramHello();

	const std::optional<Admission> answer = AnswerTo(field, hello);
	REQUIRE(answer.has_value());
	CHECK(answer->Kind == AdmissionKind::Refuse);

	// Which wire it does serve, so a client tries the right one next rather
	// than the next one in a list.
	CHECK(answer->Refusal.Wire == WireKind::Quic);

	// Counted apart from `Refused`: this datagram was a perfectly good opening
	// message for the other stack, which is a different fix from a port
	// somebody is probing.
	CHECK(field.Server->Stats().Mismatched == 1);
	CHECK(field.Server->Stats().Refused == 0);
	CHECK(field.Server->Count() == 0);
}

TEST_CASE("a refusal is smaller than the hello that caused it", "[replication][negotiation]") {
	// `net/AGENTS.md`: a responder that answers a 35-byte hello with something
	// larger is a reflector somebody else's traffic can be bounced off, and the
	// amplification factor is the whole of what makes that worth doing.
	Field field(WireMode::Quic);
	const std::vector<std::byte> hello = DatagramHello();

	field.ClientWire->Send(field.ServerWire->Local(), hello);
	field.Server->Poll(field.Now);

	std::vector<std::byte> reply;
	REQUIRE(field.ClientWire->Receive(reply).Status == engine::net::TransportStatus::Ok);
	CHECK(reply.size() < hello.size());
}

// --- datagram only ----------------------------------------------------------

TEST_CASE("a datagram-only server refuses quic and the client joins anyway", "[replication][negotiation]") {
	Field field(WireMode::Datagram);
	field.Connect({});
	REQUIRE(field.Admit());

	// **The whole point of the refusal**: the client opened with QUIC, was told
	// no in one round trip, and joined on the other stack without anybody
	// configuring it.
	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 2);
	CHECK(field.Server->Count() == 1);
	CHECK(field.Server->Stats().Mismatched >= 1);
}

TEST_CASE("the refusal is what makes the fallback fast", "[replication][negotiation]") {
	// A deadline far longer than this case runs for. If the fallback waited for
	// it rather than acting on the refusal, nothing here would join.
	ConnectorSettings connecting;
	connecting.AttemptSeconds = 1000.0;

	Field field(WireMode::Datagram);
	field.Connect(connecting);
	REQUIRE(field.Admit(120));
	CHECK(field.Client->Wire() == WireKind::Datagram);
}

// --- both -------------------------------------------------------------------

TEST_CASE("a server serving both takes whichever arrives", "[replication][negotiation]") {
	Field field(WireMode::Both);

	// The one that opens with QUIC, which is every client with nothing to say
	// otherwise.
	field.Connect({});
	REQUIRE(field.Admit());
	CHECK(field.Client->Wire() == WireKind::Quic);

	// And one that heard a datagram-only advert and opened there. A server on
	// `Both` takes it without refusing anything.
	ConnectorSettings old;
	old.Advertised = WireMode::Datagram;
	Connector second(*field.ClientWire, field.ServerWire->Local(), field.Now, old);

	for (uint64_t tick = 1; tick <= 200 && !second.Admitted(); ++tick) {
		field.Now += 1.0 / 60.0;
		second.Poll(field.Replica, field.Now);
		field.Server->Poll(field.Now);
		field.Server->Advance(field.Now);
		second.Advance(field.Now);
	}

	CHECK(second.Admitted());
	CHECK(second.Wire() == WireKind::Datagram);
	CHECK(second.Attempts() == 1);
	CHECK(field.Server->Count() == 2);
	CHECK(field.Server->Stats().Mismatched == 0);
}

// --- what an advert saves ---------------------------------------------------

TEST_CASE("an advertised transport spares the refusal entirely", "[replication][negotiation]") {
	ConnectorSettings connecting;
	connecting.Advertised = WireMode::Datagram;

	Field field(WireMode::Datagram);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	// One attempt and no refusal at all, where the same client with no advert
	// pays one of each.
	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 1);
	CHECK(field.Server->Stats().Mismatched == 0);
}

TEST_CASE("an advert that says quic-only stops the client falling back", "[replication][negotiation]") {
	// Nothing is listening, so the QUIC attempt times out. A client told the
	// server serves only QUIC has nothing to fall back *to*, so it gives up
	// rather than spending a second deadline on a stack the server refuses.
	std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	ConnectorSettings connecting;
	connecting.Advertised = WireMode::Quic;
	connecting.AttemptSeconds = 0.1;

	engine::ecs::Store replica("client");
	Connector client(*ends[1], ends[0]->Local(), 0.0, connecting);

	double now = 0.0;
	for (int tick = 0; tick < 60 && !client.Rejected(); tick++) {
		now += 1.0 / 60.0;
		client.Poll(replica, now);
		client.Advance(now);
	}

	CHECK(client.Rejected());
	CHECK(client.Attempts() == 1);
}

TEST_CASE("a silent server costs two attempts and no more", "[replication][negotiation]") {
	std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 0.1;

	engine::ecs::Store replica("client");
	Connector client(*ends[1], ends[0]->Local(), 0.0, connecting);

	double now = 0.0;
	for (int tick = 0; tick < 120 && !client.Rejected(); tick++) {
		now += 1.0 / 60.0;
		client.Poll(replica, now);
		client.Advance(now);
	}

	// **Bounded, and the bound is two because there are two stacks.** A client
	// that kept cycling would be one that never reports a wrong address.
	CHECK(client.Rejected());
	CHECK(client.Attempts() == Connector::MAXIMUM_ATTEMPTS);
	CHECK(client.Wire() == WireKind::Datagram);
}

// --- over a link that loses things ------------------------------------------

TEST_CASE("a lost refusal costs the deadline and not the connection", "[replication][negotiation]") {
	// **The case the timeout exists for.** The refusal is one datagram and one
	// datagram can go missing, so a client that only ever fell back on an
	// explicit refusal would hang for ever on a link that dropped it. Nominated
	// by ordinal rather than by percentage: arrivals are counted from zero and
	// the first thing this client hears back is the refusal, so arrival zero is
	// exactly that datagram and nothing else.
	LossSettings lost;
	lost.Drop = {0};

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 0.25;

	Field field(WireMode::Datagram, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 2);
}

TEST_CASE("the fallback survives a link losing the handshake too", "[replication][negotiation]") {
	// The refusal and the first two answers the datagram exchange gets back.
	// What is being checked is that a fallback is an ordinary connection once it
	// has started, repaired by the retransmission that stack already has -
	// `ConnectorSettings::RepeatEverySeconds` is what drives it.
	LossSettings lost;
	lost.Drop = {0, 1, 2};

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 1.5;

	Field field(WireMode::Datagram, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());
	CHECK(field.Client->Wire() == WireKind::Datagram);
}

TEST_CASE("a quic server still admits over a link that loses the initial", "[replication][negotiation]") {
	LossSettings lost;
	lost.Drop = {1, 3};

	ConnectorSettings connecting;
	// Long enough that the QUIC handshake's own retransmission is what repairs
	// this, rather than the fallback quietly rescuing the case.
	connecting.AttemptSeconds = 30.0;

	Field field(WireMode::Quic, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	CHECK(field.Client->Wire() == WireKind::Quic);
	CHECK(field.Client->Attempts() == 1);
}
