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

TEST_SUITE_ID("engine.net.handshake")
TEST_DEPENDS("engine.net.cipher")
TEST_DEPENDS("engine.core.metrics")

using engine::net::Cipher;
using engine::net::Handshake;
using engine::net::HandshakeRole;
using engine::net::HandshakeState;

namespace {
	// Published vectors are hex, so they are transcribed as hex and converted
	// here. The same ten lines as the cipher suite's, which is the price of a
	// suite being one file the runner can re-run on its own.
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

	std::array<std::byte, Handshake::SECRET_BYTES> Secret(uint8_t fill) {
		std::array<std::byte, Handshake::SECRET_BYTES> secret{};
		secret.fill(static_cast<std::byte>(fill));
		return secret;
	}

	Handshake Begin(HandshakeRole role, uint8_t fill) {
		auto handshake = Handshake::BeginFromSecret(role, Secret(fill));
		REQUIRE(handshake.has_value());
		return std::move(*handshake);
	}

	// Whether a sealed frame from one side opens on the other.
	bool Carries(Cipher::Sealer &sealer, Cipher::Opener &opener, std::string_view text) {
		const auto plaintext = Bytes(text);
		const auto sealed = sealer.Seal(plaintext, {});
		REQUIRE(sealed.has_value());
		const auto frame = Copy(sealed->Bytes);

		const auto opened = opener.Open(sealed->Counter, frame, {});
		if (!opened.has_value()) {
			return false;
		}
		return std::equal(plaintext.begin(), plaintext.end(), opened->begin(), opened->end());
	}
}

TEST_CASE("two handshakes run apart agree on the same keys", "[net][handshake]") {
	auto initiator = Begin(HandshakeRole::Initiator, 1);
	auto responder = Begin(HandshakeRole::Responder, 2);

	CHECK(initiator.State() == HandshakeState::AwaitingPeer);
	CHECK(initiator.Message().size() == Handshake::MESSAGE_BYTES);

	// Neither side is told anything but the other's message, which is the whole
	// contract: two objects that never see each other's state end up able to
	// talk.
	const auto fromInitiator = Copy(initiator.Message());
	const auto fromResponder = Copy(responder.Message());
	REQUIRE(initiator.Consume(fromResponder));
	REQUIRE(responder.Consume(fromInitiator));

	auto one = initiator.TakeKeys();
	auto two = responder.TakeKeys();
	REQUIRE(one.has_value());
	REQUIRE(two.has_value());

	CHECK(Carries(one->Sending, two->Receiving, "client to server"));
	CHECK(Carries(two->Sending, one->Receiving, "server to client"));
}

TEST_CASE("the two directions get different keys", "[net][handshake]") {
	auto initiator = Begin(HandshakeRole::Initiator, 3);
	auto responder = Begin(HandshakeRole::Responder, 4);
	const auto fromInitiator = Copy(initiator.Message());
	const auto fromResponder = Copy(responder.Message());
	REQUIRE(initiator.Consume(fromResponder));
	REQUIRE(responder.Consume(fromInitiator));

	auto one = *initiator.TakeKeys();
	auto two = *responder.TakeKeys();

	// One key both ways would be the easy mistake and a serious one: both ends
	// start their counter at zero, so the first frame each way would share a key
	// and a nonce and leak the XOR of the two plaintexts.
	CHECK_FALSE(Carries(one.Sending, one.Receiving, "back to myself"));
	CHECK_FALSE(Carries(two.Sending, two.Receiving, "back to myself"));
}

TEST_CASE("a second session shares nothing with the first", "[net][handshake]") {
	auto firstInitiator = Begin(HandshakeRole::Initiator, 5);
	auto firstResponder = Begin(HandshakeRole::Responder, 6);
	REQUIRE(firstInitiator.Consume(Copy(firstResponder.Message())));
	REQUIRE(firstResponder.Consume(Copy(firstInitiator.Message())));

	auto secondInitiator = Begin(HandshakeRole::Initiator, 7);
	auto secondResponder = Begin(HandshakeRole::Responder, 8);
	REQUIRE(secondInitiator.Consume(Copy(secondResponder.Message())));
	REQUIRE(secondResponder.Consume(Copy(secondInitiator.Message())));

	auto first = *firstInitiator.TakeKeys();
	auto second = *secondResponder.TakeKeys();

	// Different ephemeral pairs, different keys. This is what makes a recording
	// of one session worthless against the next, and what a configured shared
	// key would give up.
	CHECK_FALSE(Carries(first.Sending, second.Receiving, "not for you"));
}

TEST_CASE("a garbage peer message is refused rather than producing a key", "[net][handshake]") {
	SECTION("a message one byte short") {
		auto initiator = Begin(HandshakeRole::Initiator, 10);
		CHECK_FALSE(initiator.Consume(std::vector<std::byte>(31, std::byte{0x11})));
		CHECK(initiator.State() == HandshakeState::Refused);
		CHECK_FALSE(initiator.TakeKeys().has_value());
	}

	SECTION("an empty message") {
		auto initiator = Begin(HandshakeRole::Initiator, 11);
		CHECK_FALSE(initiator.Consume({}));
		CHECK(initiator.State() == HandshakeState::Refused);
		CHECK_FALSE(initiator.TakeKeys().has_value());
	}

	SECTION("a message that is one byte too long") {
		auto initiator = Begin(HandshakeRole::Initiator, 12);
		CHECK_FALSE(initiator.Consume(std::vector<std::byte>(33, std::byte{0x22})));
		CHECK(initiator.State() == HandshakeState::Refused);
	}

	SECTION("a low-order point") {
		// The all-zero key, and the two other points of small order that RFC 7748
		// §6.1 has implementations check for. Every one of them agrees to all
		// zeros against a clamped scalar, which would hand a peer a session key
		// it knows in full.
		for (const auto *hostile :
			 {"0000000000000000000000000000000000000000000000000000000000000000",
			  "0100000000000000000000000000000000000000000000000000000000000000",
			  "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"}) {
			INFO(hostile);
			auto initiator = Begin(HandshakeRole::Initiator, 13);
			CHECK_FALSE(initiator.Consume(Hex(hostile)));
			CHECK(initiator.State() == HandshakeState::Refused);
			CHECK_FALSE(initiator.TakeKeys().has_value());
		}
	}

	SECTION("our own key, reflected") {
		auto initiator = Begin(HandshakeRole::Initiator, 14);
		const auto mine = Copy(initiator.Message());
		CHECK_FALSE(initiator.Consume(mine));
		CHECK(initiator.State() == HandshakeState::Refused);
		CHECK_FALSE(initiator.TakeKeys().has_value());
	}
}

TEST_CASE("the lifecycle goes one way", "[net][handshake]") {
	auto initiator = Begin(HandshakeRole::Initiator, 15);
	auto responder = Begin(HandshakeRole::Responder, 16);
	const auto fromResponder = Copy(responder.Message());

	CHECK(initiator.Role() == HandshakeRole::Initiator);
	CHECK(initiator.State() == HandshakeState::AwaitingPeer);
	CHECK_FALSE(initiator.TakeKeys().has_value());

	REQUIRE(initiator.Consume(fromResponder));
	CHECK(initiator.State() == HandshakeState::Established);

	// Not idempotent, deliberately - the reason Link::CompleteHandshake is not
	// either. A second message is a replay or two code paths both owning the
	// transition, and the established session is left intact rather than torn
	// down over a duplicate packet.
	CHECK_FALSE(initiator.Consume(fromResponder));
	CHECK(initiator.State() == HandshakeState::Established);

	CHECK(initiator.TakeKeys().has_value());
	CHECK(initiator.State() == HandshakeState::Complete);

	// One Sealer per key, ever. A second one would count from zero under a key
	// that has already sealed frames.
	CHECK_FALSE(initiator.TakeKeys().has_value());
	CHECK_FALSE(initiator.Consume(fromResponder));
	CHECK(initiator.State() == HandshakeState::Complete);
}

TEST_CASE("a moved-from handshake yields nothing", "[net][handshake]") {
	auto initiator = Begin(HandshakeRole::Initiator, 17);
	auto responder = Begin(HandshakeRole::Responder, 18);
	REQUIRE(initiator.Consume(Copy(responder.Message())));

	Handshake moved = std::move(initiator);
	CHECK(initiator.State() == HandshakeState::Refused);
	CHECK_FALSE(initiator.TakeKeys().has_value());

	CHECK(moved.State() == HandshakeState::Established);
	CHECK(moved.TakeKeys().has_value());
}

TEST_CASE("a secret of the wrong length is refused", "[net][handshake]") {
	const std::array<std::byte, Handshake::SECRET_BYTES> secret{};
	CHECK(Handshake::BeginFromSecret(HandshakeRole::Initiator, secret).has_value());
	CHECK_FALSE(
		Handshake::BeginFromSecret(HandshakeRole::Initiator, std::span<const std::byte>(secret).first(31))
			.has_value()
	);
	CHECK_FALSE(Handshake::BeginFromSecret(HandshakeRole::Responder, {}).has_value());
}

TEST_CASE("Begin draws a fresh key pair every time", "[net][handshake]") {
	auto first = Handshake::Begin(HandshakeRole::Initiator);
	auto second = Handshake::Begin(HandshakeRole::Initiator);
	REQUIRE(first.has_value());
	REQUIRE(second.has_value());

	// Ephemeral is the whole point. Two handshakes sharing a key pair would mean
	// one recorded session decrypts the next.
	CHECK(Copy(first->Message()) != Copy(second->Message()));
}

TEST_CASE("RFC 7748 section 6.1 derives the published public keys", "[net][handshake]") {
	// Alice and Bob, verbatim. X25519 clamps the scalar, so these private keys -
	// which the RFC prints unclamped - must still produce exactly these public
	// keys; an implementation that forgot to clamp produces different ones and
	// still interoperates with itself.
	const auto alicePrivate = Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	const auto alicePublic = Hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
	const auto bobPrivate = Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
	const auto bobPublic = Hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

	auto alice = Handshake::BeginFromSecret(HandshakeRole::Initiator, alicePrivate);
	auto bob = Handshake::BeginFromSecret(HandshakeRole::Responder, bobPrivate);
	REQUIRE(alice.has_value());
	REQUIRE(bob.has_value());

	CHECK(Copy(alice->Message()) == alicePublic);
	CHECK(Copy(bob->Message()) == bobPublic);

	// And the agreement itself over the published pair. The shared secret never
	// leaves the handshake, so what is checked is that both sides reached the
	// same one.
	REQUIRE(alice->Consume(bobPublic));
	REQUIRE(bob->Consume(alicePublic));
	auto one = *alice->TakeKeys();
	auto two = *bob->TakeKeys();
	CHECK(Carries(one.Sending, two.Receiving, "RFC 7748 section 6.1"));
}

TEST_CASE("the derivation is pinned, because it is a wire format", "[net][handshake]") {
	// Two builds that derive keys differently do not fail to connect - they
	// connect and then refuse every frame, which is a much worse bug to be
	// looking at. So the whole chain is nailed to a value produced outside this
	// codebase: RFC 7748 §6.1's key pair, HKDF-SHA256 salted with
	// "atomic-net-handshake-v1", the transcript as info with the initiator's key
	// first, the 72 bytes split key-prefix-key-prefix, and the nonce as the
	// prefix followed by a big-endian counter.
	//
	// Changing any of those is a protocol change, and this test is where that
	// has to be said out loud rather than discovered by two clients.
	const auto alicePrivate = Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	const auto bobPrivate = Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

	auto alice = *Handshake::BeginFromSecret(HandshakeRole::Initiator, alicePrivate);
	auto bob = *Handshake::BeginFromSecret(HandshakeRole::Responder, bobPrivate);
	const auto fromAlice = Copy(alice.Message());
	const auto fromBob = Copy(bob.Message());
	REQUIRE(alice.Consume(fromBob));
	REQUIRE(bob.Consume(fromAlice));

	auto initiator = *alice.TakeKeys();
	auto responder = *bob.TakeKeys();

	const auto plaintext = Bytes("pinned by the wire format");
	const auto associatedData = Bytes("header");

	const auto fromInitiator = initiator.Sending.Seal(plaintext, associatedData);
	REQUIRE(fromInitiator.has_value());
	CHECK(
		Copy(fromInitiator->Bytes) ==
		Hex("809b1507b53427ec5b670f825060b4eaa123db3f14c779bfe3cfc9be80b9dd91c3beddf2adfe32d123")
	);

	const auto fromResponder = responder.Sending.Seal(plaintext, associatedData);
	REQUIRE(fromResponder.has_value());
	CHECK(
		Copy(fromResponder->Bytes) ==
		Hex("80d03317e12e0523688aa1b3681b09b7c8d167207b9aa1ad5a0e0910e3c5693a3477238eb6991dd128")
	);
}

TEST_CASE("handshakes are counted", "[net][handshake][metrics]") {
	using engine::core::Metrics;

	auto initiator = Begin(HandshakeRole::Initiator, 19);
	auto responder = Begin(HandshakeRole::Responder, 20);
	auto refused = Begin(HandshakeRole::Initiator, 21);

	Metrics::Clear();
	CHECK(initiator.Consume(Copy(responder.Message())));
	CHECK_FALSE(refused.Consume(std::vector<std::byte>(8, std::byte{0x33})));

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

	// A refusal rate that climbs is somebody probing the port with junk, or two
	// builds disagreeing about the message. Neither looks like a lossy network.
	CHECK(total("net.handshake.established") == 1.0);
	CHECK(total("net.handshake.refused") == 1.0);
}
