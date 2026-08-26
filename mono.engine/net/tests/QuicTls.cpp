#include <engine/net/quic/Tls.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.net.quic.tls")
TEST_DEPENDS("engine.net.quic.crypto")

using engine::net::quic::Level;
using engine::net::quic::Tls;
using engine::net::quic::TlsSettings;

namespace {
	// A seed is 32 bytes and nothing here needs it to be secret, so it is stated
	// rather than generated - a failing case is then reproducible from the file
	// alone, which is the same reason `LossyTransport` refuses a random device.
	std::array<std::byte, 32> Seed(uint8_t flavour) {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index * 7 + flavour);
		}
		return seed;
	}

	// One end of a handshake plus everything it was told to install.
	struct End {
		std::optional<Tls> Stack;

		// The traffic secrets, by level, as they were handed over.
		//@{
		std::array<std::vector<std::byte>, 3> ReadSecret;
		std::array<std::vector<std::byte>, 3> WriteSecret;
		//@}

		std::vector<std::byte> PeerParameters;
		std::vector<std::byte> Local;
		bool Complete = false;

		// What the peer must be given next, with the level it belongs at.
		std::vector<std::pair<Level, std::vector<std::byte>>> Outbound;

		// Applies everything the handshake asked for, and keeps the bytes.
		//
		// A server pauses after the ServerHello to be told its own transport
		// parameters, because it cannot know them until it has read the client's
		// - `Tls::NeedsParameters` is the argument. Here they are a constant, so
		// the pause is answered immediately; a real transport answers it by
		// encoding what its peer just asked for.
		void Drain() {
			Apply();
			while (Stack->NeedsParameters()) {
				Stack->SetTransportParameters(Local);
				if (!Stack->Resume()) {
					return;
				}
				Apply();
			}
		}

		void Apply() {
			for (const Tls::Event &event : Stack->Pending()) {
				switch (event.What) {
				case Tls::Event::Kind::Send:
					Outbound.emplace_back(event.At, event.Bytes);
					break;
				case Tls::Event::Kind::ReadKey:
					ReadSecret[static_cast<size_t>(event.At)] = {event.Secret.begin(), event.Secret.end()};
					break;
				case Tls::Event::Kind::WriteKey:
					WriteSecret[static_cast<size_t>(event.At)] = {event.Secret.begin(), event.Secret.end()};
					break;
				case Tls::Event::Kind::PeerParameters:
					PeerParameters = event.Bytes;
					break;
				case Tls::Event::Kind::Complete:
					Complete = true;
					break;
				}
			}
			Stack->ClearPending();
		}
	};

	std::vector<std::byte> Parameters(uint8_t flavour) {
		// Opaque here on purpose: this file is the handshake, and what the bytes
		// mean is the transport's business one layer out.
		return {std::byte{flavour}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
	}

	// Runs both ends until neither has anything left to say.
	//
	// @param client     The client end.
	// @param server     The server end.
	// @param fragmented Whether to hand every flight over one byte at a time,
	//        which is what a real CRYPTO stream does when a flight spans packets.
	// @return `false` as soon as either end refuses.
	bool Exchange(End &client, End &server, bool fragmented = false) {
		for (int round = 0; round < 8; round++) {
			const bool any = !client.Outbound.empty() || !server.Outbound.empty();
			if (!any) {
				return true;
			}

			const auto deliver = [&](End &from, End &to) {
				auto flights = std::move(from.Outbound);
				from.Outbound.clear();
				for (const auto &[level, bytes] : flights) {
					bool taken = true;
					if (fragmented) {
						for (const std::byte value : bytes) {
							const std::array<std::byte, 1> one{value};
							taken = to.Stack->Receive(level, one);
							if (!taken) {
								break;
							}
						}
					} else {
						taken = to.Stack->Receive(level, bytes);
					}
					if (!taken) {
						// The reason, not just the refusal. A handshake that
						// stopped says exactly why, and a failing case that only
						// says `false` sends the reader back through six messages
						// by hand.
						UNSCOPED_INFO("refused: " << to.Stack->Failure());
						return false;
					}
				}
				to.Drain();
				return true;
			};

			if (!deliver(client, server)) {
				return false;
			}
			if (!deliver(server, client)) {
				return false;
			}
		}
		return true;
	}

	// A client and a server that agree about everything.
	std::pair<End, End> Pair(bool pin = true) {
		End client;
		End server;

		TlsSettings serverSide;
		serverSide.Seed = Seed(1);
		serverSide.HasSeed = true;
		server.Stack.emplace(Tls::Role::Server, serverSide);
		server.Local = Parameters(0xa0);

		TlsSettings clientSide;
		clientSide.PinIdentity = pin;
		clientSide.Expected = engine::net::quic::IdentityFor(Seed(1));
		client.Stack.emplace(Tls::Role::Client, clientSide);
		client.Stack->SetTransportParameters(Parameters(0xb0));

		return {std::move(client), std::move(server)};
	}
}

// --- the happy path ---------------------------------------------------------

TEST_CASE("a handshake completes and both ends agree on every key", "[net][quic][tls]") {
	auto [client, server] = Pair();

	REQUIRE(client.Stack->Begin());
	client.Drain();
	REQUIRE(Exchange(client, server));

	CHECK(client.Complete);
	CHECK(server.Complete);
	CHECK(client.Stack->Complete());
	CHECK(server.Stack->Complete());

	// The whole point of a key exchange: what one end seals with is what the
	// other opens with, at both levels, in both directions.
	for (const Level level : {Level::Handshake, Level::Application}) {
		const size_t at = static_cast<size_t>(level);
		CHECK_FALSE(client.WriteSecret[at].empty());
		CHECK(client.WriteSecret[at] == server.ReadSecret[at]);
		CHECK(client.ReadSecret[at] == server.WriteSecret[at]);
	}

	// And the two directions are not the same secret, which is the property
	// that stops one direction's packets opening under the other's key.
	CHECK(
		client.WriteSecret[static_cast<size_t>(Level::Application)] !=
		client.ReadSecret[static_cast<size_t>(Level::Application)]
	);
}

TEST_CASE("the engine's own suite is chosen", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();
	REQUIRE(Exchange(client, server));

	// ChaCha20-Poly1305, for `Cipher.hpp`'s reason: constant time on hardware
	// with no AES instructions. The server picks, and it picks this when the
	// client offers it.
	CHECK(client.Stack->Suite() == engine::net::quic::Aead::ChaCha20Poly1305);
	CHECK(server.Stack->Suite() == engine::net::quic::Aead::ChaCha20Poly1305);
	CHECK(client.Stack->Header() == engine::net::quic::HeaderCipher::ChaCha20);
}

TEST_CASE("the transport parameters cross in both directions", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();
	REQUIRE(Exchange(client, server));

	CHECK(client.PeerParameters == Parameters(0xa0));
	CHECK(server.PeerParameters == Parameters(0xb0));
}

TEST_CASE("a flight split byte by byte still completes", "[net][quic][tls]") {
	// A CRYPTO frame ends where the sender's packet ended, so a server's flight
	// - four messages and a signature - is routinely spread over several. A
	// parser that assumed one message per call would work on a loopback and fail
	// on a network.
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();
	REQUIRE(Exchange(client, server, true));

	CHECK(client.Complete);
	CHECK(server.Complete);
	CHECK(
		client.WriteSecret[static_cast<size_t>(Level::Application)] ==
		server.ReadSecret[static_cast<size_t>(Level::Application)]
	);
}

TEST_CASE("two handshakes with one identity share no keys", "[net][quic][tls]") {
	// The identity is long-lived and the agreement is not. If a second
	// connection derived the same traffic secrets, recording one and replaying it
	// against the other would work.
	auto first = Pair();
	REQUIRE(first.first.Stack->Begin());
	first.first.Drain();
	REQUIRE(Exchange(first.first, first.second));

	auto second = Pair();
	REQUIRE(second.first.Stack->Begin());
	second.first.Drain();
	REQUIRE(Exchange(second.first, second.second));

	CHECK(
		first.first.WriteSecret[static_cast<size_t>(Level::Application)] !=
		second.first.WriteSecret[static_cast<size_t>(Level::Application)]
	);
}

// --- the identity, which is what D00006 was filed about ---------------------

TEST_CASE("a client refuses a server it did not pin", "[net][quic][tls]") {
	End client;
	End server;

	TlsSettings serverSide;
	serverSide.Seed = Seed(2);
	serverSide.HasSeed = true;
	server.Stack.emplace(Tls::Role::Server, serverSide);
	server.Local = Parameters(0xa0);

	TlsSettings clientSide;
	clientSide.PinIdentity = true;
	// The identity of a different server. An X25519 agreement with nothing bound
	// to it is safe against a listener and not against a relay, which can hold
	// one exchange with each side; this is the check that closes that.
	clientSide.Expected = engine::net::quic::IdentityFor(Seed(1));
	client.Stack.emplace(Tls::Role::Client, clientSide);
	client.Stack->SetTransportParameters(Parameters(0xb0));

	REQUIRE(client.Stack->Begin());
	client.Drain();
	CHECK_FALSE(Exchange(client, server));

	CHECK(client.Stack->Failed());
	CHECK_FALSE(client.Stack->Complete());
	// `certificate_unknown`, RFC 8446 §6.2.
	CHECK(client.Stack->Alert() == 46);
}

TEST_CASE("a client with pinning off reaches the key it was not checking", "[net][quic][tls]") {
	auto [client, server] = Pair(false);
	REQUIRE(client.Stack->Begin());
	client.Drain();
	REQUIRE(Exchange(client, server));

	CHECK(client.Complete);
	// The identity is still reported, so a caller that turned the check off can
	// make it for itself. What it must not be is absent.
	CHECK(client.Stack->PeerIdentity().size() == engine::net::quic::IDENTITY_BYTES);
}

TEST_CASE("a server with no identity refuses at construction", "[net][quic][tls]") {
	// A server that cannot sign the transcript cannot be pinned against, so the
	// refusal is here rather than three messages in where it would look like a
	// network problem.
	TlsSettings settings;
	settings.HasSeed = false;
	Tls stack(Tls::Role::Server, settings);
	CHECK(stack.Failed());
}

// --- every field of it is hostile -------------------------------------------

TEST_CASE("a rewritten signature is refused", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();

	// One round: the server answers the ClientHello with its whole flight.
	for (const auto &[level, bytes] : client.Outbound) {
		REQUIRE(server.Stack->Receive(level, bytes));
	}
	client.Outbound.clear();
	server.Drain();

	// The Initial flight is the ServerHello; the Handshake one carries the
	// signature. Flipping a bit anywhere in it must fail the verification rather
	// than fail the Finished a message later.
	REQUIRE(server.Outbound.size() == 2);
	auto &flight = server.Outbound[1].second;
	flight[flight.size() / 2] ^= std::byte{0x40};

	bool refused = false;
	for (const auto &[level, bytes] : server.Outbound) {
		if (!client.Stack->Receive(level, bytes)) {
			refused = true;
			break;
		}
	}
	CHECK(refused);
	CHECK(client.Stack->Failed());
}

TEST_CASE("a truncated ClientHello is refused rather than half-read", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();

	REQUIRE(client.Outbound.size() == 1);
	auto hello = client.Outbound[0].second;

	// The length field says one thing and the body is another. A parser that
	// trusted the first would read past the buffer.
	hello[3] = static_cast<std::byte>(static_cast<uint8_t>(hello[3]) + 4);
	CHECK(server.Stack->Receive(Level::Initial, hello));
	// Not a refusal: it is a short message, which is what a CRYPTO frame that
	// has not all arrived looks like. Nothing has been acted on.
	CHECK_FALSE(server.Stack->Complete());
	CHECK(server.Stack->Pending().empty());
}

TEST_CASE("a ClientHello with a broken extension block is refused", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();

	auto hello = client.Outbound[0].second;

	// Eight bytes off the end, with the message length corrected to match. The
	// message is now whole and self-consistent and its extension block still
	// claims the bytes that are gone - which is the shape a length-prefixed
	// parser gets wrong when it trusts an inner length against an outer one.
	hello.resize(hello.size() - 8);
	const size_t body = hello.size() - 4;
	hello[1] = static_cast<std::byte>((body >> 16) & 0xff);
	hello[2] = static_cast<std::byte>((body >> 8) & 0xff);
	hello[3] = static_cast<std::byte>(body & 0xff);

	CHECK_FALSE(server.Stack->Receive(Level::Initial, hello));
	CHECK(server.Stack->Failed());
	// `decode_error`, RFC 8446 §6.2.
	CHECK(server.Stack->Alert() == 50);
}

TEST_CASE("a message at the wrong level is refused", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();

	// A ClientHello belongs at Initial. Accepting it at Handshake would let
	// somebody who can write to this address restart a handshake mid-connection.
	CHECK_FALSE(server.Stack->Receive(Level::Handshake, client.Outbound[0].second));
	CHECK(server.Stack->Failed());
	// `unexpected_message`, RFC 8446 §6.2.
	CHECK(server.Stack->Alert() == 10);
}

TEST_CASE("a server refuses a client that speaks another protocol", "[net][quic][tls]") {
	End client;
	End server;

	TlsSettings serverSide;
	serverSide.Seed = Seed(1);
	serverSide.HasSeed = true;
	serverSide.Protocol = "atomic/1";
	server.Stack.emplace(Tls::Role::Server, serverSide);
	server.Local = Parameters(0xa0);

	TlsSettings clientSide;
	clientSide.PinIdentity = false;
	clientSide.Protocol = "something-else/9";
	client.Stack.emplace(Tls::Role::Client, clientSide);
	client.Stack->SetTransportParameters(Parameters(0xb0));

	REQUIRE(client.Stack->Begin());
	client.Drain();
	CHECK_FALSE(server.Stack->Receive(Level::Initial, client.Outbound[0].second));
	// `handshake_failure`. Two protocols sharing a port and a key schedule and
	// differing only in what the first byte after the handshake means is what
	// ALPN exists to prevent.
	CHECK(server.Stack->Alert() == 40);
}

TEST_CASE("a handshake with no transport parameters refuses to start", "[net][quic][tls]") {
	TlsSettings settings;
	settings.PinIdentity = false;
	Tls stack(Tls::Role::Client, settings);
	CHECK_FALSE(stack.Begin());
	CHECK(stack.Failed());
}

TEST_CASE("a refused handshake stays refused", "[net][quic][tls]") {
	auto [client, server] = Pair();
	REQUIRE(client.Stack->Begin());
	client.Drain();

	CHECK_FALSE(server.Stack->Receive(Level::Handshake, client.Outbound[0].second));
	// A second, entirely valid message must not revive it. A handle that can come
	// back to life is one every caller has to re-check, which is the rule
	// `net/AGENTS.md` states for a `Link` and holds here too.
	CHECK_FALSE(server.Stack->Receive(Level::Initial, client.Outbound[0].second));
	CHECK(server.Stack->Failed());
}
