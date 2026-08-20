// The wire codec, and the case that decides whether hand-rolled protocol code
// survives contact with somebody else's machine.
//
// **A frame arriving in two reads is the interesting one.** A unix socket
// carrying a two-hundred-byte payload delivers it whole almost every time, so a
// decoder that assumes it will passes every run on the machine it was written
// on. That case is written down here rather than waited for.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <discord/Frame.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("discord.frame")

using discord::DecodedFrame;
using discord::DecodeFrame;
using discord::DecodeResult;
using discord::EncodeFrame;
using discord::Opcode;

namespace {
	std::string TextOf(const std::vector<std::byte> &bytes, size_t from) {
		std::string text;
		for (size_t index = from; index < bytes.size(); index++) {
			text.push_back(static_cast<char>(bytes[index]));
		}
		return text;
	}

	uint32_t NumberAt(const std::vector<std::byte> &bytes, size_t at) {
		return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
			   (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
	}
}

TEST_CASE("a frame is an opcode, a length and a body, little-endian", "[discord][frame]") {
	const std::vector<std::byte> frame = EncodeFrame(Opcode::Handshake, R"({"v":1})");

	REQUIRE(frame.size() == discord::FRAME_HEADER_BYTES + 7);
	CHECK(NumberAt(frame, 0) == 0);
	CHECK(NumberAt(frame, 4) == 7);
	CHECK(TextOf(frame, discord::FRAME_HEADER_BYTES) == R"({"v":1})");
}

TEST_CASE("a frame decodes to what was encoded", "[discord][frame]") {
	std::vector<std::byte> buffer = EncodeFrame(Opcode::Frame, R"({"cmd":"SET_ACTIVITY"})");

	DecodedFrame decoded;
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Ok);
	CHECK(decoded.Op == Opcode::Frame);
	CHECK(decoded.Payload == R"({"cmd":"SET_ACTIVITY"})");

	// Consumed, so a buffer holding one frame is empty afterwards.
	CHECK(buffer.empty());
}

TEST_CASE("two frames in one buffer come out one at a time", "[discord][frame]") {
	std::vector<std::byte> buffer = EncodeFrame(Opcode::Frame, "one");
	const std::vector<std::byte> second = EncodeFrame(Opcode::Ping, "two");
	buffer.insert(buffer.end(), second.begin(), second.end());

	DecodedFrame decoded;
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Ok);
	CHECK(decoded.Payload == "one");
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Ok);
	CHECK(decoded.Op == Opcode::Ping);
	CHECK(decoded.Payload == "two");
	CHECK(DecodeFrame(buffer, decoded) == DecodeResult::Incomplete);
}

TEST_CASE("a frame split across two reads reassembles", "[discord][frame]") {
	const std::vector<std::byte> whole = EncodeFrame(Opcode::Frame, R"({"evt":"READY"})");

	// The split is inside the header, which is the worse of the two places it
	// can land: a decoder that reads the length before checking it has one
	// gets a number made of whatever follows in memory.
	std::vector<std::byte> buffer(whole.begin(), whole.begin() + 3);

	DecodedFrame decoded;
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Incomplete);
	CHECK(buffer.size() == 3);

	buffer.insert(buffer.end(), whole.begin() + 3, whole.end() - 4);
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Incomplete);

	buffer.insert(buffer.end(), whole.end() - 4, whole.end());
	REQUIRE(DecodeFrame(buffer, decoded) == DecodeResult::Ok);
	CHECK(decoded.Payload == R"({"evt":"READY"})");
}

TEST_CASE("a length past the cap is refused rather than allocated", "[discord][frame]") {
	std::vector<std::byte> buffer(discord::FRAME_HEADER_BYTES, std::byte{0});
	buffer[0] = std::byte{1};

	// A gigabyte. The point is that nothing tries to hold it.
	buffer[4] = std::byte{0x00};
	buffer[5] = std::byte{0x00};
	buffer[6] = std::byte{0x00};
	buffer[7] = std::byte{0x40};

	DecodedFrame decoded;
	CHECK(DecodeFrame(buffer, decoded) == DecodeResult::Corrupt);
}

TEST_CASE("an opcode outside the five is corrupt rather than cast", "[discord][frame]") {
	std::vector<std::byte> buffer(discord::FRAME_HEADER_BYTES, std::byte{0});
	buffer[0] = std::byte{9};

	DecodedFrame decoded;
	CHECK(DecodeFrame(buffer, decoded) == DecodeResult::Corrupt);
}

TEST_CASE("a payload too large to frame produces nothing", "[discord][frame]") {
	const std::string enormous(discord::MAXIMUM_FRAME_BYTES, 'x');
	CHECK(EncodeFrame(Opcode::Frame, enormous).empty());
}
