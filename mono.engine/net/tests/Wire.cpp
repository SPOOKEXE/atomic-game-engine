// The discriminator, over bytes the two stacks really produce.
//
// **The claim under test is that the two header forms cannot be confused**, and
// the only honest way to check it is to hand `WireOf` packets the stacks
// themselves wrote rather than bytes this file made up to match the comment it
// is checking.

#include <engine/core/Bytes.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/net/quic/Connection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.net.wire")
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.net.quic.connection")

using engine::net::ChannelKind;
using engine::net::Describe;
using engine::net::MakeLoopbackTransport;
using engine::net::Packet;
using engine::net::PacketHeader;
using engine::net::ParseWireMode;
using engine::net::Serves;
using engine::net::Transport;
using engine::net::WireKind;
using engine::net::WireMode;
using engine::net::WireOf;

namespace {
	// One datagram of each stack's opening message, as the stack writes it.
	struct Openings {
		std::vector<std::unique_ptr<Transport>> Ends;
		std::vector<std::byte> Quic;
		std::vector<std::byte> Datagram;

		Openings() {
			Ends = MakeLoopbackTransport(2);
			REQUIRE(Ends.size() == 2);

			engine::net::quic::ConnectionSettings settings;
			settings.Tls.PinIdentity = false;
			std::unique_ptr<engine::net::quic::Connection> client =
				engine::net::quic::Connection::Connect(*Ends[1], Ends[0]->Local(), 0.0, settings);
			REQUIRE(client != nullptr);
			REQUIRE(client->Flush(0.0) > 0);

			const Transport::Inbound arrived = Ends[0]->Receive(Quic);
			REQUIRE(arrived.Status == engine::net::TransportStatus::Ok);

			// The datagram stack's opening message is a handshake-channel
			// packet, which is what `replication::FrameAdmission` produces.
			engine::core::ByteWriter writer;
			PacketHeader header;
			header.Channel = ChannelKind::Handshake;
			const std::array<std::byte, 32> body{};
			REQUIRE(Packet::Write(writer, header, body));
			Datagram.assign(writer.Bytes().begin(), writer.Bytes().end());
		}
	};
}

TEST_CASE("the two opening packets are told apart by their first byte", "[net][wire]") {
	Openings openings;

	REQUIRE(WireOf(openings.Quic).has_value());
	CHECK(*WireOf(openings.Quic) == WireKind::Quic);

	REQUIRE(WireOf(openings.Datagram).has_value());
	CHECK(*WireOf(openings.Datagram) == WireKind::Datagram);

	// The bit the comment in `Wire.hpp` names, checked rather than asserted in
	// prose. A QUIC long header sets Header Form and Fixed Bit; the datagram
	// stack's magic is `ATN1` little-endian, so its first byte is `'A'`.
	CHECK((static_cast<unsigned>(openings.Quic[0]) & 0xC0u) == 0xC0u);
	CHECK(openings.Datagram[0] == static_cast<std::byte>('A'));
	CHECK((static_cast<unsigned>(openings.Datagram[0]) & 0x80u) == 0u);
}

TEST_CASE("neither stack claims what is not its own", "[net][wire]") {
	CHECK_FALSE(WireOf({}).has_value());

	// A long header with a version nothing serves. It is not an Initial this
	// build can stand a connection up from, so calling it QUIC would move the
	// refusal one step later rather than remove it.
	const std::array<std::byte, 8> stranger{
		std::byte{0xC0},
		std::byte{0x7f},
		std::byte{0x00},
		std::byte{0x00},
		std::byte{0x01},
		std::byte{0x00},
		std::byte{0x00},
		std::byte{0x00}
	};
	CHECK_FALSE(WireOf(stranger).has_value());

	// A short header whose first byte is exactly the datagram stack's magic
	// byte. The full magic and version are what separate the two, not the byte.
	const std::array<std::byte, 8> ambiguous{
		std::byte{0x41},
		std::byte{0x54},
		std::byte{0x4E},
		std::byte{0x32},
		std::byte{0x01},
		std::byte{0x00},
		std::byte{0x00},
		std::byte{0x00}
	};
	CHECK_FALSE(WireOf(ambiguous).has_value());
}

TEST_CASE("a version negotiation answers an initial and is smaller than it", "[net][wire]") {
	Openings openings;

	std::array<std::byte, engine::net::quic::VERSION_NEGOTIATION_BYTES> reply{};
	const size_t written = engine::net::quic::WriteVersionNegotiation(openings.Quic, reply);
	REQUIRE(written > 0);

	// **The amplification rule survives the change of transport.** An Initial is
	// padded to 1200 bytes and the refusal is under sixty, so answering one
	// cannot be used to bounce traffic at somebody else.
	CHECK(written < openings.Quic.size());

	// A long header with the Version field zero, which is what a Version
	// Negotiation packet is and nothing else is.
	CHECK((static_cast<unsigned>(reply[0]) & 0x80u) == 0x80u);
	CHECK(reply[1] == std::byte{0});
	CHECK(reply[2] == std::byte{0});
	CHECK(reply[3] == std::byte{0});
	CHECK(reply[4] == std::byte{0});

	// It is not itself something the discriminator would admit as an opening
	// packet, so a reply bouncing back cannot open a connection.
	CHECK_FALSE(WireOf(std::span<const std::byte>(reply).first(written)).has_value());

	// And nothing that is not a QUIC packet produces one.
	std::array<std::byte, engine::net::quic::VERSION_NEGOTIATION_BYTES> unused{};
	CHECK(engine::net::quic::WriteVersionNegotiation(openings.Datagram, unused) == 0);
}

TEST_CASE("a client falls out of its handshake when it is refused", "[net][wire]") {
	Openings openings;

	engine::net::quic::ConnectionSettings settings;
	settings.Tls.PinIdentity = false;
	std::unique_ptr<engine::net::quic::Connection> client =
		engine::net::quic::Connection::Connect(*openings.Ends[1], openings.Ends[0]->Local(), 0.0, settings);
	REQUIRE(client != nullptr);
	REQUIRE(client->Flush(0.0) > 0);

	std::vector<std::byte> initial;
	REQUIRE(openings.Ends[0]->Receive(initial).Status == engine::net::TransportStatus::Ok);
	CHECK_FALSE(client->Refused());

	std::array<std::byte, engine::net::quic::VERSION_NEGOTIATION_BYTES> reply{};
	const size_t written = engine::net::quic::WriteVersionNegotiation(initial, reply);
	REQUIRE(written > 0);

	client->Receive(std::span<const std::byte>(reply).first(written), 0.001);
	CHECK(client->Refused());
	CHECK(client->State() == engine::net::quic::ConnectionState::Closed);
}

TEST_CASE("a mode says which wires it answers", "[net][wire]") {
	CHECK(Serves(WireMode::Quic, WireKind::Quic));
	CHECK_FALSE(Serves(WireMode::Quic, WireKind::Datagram));
	CHECK(Serves(WireMode::Datagram, WireKind::Datagram));
	CHECK_FALSE(Serves(WireMode::Datagram, WireKind::Quic));
	CHECK(Serves(WireMode::Both, WireKind::Quic));
	CHECK(Serves(WireMode::Both, WireKind::Datagram));
}

TEST_CASE("every wire and mode has a name, and a name round-trips", "[net][wire]") {
	for (const WireKind kind : {WireKind::Datagram, WireKind::Quic}) {
		CHECK(std::string(Describe(kind)) != "?");
	}
	for (const WireMode mode : {WireMode::Quic, WireMode::Datagram, WireMode::Both}) {
		CHECK(std::string(Describe(mode)) != "?");
		const std::optional<WireMode> parsed = ParseWireMode(Describe(mode));
		REQUIRE(parsed.has_value());
		CHECK(*parsed == mode);
	}

	// Named rather than defaulted, which is `server.idle-sleep`'s position: a
	// misspelling that silently means `datagram` is a deployment that thinks it
	// said otherwise.
	CHECK_FALSE(ParseWireMode("Quic").has_value());
	CHECK_FALSE(ParseWireMode("").has_value());
	CHECK_FALSE(ParseWireMode("udp").has_value());
}
