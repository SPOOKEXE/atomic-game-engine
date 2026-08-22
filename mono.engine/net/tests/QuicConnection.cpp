#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/quic/Connection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.quic.connection")
TEST_DEPENDS("engine.net.quic.tls")
TEST_DEPENDS("engine.net.transport")
TEST_DEPENDS("engine.net.lossytransport")

using engine::net::Endpoint;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::net::TransportStatus;
using engine::net::quic::Accepts;
using engine::net::quic::Arrival;
using engine::net::quic::Connection;
using engine::net::quic::ConnectionSettings;
using engine::net::quic::ConnectionState;

namespace {
	std::array<std::byte, 32> Seed() {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index * 11 + 3);
		}
		return seed;
	}

	std::vector<std::byte> Text(std::string_view text) {
		std::vector<std::byte> bytes;
		for (const char letter : text) {
			bytes.push_back(static_cast<std::byte>(letter));
		}
		return bytes;
	}

	std::string Read(std::span<const std::byte> bytes) {
		std::string text;
		for (const std::byte value : bytes) {
			text.push_back(static_cast<char>(value));
		}
		return text;
	}

	// Two ends of a loopback network with a QUIC connection over each, driven by
	// a tick the test states rather than by a clock.
	//
	// The server half is what a `Listener` will be: poll, decide whether the
	// datagram opens a connection or belongs to one, and never remember anything
	// about a sender that has not answered.
	struct Fixture {
		std::vector<std::unique_ptr<Transport>> Ends;
		Transport *ClientWire = nullptr;
		Transport *ServerWire = nullptr;
		std::unique_ptr<Transport> ClientLossy;
		std::unique_ptr<Transport> ServerLossy;
		std::unique_ptr<Connection> Client;
		std::unique_ptr<Connection> Server;
		ConnectionSettings ServerSide;
		double Now = 0.0;

		// A tick is a sixtieth, which is what this engine's are.
		static constexpr double TICK = 1.0 / 60.0;

		explicit Fixture(
			const LossSettings &clientLoss = {}, const LossSettings &serverLoss = {}, bool pinCorrectly = true
		) {
			Ends = MakeLoopbackTransport(2);

			ClientLossy = std::make_unique<LossyTransport>(std::move(Ends[0]), clientLoss);
			ServerLossy = std::make_unique<LossyTransport>(std::move(Ends[1]), serverLoss);
			ClientWire = ClientLossy.get();
			ServerWire = ServerLossy.get();

			ServerSide.Tls.Seed = Seed();
			ServerSide.Tls.HasSeed = true;

			ConnectionSettings clientSide;
			clientSide.Tls.PinIdentity = true;
			clientSide.Tls.Expected = engine::net::quic::IdentityFor(Seed());
			if (!pinCorrectly) {
				clientSide.Tls.Expected[0] ^= std::byte{0xff};
			}

			Client = Connection::Connect(*ClientWire, ServerWire->Local(), Now, clientSide);
		}

		// One tick each way: flush, then deliver everything that arrived.
		void Step(int ticks = 1) {
			for (int tick = 0; tick < ticks; tick++) {
				Now += TICK;
				if (Client != nullptr) {
					Client->Flush(Now);
				}
				if (Server != nullptr) {
					Server->Flush(Now);
				}
				PumpServer();
				PumpClient();
			}
		}

		void PumpServer() {
			std::vector<std::byte> datagram;
			while (true) {
				const Transport::Inbound inbound = ServerWire->Receive(datagram);
				if (inbound.Status != TransportStatus::Ok) {
					break;
				}
				if (Server == nullptr) {
					// Nothing is remembered about a sender that has not opened a
					// connection, which is `net/AGENTS.md`'s zero-bytes rule
					// surviving the swap.
					if (Accepts(datagram)) {
						Server = Connection::Accept(*ServerWire, inbound.From, datagram, Now, ServerSide);
					}
					continue;
				}
				Server->Receive(datagram, Now);
			}
		}

		void PumpClient() {
			std::vector<std::byte> datagram;
			while (true) {
				const Transport::Inbound inbound = ClientWire->Receive(datagram);
				if (inbound.Status != TransportStatus::Ok) {
					break;
				}
				Client->Receive(datagram, Now);
			}
		}

		// Runs until both ends are established, or gives up.
		bool Settle(int ticks = 40) {
			for (int tick = 0; tick < ticks; tick++) {
				Step();
				if (Server != nullptr && Client->State() == ConnectionState::Established &&
					Server->State() == ConnectionState::Established) {
					return true;
				}
				if (Client->State() == ConnectionState::Closed) {
					return false;
				}
			}
			return false;
		}
	};

	std::vector<std::string> Messages(const Connection &connection, uint8_t channel) {
		std::vector<std::string> found;
		for (const Arrival &arrival : connection.Inbound()) {
			if (arrival.Channel == channel) {
				found.push_back(Read(arrival.Bytes));
			}
		}
		return found;
	}
}

// --- the handshake ----------------------------------------------------------

TEST_CASE("a QUIC handshake completes over the loopback", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	CHECK(fixture.Client->State() == ConnectionState::Established);
	CHECK(fixture.Server->State() == ConnectionState::Established);
	CHECK(fixture.Server->IsServer());
	CHECK_FALSE(fixture.Client->IsServer());

	// The client knows who it is talking to, which is the whole of what `D00006`
	// asked for and what a bare X25519 exchange could not answer.
	CHECK(fixture.Client->PeerIdentity().size() == engine::net::quic::IDENTITY_BYTES);
	// The server asked for no client certificate, so it has nothing to report.
	CHECK(fixture.Server->PeerIdentity().empty());
}

TEST_CASE("a client refuses a server it did not pin", "[net][quic][connection]") {
	Fixture fixture({}, {}, false);
	CHECK_FALSE(fixture.Settle());
	CHECK(fixture.Client->State() != ConnectionState::Established);
	CHECK(std::string_view(fixture.Client->Failure()).find("pinned") != std::string_view::npos);
}

TEST_CASE("a connection answers to an identifier a listener can route by", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	// A server holds one socket and many connections, so the destination
	// connection id in the header is what says which one a datagram is for.
	CHECK_FALSE(fixture.Server->LocalIds().empty());
	CHECK(fixture.Server->LocalIds()[0].size() == engine::net::quic::CONNECTION_ID_BYTES);
}

// --- reliable channels ------------------------------------------------------

TEST_CASE("reliable messages arrive whole and in order", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->Send(0, Text("one"), fixture.Now));
	REQUIRE(fixture.Client->Send(0, Text("two"), fixture.Now));
	REQUIRE(fixture.Client->Send(0, Text("three"), fixture.Now));
	fixture.Step(4);

	const std::vector<std::string> found = Messages(*fixture.Server, 0);
	REQUIRE(found.size() == 3);
	CHECK(found[0] == "one");
	CHECK(found[1] == "two");
	CHECK(found[2] == "three");
}

TEST_CASE("a message longer than one datagram still arrives whole", "[net][quic][connection]") {
	// The old framing refuses anything over `MAXIMUM_PAYLOAD_BYTES` because a
	// fragmented datagram is lost entirely when any one fragment is. A stream has
	// no such limit, which is what makes a join snapshot a message rather than a
	// chunking scheme above the transport.
	Fixture fixture;
	REQUIRE(fixture.Settle());

	const std::string big(20000, 'x');
	REQUIRE(fixture.Client->Send(1, Text(big), fixture.Now));
	fixture.Step(30);

	const std::vector<std::string> found = Messages(*fixture.Server, 1);
	REQUIRE(found.size() == 1);
	CHECK(found[0].size() == big.size());
	CHECK(found[0] == big);
}

TEST_CASE("both directions carry traffic at once", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->Send(2, Text("up"), fixture.Now));
	REQUIRE(fixture.Server->Send(2, Text("down"), fixture.Now));
	fixture.Step(4);

	CHECK(Messages(*fixture.Server, 2) == std::vector<std::string>{"up"});
	CHECK(Messages(*fixture.Client, 2) == std::vector<std::string>{"down"});
}

TEST_CASE("a channel out of range is refused", "[net][quic][connection]") {
	// A channel indexes an array on both ends, so the refusal is at the call site
	// rather than at the far one.
	Fixture fixture;
	REQUIRE(fixture.Settle());
	CHECK_FALSE(fixture.Client->Send(engine::net::quic::MAXIMUM_CHANNELS, Text("no"), fixture.Now));
}

TEST_CASE("a message over the configured maximum is refused", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());
	const std::string huge(2 * 1024 * 1024, 'y');
	CHECK_FALSE(fixture.Client->Send(0, Text(huge), fixture.Now));
}

// --- the head-of-line property this was done for ----------------------------

TEST_CASE(
	"a large message on one channel does not hold up a small one on another", "[net][quic][connection]"
) {
	// **The reason for the whole exercise.** `docs/CODE_ARCH.md` §10 calls the
	// shared reliable window "a property of the design": a megabyte snapshot
	// stalls a door opening because both travel through one ordered window. Two
	// streams have two windows.
	Fixture fixture;
	REQUIRE(fixture.Settle());

	const std::string snapshot(60000, 'z');
	REQUIRE(fixture.Client->Send(0, Text(snapshot), fixture.Now));
	REQUIRE(fixture.Client->Send(1, Text("door"), fixture.Now));

	// A handful of ticks: nowhere near enough for sixty kilobytes, and plenty for
	// four bytes on a channel of its own.
	fixture.Step(3);

	CHECK(Messages(*fixture.Server, 1) == std::vector<std::string>{"door"});
	CHECK(Messages(*fixture.Server, 0).empty());

	fixture.Step(60);
	const std::vector<std::string> late = Messages(*fixture.Server, 0);
	REQUIRE(late.size() == 1);
	CHECK(late[0].size() == snapshot.size());
}

// --- unreliable traffic -----------------------------------------------------

TEST_CASE("an unreliable message arrives as a datagram frame", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->SendUnreliable(3, Text("position"), fixture.Now));
	fixture.Step(3);

	const std::vector<std::string> found = Messages(*fixture.Server, 3);
	REQUIRE(found.size() == 1);
	CHECK(found[0] == "position");
	CHECK(fixture.Client->Stats().Datagrams == 1);
}

TEST_CASE("an unreliable message reports whether it arrived", "[net][quic][connection]") {
	// **The one thing the hand-rolled stack structurally cannot do.** RFC 9221
	// datagrams are never retransmitted and their containing packet is still
	// acknowledged, so a server publishing a still world sees the whole outbound
	// stream rather than the reliable slice of it - which is `D00014`'s fourth
	// argument and the gap `net::CongestionControl` was shipped with.
	Fixture fixture;
	REQUIRE(fixture.Settle());

	for (int index = 0; index < 5; index++) {
		REQUIRE(fixture.Client->SendUnreliable(3, Text("tick"), fixture.Now));
		fixture.Step(2);
	}
	fixture.Step(6);

	CHECK(fixture.Client->Stats().Datagrams == 5);
	CHECK(fixture.Client->Stats().DatagramsAcknowledged > 0);
}

TEST_CASE("an unreliable message too large for one datagram is refused whole", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	const std::string big(4000, 'q');
	CHECK_FALSE(fixture.Client->SendUnreliable(3, Text(big), fixture.Now));
	CHECK(fixture.Client->Stats().DatagramsRefused == 1);
}

TEST_CASE(
	"an unreliable message before the handshake is refused rather than queued", "[net][quic][connection]"
) {
	// Nothing would resend it, and a backlog that arrives all at once when the
	// handshake finishes is worse than the loss it was trying to avoid.
	Fixture fixture;
	CHECK_FALSE(fixture.Client->SendUnreliable(0, Text("early"), fixture.Now));
	CHECK(fixture.Client->Stats().DatagramsRefused == 1);
}

// --- over a link that loses things ------------------------------------------

TEST_CASE("a handshake completes across a lossy link", "[net][quic][connection]") {
	// `LossyTransport` is how the hand-rolled stack was proved and is how this
	// one is. The seed makes the failure reproducible from the file alone.
	LossSettings loss;
	loss.LossChance = 0.2f;
	loss.Seed = 7;

	Fixture fixture(loss, loss);
	REQUIRE(fixture.Settle(200));
	CHECK(fixture.Client->State() == ConnectionState::Established);
}

TEST_CASE("a reliable message survives a dropped packet", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->Send(0, Text("kept"), fixture.Now));

	// The next arrival at the server is discarded, whichever datagram that turns
	// out to be - which is worth more than a percentage, because a test knows
	// what it just sent and does not know which arrival that will be.
	static_cast<LossyTransport *>(fixture.ServerWire)->DropNext(1);
	fixture.Step(60);

	CHECK(Messages(*fixture.Server, 0) == std::vector<std::string>{"kept"});
}

TEST_CASE("a duplicated packet delivers its message once", "[net][quic][connection]") {
	LossSettings loss;
	loss.Duplicate = {3, 4, 5};

	Fixture fixture({}, loss);
	REQUIRE(fixture.Settle(80));

	REQUIRE(fixture.Client->Send(0, Text("once"), fixture.Now));
	fixture.Step(10);

	CHECK(Messages(*fixture.Server, 0) == std::vector<std::string>{"once"});
}

TEST_CASE("reordered packets are delivered in order on the stream", "[net][quic][connection]") {
	LossSettings loss;
	loss.Reorder = {2, 4, 6, 8};

	Fixture fixture({}, loss);
	REQUIRE(fixture.Settle(80));

	for (int index = 0; index < 6; index++) {
		REQUIRE(fixture.Client->Send(0, Text(std::to_string(index)), fixture.Now));
	}
	fixture.Step(30);

	const std::vector<std::string> found = Messages(*fixture.Server, 0);
	REQUIRE(found.size() == 6);
	for (size_t index = 0; index < found.size(); index++) {
		CHECK(found[index] == std::to_string(index));
	}
}

// --- lifecycle ---------------------------------------------------------------

TEST_CASE("a polite close reaches the peer", "[net][quic][connection]") {
	// **Not a formality.** Skipping it makes a peer that left politely
	// indistinguishable from one that crashed, and every clean exit then costs
	// the other end a full idle timeout.
	Fixture fixture;
	REQUIRE(fixture.Settle());

	fixture.Client->Close(fixture.Now);
	CHECK(fixture.Client->State() == ConnectionState::Closing);

	fixture.Step(2);
	CHECK(fixture.Server->State() == ConnectionState::Closed);
}

TEST_CASE("inbound is dropped when the caller has taken it", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->Send(0, Text("taken"), fixture.Now));
	fixture.Step(4);
	REQUIRE(fixture.Server->Inbound().size() == 1);

	fixture.Server->ClearInbound();
	CHECK(fixture.Server->Inbound().empty());
}

TEST_CASE("the round trip is measured and the window is reported", "[net][quic][connection]") {
	Fixture fixture;
	REQUIRE(fixture.Settle());

	REQUIRE(fixture.Client->Send(0, Text("measure"), fixture.Now));
	fixture.Step(10);

	const Connection::Statistics stats = fixture.Client->Stats();
	CHECK(stats.Sent > 0);
	CHECK(stats.CongestionWindow > 0);
	// Zero would mean unknown rather than instant; something has crossed and
	// come back by now.
	CHECK(stats.RoundTripMilliseconds > 0.0);
}

TEST_CASE(
	"an exported value is the same on both ends and differs per connection", "[net][quic][connection]"
) {
	// What replaces `AdmissionTranscript`. A client's identity claim is signed
	// over one of these, so a signature captured from one connection proves
	// nothing on another and a relay holding one handshake with each side cannot
	// carry the claim across.
	Fixture first;
	REQUIRE(first.Settle());

	std::array<std::byte, 32> mine{};
	std::array<std::byte, 32> theirs{};
	REQUIRE(first.Client->Export("atomic identity", mine));
	REQUIRE(first.Server->Export("atomic identity", theirs));
	CHECK(mine == theirs);

	// A different label over the same connection is a different value.
	std::array<std::byte, 32> other{};
	REQUIRE(first.Client->Export("atomic something else", other));
	CHECK(mine != other);

	Fixture second;
	REQUIRE(second.Settle());
	std::array<std::byte, 32> later{};
	REQUIRE(second.Client->Export("atomic identity", later));
	CHECK(mine != later);
}

TEST_CASE("nothing can be exported before the handshake completes", "[net][quic][connection]") {
	Fixture fixture;
	std::array<std::byte, 32> value{};
	CHECK_FALSE(fixture.Client->Export("atomic identity", value));
}
