#include <engine/net/quic/Crypto.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.quic.crypto")

using namespace engine::net::quic;

namespace {
	// The published vectors are hex and are transcribed here as hex, so a row can
	// be compared against RFC 9001 by eye rather than by trusting a conversion
	// somebody did once. The same shape `tests/Cipher.cpp` uses.
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

	bool Same(std::span<const std::byte> left, std::string_view hex) {
		const std::vector<std::byte> right = Hex(hex);
		return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
	}
}

// --- the key schedule -------------------------------------------------------

TEST_CASE("the Initial keys match RFC 9001 A.1", "[net][quic][crypto]") {
	// The connection id the whole of Appendix A is worked from.
	const std::vector<std::byte> destination = Hex("8394c8f03e515708");
	const InitialKeys keys = DeriveInitialKeys(destination);

	// A.1's client_initial_secret expansion.
	CHECK(Same(keys.Client.KeySpan(), "1f369613dd76d5467730efcbe3b1a22d"));
	CHECK(Same(keys.Client.Iv, "fa044b2f42a3fd3b46fb255c"));
	CHECK(Same(keys.Client.HeaderKeySpan(), "9f50449e04a0e810283a1e9933adedd2"));

	// A.1's server_initial_secret expansion.
	CHECK(Same(keys.Server.KeySpan(), "cf3a5331653c364c88f0f379b6067e37"));
	CHECK(Same(keys.Server.Iv, "0ac1493ca1905853b0bba03e"));
	CHECK(Same(keys.Server.HeaderKeySpan(), "c206b8d9b9f0f37644430b490eeaa314"));

	// Both directions are AES-128-GCM whatever is negotiated later, which is
	// the reason `Aead::Aes128Gcm` exists in an engine whose suite is ChaCha20.
	CHECK(keys.Client.KeyLength == 16);
	CHECK(keys.Server.KeyLength == 16);
}

TEST_CASE("the Initial secret is a published salt over a public connection id", "[net][quic][crypto]") {
	// RFC 9001 A.1's intermediate value. Checked on its own because the four
	// expansions above would all be wrong together if this were, and a single
	// row saying which half failed is worth having.
	const std::vector<std::byte> salt = Hex("38762cf7f55934b34d179ae6a4c80cadccbb7f0a");
	const std::vector<std::byte> destination = Hex("8394c8f03e515708");
	const auto initial = Extract(salt, destination);
	CHECK(Same(initial, "7db5df06e7a69e432496adedb00851923595221596ae2ae9fb8115c1e9ed0a44"));
}

TEST_CASE("a traffic secret expands to the ChaCha20 keys of RFC 9001 A.5", "[net][quic][crypto]") {
	const std::vector<std::byte> secret =
		Hex("9ac312a7f877468ebe69422748ad00a15443f18203a07d6060f688f30f21632b");
	const PacketKeys keys = DeriveKeys(secret, Aead::ChaCha20Poly1305, HeaderCipher::ChaCha20);

	CHECK(Same(keys.KeySpan(), "c6d98ff3441c3fe1b2182094f69caa2ed4b716b65488960a7a984979fb23e1c8"));
	CHECK(Same(keys.Iv, "e0459b3474bdd0e44a41c144"));
	CHECK(Same(keys.HeaderKeySpan(), "25a282b9e82f06f21f488917a4fc8f1b73573685608597d0efcb076b0ab7a7a4"));
}

TEST_CASE("a key update derives the next secret", "[net][quic][crypto]") {
	const std::vector<std::byte> secret =
		Hex("9ac312a7f877468ebe69422748ad00a15443f18203a07d6060f688f30f21632b");
	CHECK(Same(NextSecret(secret), "1223504755036d556342ee9361d253421a826c9ecdf3c7148684b36b714881f9"));
}

TEST_CASE("an expansion longer than one HKDF can produce is refused", "[net][quic][crypto]") {
	const std::vector<std::byte> secret = Hex("00112233445566778899aabbccddeeff");
	std::vector<std::byte> out(255 * 32 + 1);
	// Refused rather than truncated. Nothing here asks for more than 32 bytes;
	// the check exists so that a caller that eventually does gets an answer
	// instead of a short key.
	CHECK_FALSE(ExpandLabel(secret, "quic key", out));
}

// --- the nonce --------------------------------------------------------------

TEST_CASE("the nonce is the packet number exclusive-ORed into the IV", "[net][quic][crypto]") {
	const std::vector<std::byte> iv = Hex("e0459b3474bdd0e44a41c144");

	// RFC 9001 A.5's packet number, 654360564.
	CHECK(Same(NonceFor(iv, 654360564), "e0459b3474bdd0e46d417eb0"));

	// Packet number zero leaves the IV alone, which is the property that makes
	// the first packet's nonce readable in a capture beside the derived IV.
	CHECK(Same(NonceFor(iv, 0), "e0459b3474bdd0e44a41c144"));
}

TEST_CASE("two packet numbers never share a nonce", "[net][quic][crypto]") {
	// The whole safety argument for taking a nonce as an argument at all. It is
	// the transport's invariant rather than this file's, so what is pinned here
	// is the half this file owns: distinct numbers produce distinct nonces.
	const std::vector<std::byte> iv = Hex("e0459b3474bdd0e44a41c144");
	CHECK(NonceFor(iv, 1) != NonceFor(iv, 2));
	CHECK(NonceFor(iv, 0) != NonceFor(iv, 0xffffffffffffffffULL));
}

// --- packet protection ------------------------------------------------------

TEST_CASE("a ChaCha20-Poly1305 packet matches RFC 9001 A.5", "[net][quic][crypto]") {
	const std::vector<std::byte> key =
		Hex("c6d98ff3441c3fe1b2182094f69caa2ed4b716b65488960a7a984979fb23e1c8");
	const std::vector<std::byte> nonce = Hex("e0459b3474bdd0e46d417eb0");

	// The unprotected short header, which is the associated data. A rewritten
	// header fails the tag rather than being acted on - the property `net`'s own
	// framing already has and the reason header protection is separate from it.
	const std::vector<std::byte> header = Hex("4200bff4");

	// One PING frame.
	const std::vector<std::byte> plaintext = Hex("01");

	std::vector<std::byte> sealed(plaintext.size() + TAG_BYTES);
	REQUIRE(Seal(Aead::ChaCha20Poly1305, key, nonce, header, plaintext, sealed));
	CHECK(Same(sealed, "655e5cd55c41f69080575d7999c25a5bfb"));

	std::vector<std::byte> opened(plaintext.size());
	REQUIRE(Open(Aead::ChaCha20Poly1305, key, nonce, header, sealed, opened));
	CHECK(Same(opened, "01"));
}

TEST_CASE("a rewritten header fails the tag", "[net][quic][crypto]") {
	const std::vector<std::byte> key =
		Hex("c6d98ff3441c3fe1b2182094f69caa2ed4b716b65488960a7a984979fb23e1c8");
	const std::vector<std::byte> nonce = Hex("e0459b3474bdd0e46d417eb0");
	const std::vector<std::byte> sealed = Hex("655e5cd55c41f69080575d7999c25a5bfb");

	// One bit of the packet number, which is exactly the field an attacker on
	// the path would want to move.
	const std::vector<std::byte> rewritten = Hex("4200bff5");
	std::vector<std::byte> opened(1);
	CHECK_FALSE(Open(Aead::ChaCha20Poly1305, key, nonce, rewritten, sealed, opened));
}

TEST_CASE("a forged tag is refused", "[net][quic][crypto]") {
	const std::vector<std::byte> key = Hex("1f369613dd76d5467730efcbe3b1a22d");
	const std::vector<std::byte> nonce = Hex("fa044b2f42a3fd3b46fb255c");
	const std::vector<std::byte> plaintext = Hex("0001020304050607");

	std::vector<std::byte> sealed(plaintext.size() + TAG_BYTES);
	REQUIRE(Seal(Aead::Aes128Gcm, key, nonce, {}, plaintext, sealed));

	std::vector<std::byte> opened(plaintext.size());
	REQUIRE(Open(Aead::Aes128Gcm, key, nonce, {}, sealed, opened));
	CHECK(Same(opened, "0001020304050607"));

	sealed.back() ^= std::byte{0x01};
	CHECK_FALSE(Open(Aead::Aes128Gcm, key, nonce, {}, sealed, opened));
}

TEST_CASE("a frame shorter than the tag is refused without a comparison", "[net][quic][crypto]") {
	const std::vector<std::byte> key = Hex("1f369613dd76d5467730efcbe3b1a22d");
	const std::vector<std::byte> nonce = Hex("fa044b2f42a3fd3b46fb255c");
	const std::vector<std::byte> stub = Hex("0011");
	std::vector<std::byte> opened;
	CHECK_FALSE(Open(Aead::Aes128Gcm, key, nonce, {}, stub, opened));
}

TEST_CASE("a key of the wrong length is refused rather than padded", "[net][quic][crypto]") {
	// AES-128-GCM's key is 16 bytes and Crypto++ would accept 32 as AES-256.
	// Silently changing algorithm on a caller that got a length wrong is how one
	// end of a connection ends up sealing under a cipher the other is not
	// opening with.
	const std::vector<std::byte> key =
		Hex("c6d98ff3441c3fe1b2182094f69caa2ed4b716b65488960a7a984979fb23e1c8");
	const std::vector<std::byte> nonce = Hex("fa044b2f42a3fd3b46fb255c");
	std::vector<std::byte> sealed(TAG_BYTES);
	CHECK_FALSE(Seal(Aead::Aes128Gcm, key, nonce, {}, {}, sealed));
}

// --- header protection ------------------------------------------------------

TEST_CASE("the AES header mask matches RFC 9001 A.2", "[net][quic][crypto]") {
	const std::vector<std::byte> key = Hex("9f50449e04a0e810283a1e9933adedd2");
	const std::vector<std::byte> sample = Hex("d1b1c98dd7689fb8ec11d242b123dc9b");

	std::array<std::byte, MASK_BYTES> mask{};
	REQUIRE(Mask(HeaderCipher::Aes128, key, sample, mask));
	CHECK(Same(mask, "437b9aec36"));
}

TEST_CASE("the ChaCha20 header mask matches RFC 9001 A.5", "[net][quic][crypto]") {
	const std::vector<std::byte> key =
		Hex("25a282b9e82f06f21f488917a4fc8f1b73573685608597d0efcb076b0ab7a7a4");
	const std::vector<std::byte> sample = Hex("5e5cd55c41f69080575d7999c25a5bfb");

	// The counter is the first four bytes of the sample read little-endian and
	// the nonce is the remaining twelve, which is the one place in this file
	// where a value from the wire chooses where a keystream starts.
	std::array<std::byte, MASK_BYTES> mask{};
	REQUIRE(Mask(HeaderCipher::ChaCha20, key, sample, mask));
	CHECK(Same(mask, "aefefe7d03"));
}

TEST_CASE("a short sample is refused", "[net][quic][crypto]") {
	const std::vector<std::byte> key = Hex("9f50449e04a0e810283a1e9933adedd2");
	const std::vector<std::byte> sample = Hex("d1b1c98dd7689fb8");
	std::array<std::byte, MASK_BYTES> mask{};
	CHECK_FALSE(Mask(HeaderCipher::Aes128, key, sample, mask));
}

// --- Retry ------------------------------------------------------------------

TEST_CASE("the Retry integrity tag matches RFC 9001 A.4", "[net][quic][crypto]") {
	const std::vector<std::byte> original = Hex("8394c8f03e515708");
	const std::vector<std::byte> retry = Hex("ff000000010008f067a5502a4262b5746f6b656e");

	std::array<std::byte, TAG_BYTES> tag{};
	REQUIRE(RetryTag(original, retry, tag));
	CHECK(Same(tag, "04a265ba2eff4d829058fb3f0f2496ba"));
}

TEST_CASE("a Retry tag over a different original connection id differs", "[net][quic][crypto]") {
	// What the tag proves is that whoever sent the Retry saw the client's first
	// packet. A tag that did not depend on the original connection id would
	// prove nothing and an off-path attacker could forge one.
	const std::vector<std::byte> retry = Hex("ff000000010008f067a5502a4262b5746f6b656e");

	std::array<std::byte, TAG_BYTES> mine{};
	std::array<std::byte, TAG_BYTES> theirs{};
	REQUIRE(RetryTag(Hex("8394c8f03e515708"), retry, mine));
	REQUIRE(RetryTag(Hex("8394c8f03e515709"), retry, theirs));
	CHECK(mine != theirs);
}

// --- the shared helpers -----------------------------------------------------

TEST_CASE("a byte comparison refuses different lengths", "[net][quic][crypto]") {
	const std::vector<std::byte> left = Hex("00112233");
	const std::vector<std::byte> right = Hex("001122");
	CHECK_FALSE(SameBytes(left, right));
	CHECK(SameBytes(left, left));
}

TEST_CASE("every suite names itself and states its key length", "[net][quic][crypto]") {
	CHECK(KeyBytes(Aead::Aes128Gcm) == 16);
	CHECK(KeyBytes(Aead::Aes256Gcm) == 32);
	CHECK(KeyBytes(Aead::ChaCha20Poly1305) == 32);
	CHECK(KeyBytes(HeaderCipher::Aes128) == 16);
	CHECK(KeyBytes(HeaderCipher::Aes256) == 32);
	CHECK(KeyBytes(HeaderCipher::ChaCha20) == 32);

	CHECK(std::string_view(Describe(Aead::ChaCha20Poly1305)) == "AEAD_CHACHA20_POLY1305");
	CHECK(std::string_view(Describe(HeaderCipher::ChaCha20)) == "ChaCha20");
}
