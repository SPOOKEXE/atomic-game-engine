#include <engine/core/Bytes.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.net.transport")
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.core.bytes")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::net::AddressFamily;
using engine::net::ChannelKind;
using engine::net::Endpoint;
using engine::net::Packet;
using engine::net::PacketHeader;
using engine::net::Transport;
using engine::net::TransportSettings;
using engine::net::TransportStatus;

namespace {
	// Two ends of one network, whichever implementation built them.
	//
	// Every case below is written against this rather than against a concrete
	// transport, because the whole claim of the interface is that a caller
	// cannot tell the two apart. A case that only ran on the loopback would be a
	// case the socket is free to fail.
	struct Pair {
		std::unique_ptr<Transport> First;
		std::unique_ptr<Transport> Second;

		// Whether a network was available at all. False only for the socket, on
		// a machine that could not create one.
		bool Usable() const {
			return First && Second;
		}
	};

	Pair Loopback(const TransportSettings &settings = {}) {
		std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(2, settings);
		REQUIRE(ends.size() == 2);
		return {std::move(ends[0]), std::move(ends[1])};
	}

	// An ephemeral port on each end, so two runs of the suite at once do not
	// collide. Returns an unusable pair rather than failing when no socket can
	// be created — a build machine with no network should skip this, not fail.
	Pair Udp(const TransportSettings &settings = {}) {
		Pair pair{engine::net::MakeUdpTransport(0, settings), engine::net::MakeUdpTransport(0, settings)};
		if (!pair.Usable()) {
			return {};
		}
		return pair;
	}

	// The socket binds every interface, so its own address is 0.0.0.0 and a peer
	// has to be addressed over the loopback interface instead. The loopback
	// transport's address is already the one to send to.
	Endpoint Reachable(const Transport &transport) {
		const Endpoint local = transport.Local();
		return Endpoint::LoopbackIPv4(local.Port);
	}

	// Bounded, so a machine whose loopback interface is not up fails the case in
	// a quarter of a second rather than hanging the suite. The loopback
	// transport delivers within the call and never sleeps here.
	Transport::Inbound Await(Transport &transport, std::vector<std::byte> &datagram) {
		constexpr int ATTEMPTS = 250;
		for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
			const Transport::Inbound inbound = transport.Receive(datagram);
			if (inbound.Status != TransportStatus::Empty) {
				return inbound;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return {TransportStatus::Empty, {}};
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes;
		bytes.reserve(text.size());
		for (const char character : text) {
			bytes.push_back(static_cast<std::byte>(character));
		}
		return bytes;
	}

	// --- the cases both implementations answer ------------------------------

	void RoundTrip(Pair &pair) {
		const std::vector<std::byte> sent = Bytes("the first thing said");
		REQUIRE(pair.First->Send(Reachable(*pair.Second), sent) == TransportStatus::Ok);

		std::vector<std::byte> received;
		const Transport::Inbound inbound = Await(*pair.Second, received);

		CHECK(inbound.Status == TransportStatus::Ok);
		CHECK(received == sent);

		// The reply goes back to where the datagram came from, which is the only
		// address the receiving end has for a peer it has never heard of.
		REQUIRE(pair.Second->Send(inbound.From, Bytes("and the answer")) == TransportStatus::Ok);

		std::vector<std::byte> answer;
		CHECK(Await(*pair.First, answer).Status == TransportStatus::Ok);
		CHECK(answer == Bytes("and the answer"));
	}

	void RefusesOversized(Pair &pair) {
		const std::vector<std::byte> oversized(Transport::MAXIMUM_DATAGRAM_BYTES + 1, std::byte{0xAB});
		CHECK(pair.First->Send(Reachable(*pair.Second), oversized) == TransportStatus::TooLarge);

		// Refused whole, not truncated: nothing arrives at all.
		std::vector<std::byte> received;
		CHECK(pair.Second->Receive(received).Status == TransportStatus::Empty);

		// One byte under is the largest that fits, and it goes.
		const std::vector<std::byte> largest(Transport::MAXIMUM_DATAGRAM_BYTES, std::byte{0xCD});
		REQUIRE(pair.First->Send(Reachable(*pair.Second), largest) == TransportStatus::Ok);
		CHECK(Await(*pair.Second, received).Status == TransportStatus::Ok);
		CHECK(received.size() == Transport::MAXIMUM_DATAGRAM_BYTES);
	}

	void UnknownEndpoint(Pair &pair) {
		// A datagram from a peer this end has never heard of is delivered, and
		// the sender is named. Filtering it here would put the connection table
		// in the transport, which is the layer above's job.
		const Endpoint sender = Reachable(*pair.First);
		REQUIRE(pair.First->Send(Reachable(*pair.Second), Bytes("hello")) == TransportStatus::Ok);

		std::vector<std::byte> received;
		const Transport::Inbound inbound = Await(*pair.Second, received);
		CHECK(inbound.Status == TransportStatus::Ok);
		CHECK(inbound.From == sender);
		CHECK(inbound.From.Family == AddressFamily::IPv4);

		// Sending somewhere nobody is listening is `Ok` and dropped, because
		// that is what the network does. Anything else and single-player would
		// branch on a failure the socket never reports.
		CHECK(pair.First->Send(Endpoint::LoopbackIPv4(9), Bytes("into the void")) == TransportStatus::Ok);

		// An endpoint that names nowhere at all is the caller's mistake.
		CHECK(pair.First->Send(Endpoint{}, Bytes("nowhere")) == TransportStatus::Unreachable);
	}

	void ClosedRefuses(Pair &pair) {
		pair.First->Close();
		CHECK_FALSE(pair.First->Open());
		CHECK_FALSE(pair.First->Local().IsValid());

		CHECK(pair.First->Send(Reachable(*pair.Second), Bytes("too late")) == TransportStatus::Closed);

		std::vector<std::byte> received;
		CHECK(pair.First->Receive(received).Status == TransportStatus::Closed);

		// Closing twice is not an error. A transport is closed by whoever
		// notices first and again by its destructor.
		pair.First->Close();
		CHECK_FALSE(pair.First->Open());

		// The far side is not told. A sender on a real network gets no signal
		// that a port went away, so neither does this one.
		CHECK(pair.Second->Open());
	}

	// The claim §16.6 rests on: the loopback carries the same bytes through the
	// same framing as the socket, so single-player is not a path that skips it.
	void CarriesRealFraming(Pair &pair) {
		PacketHeader header;
		header.Channel = ChannelKind::Reliable;
		header.Sequence = 4113;
		header.Acknowledge = 4096;
		header.AcknowledgeBits = 0xF00DBEEFu;

		const std::vector<std::byte> payload = Bytes("a door opened");

		ByteWriter writer;
		REQUIRE(Packet::Write(writer, header, payload));
		REQUIRE(pair.First->Send(Reachable(*pair.Second), writer.Bytes()) == TransportStatus::Ok);

		std::vector<std::byte> received;
		REQUIRE(Await(*pair.Second, received).Status == TransportStatus::Ok);
		CHECK(received.size() == Packet::HEADER_BYTES + payload.size());

		ByteReader reader{received};
		const std::optional<Packet::Inbound> parsed = Packet::Read(reader);
		REQUIRE(parsed.has_value());
		CHECK(parsed->Header.Channel == ChannelKind::Reliable);
		CHECK(parsed->Header.Sequence == 4113);
		CHECK(parsed->Header.Acknowledge == 4096);
		CHECK(parsed->Header.AcknowledgeBits == 0xF00DBEEFu);
		CHECK(std::vector<std::byte>(parsed->Payload.begin(), parsed->Payload.end()) == payload);
	}
}

TEST_CASE("the loopback carries datagrams", "[net][transport]") {
	Pair pair = Loopback();

	SECTION("a datagram makes the round trip") {
		RoundTrip(pair);
	}
	SECTION("an oversized datagram is refused whole") {
		RefusesOversized(pair);
	}
	SECTION("a datagram from an unknown endpoint is delivered and named") {
		UnknownEndpoint(pair);
	}
	SECTION("a closed end refuses both directions") {
		ClosedRefuses(pair);
	}
	SECTION("real framing survives the hop") {
		CarriesRealFraming(pair);
	}
}

TEST_CASE("a udp socket carries the same datagrams", "[net][transport]") {
	Pair pair = Udp();
	if (!pair.Usable()) {
		// A machine with no network skips this rather than failing it. The
		// loopback cases above still cover the interface.
		WARN("no udp socket could be created; skipping the socket cases");
		return;
	}

	SECTION("a datagram makes the round trip") {
		RoundTrip(pair);
	}
	SECTION("an oversized datagram is refused whole") {
		RefusesOversized(pair);
	}
	SECTION("a datagram from an unknown endpoint is delivered and named") {
		UnknownEndpoint(pair);
	}
	SECTION("a closed end refuses both directions") {
		ClosedRefuses(pair);
	}
	SECTION("real framing survives the hop") {
		CarriesRealFraming(pair);
	}
}

TEST_CASE("a bound receive queue refuses rather than blocking", "[net][transport]") {
	// Small enough that a handful of datagrams fill it. The socket's buffer size
	// is advisory — a kernel rounds it — so this is stated on the loopback,
	// where the cap is exact.
	TransportSettings settings;
	settings.ReceiveQueueBytes = 64;
	Pair pair = Loopback(settings);

	const std::vector<std::byte> datagram(16, std::byte{0x7F});
	const Endpoint target = Reachable(*pair.Second);

	for (int index = 0; index < 4; ++index) {
		REQUIRE(pair.First->Send(target, datagram) == TransportStatus::Ok);
	}
	CHECK(pair.First->Send(target, datagram) == TransportStatus::Full);

	// Draining one makes room for one. The refusal is back pressure, not a
	// permanent state.
	std::vector<std::byte> received;
	REQUIRE(pair.Second->Receive(received).Status == TransportStatus::Ok);
	CHECK(pair.First->Send(target, datagram) == TransportStatus::Ok);
}

TEST_CASE("a loopback network routes by endpoint", "[net][transport]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	REQUIRE(ends.size() == 3);

	// Distinct addresses, which is what makes routing mean anything.
	CHECK(ends[0]->Local() != ends[1]->Local());
	CHECK(ends[1]->Local() != ends[2]->Local());

	REQUIRE(ends[0]->Send(ends[2]->Local(), Bytes("for the third")) == TransportStatus::Ok);

	std::vector<std::byte> received;
	CHECK(ends[1]->Receive(received).Status == TransportStatus::Empty);

	const Transport::Inbound inbound = ends[2]->Receive(received);
	CHECK(inbound.Status == TransportStatus::Ok);
	CHECK(inbound.From == ends[0]->Local());
	CHECK(received == Bytes("for the third"));
}

TEST_CASE("an endpoint is a value that survives its own text", "[net][transport]") {
	const Endpoint address = Endpoint::LoopbackIPv4(7777);
	CHECK(address.IsValid());
	CHECK(address.Text() == "127.0.0.1:7777");
	CHECK(Endpoint::Parse("127.0.0.1:7777") == address);

	const Endpoint six = Endpoint::FromIPv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 7777);
	CHECK(six.Text() == "[::1]:7777");
	CHECK(Endpoint::Parse("[::1]:7777") == six);

	// A v4 and a v6 address are never the same endpoint, whatever their bytes.
	CHECK_FALSE(address == six);

	CHECK_FALSE(Endpoint{}.IsValid());
	CHECK(Endpoint{}.Text() == "none");

	// Hostile text is refused rather than half-read: no port, a port past what
	// a port holds, an unbracketed v6 address whose last colon is ambiguous, a
	// host name that would need a blocking lookup, and trailing rubbish.
	CHECK_FALSE(Endpoint::Parse("127.0.0.1").has_value());
	CHECK_FALSE(Endpoint::Parse("127.0.0.1:70000").has_value());
	CHECK_FALSE(Endpoint::Parse("::1:7777").has_value());
	CHECK_FALSE(Endpoint::Parse("localhost:7777").has_value());
	CHECK_FALSE(Endpoint::Parse("127.0.0.1:77x").has_value());
	CHECK_FALSE(Endpoint::Parse("").has_value());
}
