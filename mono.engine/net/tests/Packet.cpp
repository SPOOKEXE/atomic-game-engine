#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/net/Packet.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.packet")
TEST_DEPENDS("engine.core.bytes")
TEST_DEPENDS("engine.core.metrics")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::net::ChannelKind;
using engine::net::Packet;
using engine::net::PacketHeader;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	std::vector<std::byte> Framed(const PacketHeader &header, std::span<const std::byte> payload) {
		ByteWriter writer;
		REQUIRE(Packet::Write(writer, header, payload));
		const auto bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}

	PacketHeader Header() {
		PacketHeader header;
		header.Channel = ChannelKind::Reliable;
		header.Sequence = 1234;
		header.Acknowledge = 1200;
		header.AcknowledgeBits = 0xDEADBEEF;
		header.Counter = 0x0123456789ABCDEFull;
		return header;
	}
}

TEST_CASE("a packet round-trips", "[net][packet]") {
	const auto payload = Bytes("a door opened");
	const auto frame = Framed(Header(), payload);

	ByteReader reader(frame);
	const auto inbound = Packet::Read(reader);
	REQUIRE(inbound.has_value());

	CHECK(inbound->Header.Channel == ChannelKind::Reliable);
	CHECK(inbound->Header.Sequence == 1234);
	CHECK(inbound->Header.Acknowledge == 1200);
	CHECK(inbound->Header.AcknowledgeBits == 0xDEADBEEF);
	CHECK(inbound->Header.Counter == 0x0123456789ABCDEFull);
	REQUIRE(inbound->Payload.size() == payload.size());
	CHECK(std::equal(payload.begin(), payload.end(), inbound->Payload.begin()));
}

TEST_CASE("the header comes back as the bytes that arrived", "[net][packet]") {
	// **What the tag is computed over, and why it is handed back rather than
	// rebuilt.** A reader that re-serialised the parsed fields would agree with
	// the sender until the day it did not, and that day every packet is refused
	// and it reads as a dead network.
	const auto payload = Bytes("a door opened");
	const auto frame = Framed(Header(), payload);

	ByteReader reader(frame);
	const auto inbound = Packet::Read(reader);
	REQUIRE(inbound.has_value());

	REQUIRE(inbound->HeaderBytes.size() == Packet::HEADER_BYTES);
	CHECK(std::equal(inbound->HeaderBytes.begin(), inbound->HeaderBytes.end(), frame.begin()));

	// The two views are the whole datagram between them, with nothing counted
	// twice and nothing left out.
	CHECK(inbound->HeaderBytes.size() + inbound->Payload.size() == frame.size());
	CHECK(inbound->HeaderBytes.data() + inbound->HeaderBytes.size() == inbound->Payload.data());
}

TEST_CASE("a header written on its own is the header of a whole packet", "[net][packet]") {
	// The sender has to serialise the header before it has a payload, because
	// the payload is sealed *over* the header. The two paths must not be able to
	// drift apart, so this pins that they produce the same bytes.
	const auto payload = Bytes("a door opened");

	ByteWriter split;
	REQUIRE(Packet::WriteHeader(split, Header(), payload.size()));
	CHECK(split.Size() == Packet::HEADER_BYTES);
	split.WriteRaw(payload.data(), payload.size());

	const auto whole = Framed(Header(), payload);
	REQUIRE(split.Size() == whole.size());
	CHECK(std::equal(whole.begin(), whole.end(), split.Bytes().begin()));
}

TEST_CASE("the message limit is the payload limit less the tag", "[net][packet]") {
	// **The number every budget above this module is sized against.** A caller
	// hands over a message and a tag is added to it, so the two limits differ by
	// sixteen bytes - and a budget measured against the wrong one produces a
	// message that can never be sent and looks exactly like a busy link.
	CHECK(
		Packet::MAXIMUM_MESSAGE_BYTES + engine::net::Cipher::OVERHEAD_BYTES == Packet::MAXIMUM_PAYLOAD_BYTES
	);
	CHECK(Packet::HEADER_BYTES + Packet::MAXIMUM_PAYLOAD_BYTES == 1200);
}

TEST_CASE("the header is the size the constant claims", "[net][packet]") {
	// Paid on every packet, sixty times a second per player, so it is worth
	// pinning: a field added without thinking is bandwidth for the life of the
	// product.
	const auto frame = Framed(Header(), {});
	CHECK(frame.size() == Packet::HEADER_BYTES);
}

TEST_CASE("an empty payload is a valid packet", "[net][packet]") {
	// How a quiet connection stays alive: a packet carrying only an
	// acknowledgement.
	const auto frame = Framed(Header(), {});

	ByteReader reader(frame);
	const auto inbound = Packet::Read(reader);
	REQUIRE(inbound.has_value());
	CHECK(inbound->Payload.empty());
	CHECK(inbound->Header.Sequence == 1234);
}

TEST_CASE("an oversized payload is refused rather than truncated", "[net][packet]") {
	const std::vector<std::byte> huge(Packet::MAXIMUM_PAYLOAD_BYTES + 1, std::byte{0x41});

	ByteWriter writer;
	CHECK_FALSE(Packet::Write(writer, Header(), huge));

	// Nothing written. A frame saying one length and carrying another is the
	// exact shape the reader refuses, and producing one locally would be a bug
	// that only ever surfaces on the far side.
	CHECK(writer.Bytes().empty());
}

TEST_CASE("a payload at exactly the maximum is allowed", "[net][packet]") {
	const std::vector<std::byte> full(Packet::MAXIMUM_PAYLOAD_BYTES, std::byte{0x42});
	const auto frame = Framed(Header(), full);

	ByteReader reader(frame);
	const auto inbound = Packet::Read(reader);
	REQUIRE(inbound.has_value());
	CHECK(inbound->Payload.size() == Packet::MAXIMUM_PAYLOAD_BYTES);
}

TEST_CASE("a wrong magic or version is refused", "[net][packet]") {
	auto frame = Framed(Header(), Bytes("payload"));

	auto badMagic = frame;
	badMagic[0] = static_cast<std::byte>(0xFF);
	ByteReader magicReader(badMagic);
	CHECK_FALSE(Packet::Read(magicReader).has_value());

	// Refused rather than negotiated downward. A server speaking an old version
	// to an old client is a server running two protocols, and the second one is
	// the one nobody tests.
	auto badVersion = frame;
	badVersion[4] = static_cast<std::byte>(0x99);
	ByteReader versionReader(badVersion);
	CHECK_FALSE(Packet::Read(versionReader).has_value());
}

TEST_CASE("a channel outside the enum is refused", "[net][packet]") {
	auto frame = Framed(Header(), Bytes("payload"));

	// Casting it anyway would produce a ChannelKind no switch handles, and every
	// Describe and every dispatch downstream would be reading a value the type
	// says cannot exist.
	frame[6] = static_cast<std::byte>(7);
	ByteReader reader(frame);
	CHECK_FALSE(Packet::Read(reader).has_value());
}

TEST_CASE("a channel can be read before the rest of the packet", "[net][packet]") {
	// For the router that has to decide where a datagram goes before it has a
	// connection to hand it to. A handshake datagram comes from an address that
	// is by definition not in the connection table yet.
	auto frame = Framed(Header(), Bytes("payload"));
	CHECK(Packet::PeekChannel(frame) == ChannelKind::Reliable);

	// The same three refusals `Read` makes, because a router acting on a value
	// outside the enum is the one place a byte from a stranger picks a code path
	// the type says cannot exist.
	auto badMagic = frame;
	badMagic[0] = static_cast<std::byte>(0xFF);
	CHECK_FALSE(Packet::PeekChannel(badMagic).has_value());

	auto badVersion = frame;
	badVersion[4] = static_cast<std::byte>(0x99);
	CHECK_FALSE(Packet::PeekChannel(badVersion).has_value());

	auto badChannel = frame;
	badChannel[6] = static_cast<std::byte>(7);
	CHECK_FALSE(Packet::PeekChannel(badChannel).has_value());

	// And a datagram too short to hold a header at all, which is what a router
	// sees before anything has checked a length.
	CHECK_FALSE(Packet::PeekChannel(std::span<const std::byte>(frame).first(4)).has_value());
	CHECK_FALSE(Packet::PeekChannel({}).has_value());
}

TEST_CASE("a length that runs past the buffer is refused", "[net][packet]") {
	const auto frame = Framed(Header(), Bytes("payload"));

	// Every truncation. The reader answers zero past the end rather than
	// throwing, so a short packet that is not checked parses as one carrying
	// nothing.
	for (size_t length = 0; length < frame.size(); ++length) {
		INFO("truncated to " << length);
		ByteReader reader(std::span<const std::byte>(frame).first(length));
		CHECK_FALSE(Packet::Read(reader).has_value());
	}
}

TEST_CASE("a length field claiming more than arrived is refused", "[net][packet]") {
	auto frame = Framed(Header(), Bytes("eight!!!"));

	// The length is the last header field, little-endian. Claim far more
	// payload than the buffer holds.
	frame[Packet::HEADER_BYTES - 2] = static_cast<std::byte>(0xFF);
	frame[Packet::HEADER_BYTES - 1] = static_cast<std::byte>(0x00);

	ByteReader reader(frame);
	CHECK_FALSE(Packet::Read(reader).has_value());
}

TEST_CASE("a refusal marks the reader failed", "[net][packet]") {
	auto frame = Framed(Header(), Bytes("payload"));
	frame[0] = static_cast<std::byte>(0xFF);

	ByteReader reader(frame);
	CHECK_FALSE(Packet::Read(reader).has_value());
	CHECK(reader.Failed());
}

TEST_CASE("sequence comparison survives the wrap", "[net][packet]") {
	CHECK(Packet::IsNewer(2, 1));
	CHECK_FALSE(Packet::IsNewer(1, 2));
	CHECK_FALSE(Packet::IsNewer(5, 5));

	// The case a plain `>` gets wrong. A 16-bit counter wraps every 65536
	// packets - about eighteen minutes at sixty a second, well inside one match
	// - and without this every packet after the first wrap is discarded as old.
	CHECK(Packet::IsNewer(0, 65535));
	CHECK(Packet::IsNewer(3, 65533));
	CHECK_FALSE(Packet::IsNewer(65535, 0));
	CHECK_FALSE(Packet::IsNewer(65533, 3));

	// Half a range apart is where the answer stops being meaningful, and the
	// rule has to at least be consistent there rather than answering true both
	// ways.
	CHECK(Packet::IsNewer(32768, 0));
	CHECK_FALSE(Packet::IsNewer(0, 32768));
}

TEST_CASE("packets are counted", "[net][packet][metrics]") {
	using engine::core::Metrics;

	auto frame = Framed(Header(), Bytes("payload"));
	auto broken = frame;
	broken[0] = static_cast<std::byte>(0xFF);

	Metrics::Clear();
	ByteReader good(frame);
	CHECK(Packet::Read(good).has_value());
	ByteReader bad(broken);
	CHECK_FALSE(Packet::Read(bad).has_value());

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// A refusal is counted apart from ordinary loss: a rate that climbs here is
	// somebody probing the port or two builds disagreeing about the format, and
	// neither reads anything like a lossy network.
	CHECK(total("net.packet.read") == 1.0);
	CHECK(total("net.packet.refused") == 1.0);
}
