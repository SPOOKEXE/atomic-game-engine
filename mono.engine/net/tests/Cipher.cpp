#include <engine/core/Metrics.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.cipher")
TEST_DEPENDS("engine.core.metrics")

using engine::net::Cipher;
using engine::net::Handshake;
using engine::net::HandshakeRole;

namespace {
	// Test vectors are published as hex and are transcribed here as hex, so that
	// a row can be compared against the RFC by eye rather than by trusting a
	// conversion somebody did once.
	std::vector<std::byte> Hex(std::string_view text) {
		std::vector<std::byte> bytes;
		bytes.reserve(text.size() / 2);
		const auto value = [](char digit) {
			if (digit >= '0' && digit <= '9') {
				return digit - '0';
			}
			return (digit - 'a') + 10;
		};
		for (size_t index = 0; index + 1 < text.size(); index += 2) {
			bytes.push_back(static_cast<std::byte>(value(text[index]) * 16 + value(text[index + 1])));
		}
		return bytes;
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	std::vector<std::byte> Copy(std::span<const std::byte> bytes) {
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}

	// Both ends of one completed agreement.
	//
	// A Sealer has no constructor taking a key — that is the point of the type —
	// so every sealing test starts from a handshake, and the handshake is driven
	// from fixed secrets so that a failure reproduces.
	struct Ends {
		Handshake::Session Initiator;
		Handshake::Session Responder;
	};

	Ends Agree(uint8_t initiatorSeed, uint8_t responderSeed) {
		std::array<std::byte, Handshake::SECRET_BYTES> initiatorSecret{};
		std::array<std::byte, Handshake::SECRET_BYTES> responderSecret{};
		initiatorSecret.fill(static_cast<std::byte>(initiatorSeed));
		responderSecret.fill(static_cast<std::byte>(responderSeed));

		auto initiator = Handshake::BeginFromSecret(HandshakeRole::Initiator, initiatorSecret);
		auto responder = Handshake::BeginFromSecret(HandshakeRole::Responder, responderSecret);
		REQUIRE(initiator.has_value());
		REQUIRE(responder.has_value());

		const auto fromInitiator = Copy(initiator->Message());
		const auto fromResponder = Copy(responder->Message());
		REQUIRE(initiator->Consume(fromResponder));
		REQUIRE(responder->Consume(fromInitiator));

		auto initiatorKeys = initiator->TakeKeys();
		auto responderKeys = responder->TakeKeys();
		REQUIRE(initiatorKeys.has_value());
		REQUIRE(responderKeys.has_value());
		return Ends{std::move(*initiatorKeys), std::move(*responderKeys)};
	}
}

TEST_CASE("a frame round-trips", "[net][cipher]") {
	auto ends = Agree(1, 2);
	const auto plaintext = Bytes("a door opened");
	const auto header = Bytes("channel 0, sequence 7");

	const auto sealed = ends.Initiator.Sending.Seal(plaintext, header);
	REQUIRE(sealed.has_value());
	CHECK(sealed->Counter == 0);
	CHECK(sealed->Bytes.size() == plaintext.size() + Cipher::TAG_BYTES);

	// The plaintext is not in the frame. Worth stating: an AEAD that authenticates
	// without encrypting would pass every other test in this file.
	CHECK(
		std::search(sealed->Bytes.begin(), sealed->Bytes.end(), plaintext.begin(), plaintext.end()) ==
		sealed->Bytes.end()
	);

	const auto frame = Copy(sealed->Bytes);
	const auto opened = ends.Responder.Receiving.Open(sealed->Counter, frame, header);
	REQUIRE(opened.has_value());
	CHECK(std::equal(plaintext.begin(), plaintext.end(), opened->begin(), opened->end()));
}

TEST_CASE("an empty payload is a valid frame", "[net][cipher]") {
	// How a packet carrying only an authenticated header goes out.
	auto ends = Agree(3, 4);
	const auto header = Bytes("only a header");

	const auto sealed = ends.Initiator.Sending.Seal({}, header);
	REQUIRE(sealed.has_value());
	CHECK(sealed->Bytes.size() == Cipher::TAG_BYTES);

	const auto frame = Copy(sealed->Bytes);
	const auto opened = ends.Responder.Receiving.Open(sealed->Counter, frame, header);
	REQUIRE(opened.has_value());
	CHECK(opened->empty());
}

TEST_CASE("a tampered frame is refused", "[net][cipher]") {
	auto ends = Agree(5, 6);
	const auto plaintext = Bytes("the door is at 12.5, 3.0");
	const auto header = Bytes("channel 1, sequence 40");

	const auto sealed = ends.Initiator.Sending.Seal(plaintext, header);
	REQUIRE(sealed.has_value());
	const auto frame = Copy(sealed->Bytes);

	// Every byte, ciphertext and tag alike. One flipped bit anywhere has to fail
	// authentication, and a test that only flips the first byte would pass
	// against an implementation that authenticates a prefix.
	for (size_t index = 0; index < frame.size(); ++index) {
		INFO("flipped byte " << index);
		auto tampered = frame;
		tampered[index] ^= std::byte{0x01};
		CHECK_FALSE(ends.Responder.Receiving.Open(sealed->Counter, tampered, header).has_value());
	}

	// And the untampered frame still opens, so the loop above was not passing
	// because nothing opens.
	CHECK(ends.Responder.Receiving.Open(sealed->Counter, frame, header).has_value());
}

TEST_CASE("a truncated frame is refused", "[net][cipher]") {
	auto ends = Agree(7, 8);
	const auto plaintext = Bytes("a shortened message");

	const auto sealed = ends.Initiator.Sending.Seal(plaintext, {});
	REQUIRE(sealed.has_value());
	const auto frame = Copy(sealed->Bytes);

	// Including the lengths below the tag, where the length arithmetic would
	// wrap if it were not checked first.
	for (size_t length = 0; length < frame.size(); ++length) {
		INFO("truncated to " << length);
		CHECK_FALSE(ends.Responder.Receiving
						.Open(sealed->Counter, std::span<const std::byte>(frame).first(length), {})
						.has_value());
	}
}

TEST_CASE("the wrong key is refused", "[net][cipher]") {
	auto ends = Agree(9, 10);
	auto other = Agree(11, 12);

	const auto plaintext = Bytes("for one session only");
	const auto sealed = ends.Initiator.Sending.Seal(plaintext, {});
	REQUIRE(sealed.has_value());
	const auto frame = Copy(sealed->Bytes);

	// A second, independently agreed session. Nothing from one opens in the
	// other, which is what makes a recorded session useless against the next one.
	CHECK_FALSE(other.Responder.Receiving.Open(sealed->Counter, frame, {}).has_value());

	// The sender's own receiving key is a different key too — the two directions
	// do not share one. A build that wired them together would pass every test
	// above and fail this one.
	CHECK_FALSE(ends.Initiator.Receiving.Open(sealed->Counter, frame, {}).has_value());
}

TEST_CASE("a wrong counter or rewritten header is refused", "[net][cipher]") {
	auto ends = Agree(13, 14);
	const auto plaintext = Bytes("position update");
	const auto header = Bytes("channel 0, sequence 9");

	const auto sealed = ends.Initiator.Sending.Seal(plaintext, header);
	REQUIRE(sealed.has_value());
	const auto frame = Copy(sealed->Bytes);

	// The counter is the nonce, so claiming a different one is claiming a
	// different frame.
	CHECK_FALSE(ends.Responder.Receiving.Open(sealed->Counter + 1, frame, header).has_value());

	// The header rides as associated data precisely so that rewriting it in
	// flight is caught here rather than acted on upstairs.
	CHECK_FALSE(
		ends.Responder.Receiving.Open(sealed->Counter, frame, Bytes("channel 0, sequence 8")).has_value()
	);
	CHECK_FALSE(ends.Responder.Receiving.Open(sealed->Counter, frame, {}).has_value());
}

TEST_CASE("the counter advances once per frame", "[net][cipher]") {
	auto ends = Agree(15, 16);
	const auto plaintext = Bytes("the same bytes every time");

	const auto first = ends.Initiator.Sending.Seal(plaintext, {});
	REQUIRE(first.has_value());
	const auto firstFrame = Copy(first->Bytes);

	const auto second = ends.Initiator.Sending.Seal(plaintext, {});
	REQUIRE(second.has_value());
	const auto secondFrame = Copy(second->Bytes);

	CHECK(first->Counter == 0);
	CHECK(second->Counter == 1);

	// The same plaintext under the same key twice. Identical frames would mean
	// the nonce had not moved, which is the failure this type is built around.
	CHECK(firstFrame != secondFrame);

	CHECK(ends.Responder.Receiving.Open(first->Counter, firstFrame, {}).has_value());
	CHECK(ends.Responder.Receiving.Open(second->Counter, secondFrame, {}).has_value());
}

TEST_CASE("a moved-from sealer seals nothing", "[net][cipher]") {
	auto ends = Agree(17, 18);

	Cipher::Sealer moved = std::move(ends.Initiator.Sending);
	const auto fromMoved = ends.Initiator.Sending.Seal(Bytes("after the move"), {});

	// Not an empty frame and not a zero-key frame: nothing at all. A second
	// object counting from the same place under the same key is the only way a
	// caller could make a nonce repeat, so the source stops being one.
	CHECK_FALSE(fromMoved.has_value());

	// The destination carries on from where the source was.
	const auto sealed = moved.Seal(Bytes("after the move"), {});
	REQUIRE(sealed.has_value());
	CHECK(sealed->Counter == 0);
}

TEST_CASE("an opener refuses key material of the wrong length", "[net][cipher]") {
	const std::array<std::byte, Cipher::KEY_BYTES> key{};
	const std::array<std::byte, Cipher::NONCE_PREFIX_BYTES> prefix{};

	CHECK(Cipher::Opener::FromKey(key, prefix).has_value());
	CHECK_FALSE(Cipher::Opener::FromKey(std::span<const std::byte>(key).first(31), prefix).has_value());
	CHECK_FALSE(Cipher::Opener::FromKey(key, std::span<const std::byte>(prefix).first(3)).has_value());
	CHECK_FALSE(Cipher::Opener::FromKey({}, {}).has_value());
}

TEST_CASE("RFC 8439 section 2.8.2 opens to the published plaintext", "[net][cipher]") {
	// The AEAD_CHACHA20_POLY1305 example vector, verbatim. An implementation
	// tested only against itself is consistent and can still be wrong in every
	// direction at once — a swapped endianness in the block counter, a Poly1305
	// key derived from the wrong block, an AAD that is padded differently. None
	// of those survive a published vector.
	//
	// The RFC builds the 96-bit nonce as a 32-bit fixed part (07 00 00 00) and a
	// 64-bit counter (40 41 42 43 44 45 46 47), which is exactly the split this
	// file's Sealer and Opener use, so the vector maps onto the API with nothing
	// bent to fit.
	const auto key = Hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
	const auto noncePrefix = Hex("07000000");
	const uint64_t counter = 0x4041424344454647ULL;
	const auto associatedData = Hex("50515253c0c1c2c3c4c5c6c7");
	const auto sealed =
		Hex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
			"3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
			"92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
			"3ff4def08e4b7a9de576d26586cec64b6116"
			// The tag, which the RFC prints separately.
			"1ae10b594f09e26a7e902ecbd0600691");
	const auto expected = Bytes(
		"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
		"sunscreen would be it."
	);

	auto opener = Cipher::Opener::FromKey(key, noncePrefix);
	REQUIRE(opener.has_value());

	const auto opened = opener->Open(counter, sealed, associatedData);
	REQUIRE(opened.has_value());
	CHECK(std::equal(expected.begin(), expected.end(), opened->begin(), opened->end()));

	// The same vector with one bit of the tag moved. The vector proves the
	// keystream; this proves the tag is checked rather than carried.
	auto forged = sealed;
	forged.back() ^= std::byte{0x80};
	CHECK_FALSE(opener->Open(counter, forged, associatedData).has_value());
}

TEST_CASE("frames are counted", "[net][cipher][metrics]") {
	using engine::core::Metrics;

	auto ends = Agree(19, 20);
	const auto sealed = ends.Initiator.Sending.Seal(Bytes("counted"), {});
	REQUIRE(sealed.has_value());
	auto frame = Copy(sealed->Bytes);

	Metrics::Clear();
	CHECK(ends.Responder.Receiving.Open(sealed->Counter, frame, {}).has_value());
	frame[0] ^= std::byte{0xFF};
	CHECK_FALSE(ends.Responder.Receiving.Open(sealed->Counter, frame, {}).has_value());

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

	// A refusal here is not ordinary loss. A rate that climbs means somebody is
	// forging frames or two builds disagree about what is authenticated, and
	// neither reads anything like a lossy network.
	CHECK(total("net.cipher.opened") == 1.0);
	CHECK(total("net.cipher.refused") == 1.0);
}
