// A world replicated over QUIC, through the same listener and the same
// connector.
//
// **The point is that nothing above the session changed.** `Authority`,
// `Replica`, `Prediction` and every message format are the ones the datagram
// wire uses; what differs is which `SessionPort` is underneath, which is the
// answer `docs/CODE_ARCH.md` §10.1 leaves open and `SessionPort.hpp` gives.
//
// So the cases here are deliberately the cases the datagram suites already run:
// a client joins, a world arrives, a change replicates, an input goes back, a
// user message crosses, an identity is proved. A case that only QUIC could pass
// would be a case that says nothing about whether the swap is honest.

#include "Wire.hpp"

#include <engine/assets/Signature.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/quic/Tls.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/SessionPort.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.replication.quicwire")
TEST_DEPENDS("engine.replication.endtoend")
TEST_DEPENDS("engine.net.quic.connection")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::replication::ClientId;
using engine::replication::Connector;
using engine::replication::ConnectorSettings;
using engine::replication::Listener;
using engine::replication::ListenerSettings;
using engine::replication::MessageKind;
using engine::replication::QuicRouteFor;
using engine::replication::WireKind;

using namespace replication_wire;

namespace {
	// The seed the server's identity is derived from. Stated rather than
	// generated, so a failure is reproducible from the file alone.
	std::array<std::byte, 32> Seed() {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index * 5 + 17);
		}
		return seed;
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes;
		for (const char character : text) {
			bytes.push_back(static_cast<std::byte>(character));
		}
		return bytes;
	}

	std::string Text(std::span<const std::byte> bytes) {
		std::string text;
		for (const std::byte value : bytes) {
			text.push_back(static_cast<char>(value));
		}
		return text;
	}

	// A listener and one connector, both on the QUIC wire.
	struct Pair {
		std::vector<std::unique_ptr<Transport>> Ends;
		std::unique_ptr<Transport> ServerWire;
		std::unique_ptr<Transport> ClientWire;
		Store World{"server"};
		Store Replica{"client"};
		std::unique_ptr<Listener> Server;
		std::unique_ptr<Connector> Client;
		double Now = 0.0;

		explicit Pair(const LossSettings &serverLoss = {}, const LossSettings &clientLoss = {}) {
			RegisterTypes();
			Ends = MakeLoopbackTransport(2);
			REQUIRE(Ends.size() == 2);

			// Both ends wrapped whether or not they lose anything, for `Wire.hpp`'s
			// reason: a harness with the wrapper only on the cases that use it is
			// two harnesses, and the one nobody looks at is the one that drifts.
			ServerWire = std::make_unique<LossyTransport>(std::move(Ends[0]), serverLoss);
			ClientWire = std::make_unique<LossyTransport>(std::move(Ends[1]), clientLoss);

			ListenerSettings serving;
			serving.Wire = WireKind::Quic;
			serving.Quic.Connection.Tls.Seed = Seed();
			serving.Quic.Connection.Tls.HasSeed = true;
			Server = std::make_unique<Listener>(*ServerWire, serving);
			Server->Authority().Replicate(Name("endtoend_test.Spot"));

			ConnectorSettings connecting;
			connecting.Wire = WireKind::Quic;
			// The pin is stated once and lands wherever the wire needs it. Under
			// QUIC it becomes the raw public key TLS checks.
			engine::assets::PublicKey identity;
			const auto derived = engine::net::quic::IdentityFor(Seed());
			for (size_t index = 0; index < identity.Value.size(); index++) {
				identity.Value[index] = static_cast<uint8_t>(derived[index]);
			}
			connecting.ServerIdentity = identity;

			Client = std::make_unique<Connector>(*ClientWire, ServerWire->Local(), Now, connecting);
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

		bool Admit(uint64_t ticks = 200) {
			for (uint64_t tick = 1; tick <= ticks && !Client->Admitted(); ++tick) {
				Tick(tick);
			}
			// **Two more, because the two ends do not finish on the same tick.**
			// A TLS client is done when it has verified the server's Finished; a
			// server is done when the client's arrives, which is one flight
			// later. A harness that returned on the client's answer would have
			// every case race the server's.
			Tick(ticks + 1);
			Tick(ticks + 2);
			return Client->Admitted();
		}

		void Settle(uint64_t from, int ticks = 8) {
			for (int step = 0; step < ticks; ++step) {
				Tick(from + static_cast<uint64_t>(step));
			}
		}
	};
}

// --- the mapping ------------------------------------------------------------

TEST_CASE("every message kind has a channel of its own", "[replication][quic]") {
	// `docs/CODE_ARCH.md` §10's table, read back. The pair that most obviously
	// must not share a stream is a snapshot chunk and a structural change: one
	// is megabytes and the other is a door opening.
	CHECK(QuicRouteFor(MessageKind::SnapshotChunk).Channel != QuicRouteFor(MessageKind::Structure).Channel);
	CHECK(QuicRouteFor(MessageKind::SnapshotChunk).Reliable);
	CHECK(QuicRouteFor(MessageKind::Structure).Reliable);
	CHECK(QuicRouteFor(MessageKind::Input).Reliable);

	// And the one whose loss is covered by the next one arriving does not go on
	// a stream at all.
	CHECK_FALSE(QuicRouteFor(MessageKind::Delta).Reliable);
	CHECK_FALSE(QuicRouteFor(MessageKind::GroupSignatures).Reliable);
	CHECK_FALSE(QuicRouteFor(MessageKind::Disputed).Reliable);
}

// --- the join ---------------------------------------------------------------

TEST_CASE("a client joins a server over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	CHECK(pair.Client->Admitted());
	CHECK(pair.Server->Count() == 1);
	CHECK(pair.Server->Stats().Admitted == 1);

	// No `net::Link` underneath, and the absence is what a caller has to look at
	// rather than walk into.
	CHECK(pair.Client->Link() == nullptr);
}

TEST_CASE("a client refuses a server it did not pin", "[replication][quic]") {
	Pair pair;

	// A different key entirely. An X25519 agreement with nothing bound to it is
	// safe against a listener and not against a relay, which is what `D00006`
	// was filed about and what the pin closes.
	ConnectorSettings connecting;
	connecting.Wire = WireKind::Quic;
	engine::assets::PublicKey wrong;
	wrong.Value.fill(0x7f);
	connecting.ServerIdentity = wrong;

	Connector stranger(*pair.ClientWire, pair.ServerWire->Local(), pair.Now, connecting);
	for (uint64_t tick = 1; tick <= 60 && !stranger.Admitted(); ++tick) {
		pair.Now += 1.0 / 60.0;
		stranger.Poll(pair.Replica, pair.Now);
		pair.Server->Poll(pair.Now);
		pair.Server->Advance(pair.Now);
		stranger.Advance(pair.Now);
	}
	CHECK_FALSE(stranger.Admitted());
}

// --- the world --------------------------------------------------------------

TEST_CASE("a world replicates over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	const Entity marker = pair.World.Create();
	pair.World.Set(marker, Spot{4.0f, 9.0f});
	pair.Settle(300, 12);

	const Spot *arrived = pair.Replica.Get<Spot>(marker);
	REQUIRE(arrived != nullptr);
	CHECK(arrived->X == 4.0f);
	CHECK(arrived->Y == 9.0f);
}

TEST_CASE("a change replicates over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	const Entity marker = pair.World.Create();
	pair.World.Set(marker, Spot{1.0f, 1.0f});
	pair.Settle(300, 12);

	pair.World.Set(marker, Spot{2.0f, 3.0f});
	pair.Settle(400, 12);

	const Spot *arrived = pair.Replica.Get<Spot>(marker);
	REQUIRE(arrived != nullptr);
	CHECK(arrived->X == 2.0f);
	CHECK(arrived->Y == 3.0f);
}

TEST_CASE("a removal replicates over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	const Entity marker = pair.World.Create();
	pair.World.Set(marker, Spot{1.0f, 1.0f});
	pair.Settle(300, 12);
	REQUIRE(pair.Replica.Get<Spot>(marker) != nullptr);

	pair.World.Destroy(marker);
	pair.Settle(400, 12);
	CHECK(pair.Replica.Get<Spot>(marker) == nullptr);
}

// --- traffic in the other direction ----------------------------------------

TEST_CASE("an input reaches the server over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	REQUIRE(pair.Client->Submit(7, Bytes("jump"), pair.Now));
	pair.Settle(300, 8);

	const std::vector<Listener::Submission> inputs = pair.Server->Inputs();
	REQUIRE(inputs.size() == 1);
	REQUIRE(inputs[0].Inputs.size() == 1);
	CHECK(inputs[0].Inputs[0].Tick == 7);
	CHECK(Text(inputs[0].Inputs[0].Bytes) == "jump");
}

TEST_CASE("a user message crosses over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	std::vector<std::string> heard;
	pair.Server->OnUserMessage([&](ClientId, std::span<const std::byte> payload) {
		heard.push_back(Text(payload));
	});

	REQUIRE(pair.Client->SendUser(Bytes("an edit"), pair.Now));
	pair.Settle(300, 8);

	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == "an edit");
}

TEST_CASE("a server message reaches the client over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());

	std::vector<std::string> heard;
	pair.Client->OnUserMessage([&](std::span<const std::byte> payload) { heard.push_back(Text(payload)); });

	REQUIRE(pair.Server->Broadcast(Bytes("a reply"), pair.Now) == 1);
	pair.Settle(300, 8);

	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == "a reply");
}

// --- identity ---------------------------------------------------------------

TEST_CASE("a client proves an identity over QUIC", "[replication][quic]") {
	// **The exporter is what replaces `AdmissionTranscript`.** A claim is signed
	// over a value derived from this connection and no other, so a signature
	// captured here proves nothing anywhere else and a relay holding one
	// handshake with each side cannot carry it across.
	Pair pair;

	std::array<std::byte, 32> clientSeed{};
	clientSeed.fill(std::byte{0x21});
	const std::optional<engine::assets::SigningKey> key = engine::assets::SigningKey::FromSeed(clientSeed);
	REQUIRE(key.has_value());

	ConnectorSettings connecting;
	connecting.Wire = WireKind::Quic;
	engine::assets::PublicKey identity;
	const auto derived = engine::net::quic::IdentityFor(Seed());
	for (size_t index = 0; index < identity.Value.size(); index++) {
		identity.Value[index] = static_cast<uint8_t>(derived[index]);
	}
	connecting.ServerIdentity = identity;
	connecting.ClientIdentity = &key.value();

	pair.Client =
		std::make_unique<Connector>(*pair.ClientWire, pair.ServerWire->Local(), pair.Now, connecting);

	pair.Server->RequireClientIdentity(true);
	REQUIRE(pair.Admit());
	pair.Settle(300, 12);

	ClientId who;
	for (const Listener::Submission &submission : pair.Server->Inputs()) {
		who = submission.Client;
	}
	(void)who;

	// The world only reaches an identified client, so the arrival of any of it
	// is the claim having verified.
	const Entity marker = pair.World.Create();
	pair.World.Set(marker, Spot{5.0f, 6.0f});
	pair.Settle(400, 16);

	CHECK(pair.Replica.Get<Spot>(marker) != nullptr);
}

// --- over a link that loses things ------------------------------------------

TEST_CASE("a world replicates over QUIC across a lossy link", "[replication][quic]") {
	LossSettings loss;
	loss.LossChance = 0.15f;
	loss.Seed = 11;

	Pair pair(loss, loss);
	REQUIRE(pair.Admit(400));

	const Entity marker = pair.World.Create();
	pair.World.Set(marker, Spot{8.0f, 2.0f});
	pair.Settle(500, 120);

	const Spot *arrived = pair.Replica.Get<Spot>(marker);
	REQUIRE(arrived != nullptr);
	CHECK(arrived->X == 8.0f);
}

// --- lifecycle ---------------------------------------------------------------

TEST_CASE("a client that goes away is dropped over QUIC", "[replication][quic]") {
	Pair pair;
	REQUIRE(pair.Admit());
	REQUIRE(pair.Server->Count() == 1);

	std::vector<ClientId> gone;
	pair.Server->OnDropped([&](ClientId client) { gone.push_back(client); });

	// The client stops answering entirely. What must not happen is the slot
	// being held for ever: the idle timeout is the transport's and is driven off
	// the tick rather than off a thread.
	pair.Client.reset();
	for (uint64_t tick = 0; tick < 4000 && pair.Server->Count() > 0; tick++) {
		pair.Now += 1.0 / 60.0;
		pair.Server->Poll(pair.Now);
		pair.Server->Advance(pair.Now);
	}

	CHECK(pair.Server->Count() == 0);
	CHECK(gone.size() == 1);
}
