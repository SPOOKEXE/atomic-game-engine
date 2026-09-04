#pragma once

// arch-waiver public-header: forward net API. QUIC adapters share this complete
// cryptographic wire contract at their boundary.

// QUIC packet protection, and why it is beside `net::Cipher` rather than inside
// it.
//
// RFC 9001 is the specification. What it asks for is a *primitive* surface -
// seal these bytes under this key with this nonce, mask these five bytes with
// this sample - because QUIC computes the nonce itself from a packet number it
// chose. `net::Cipher` refuses to offer that surface and is right to:
// `Cipher.hpp`'s three properties are that the counter is private, that a
// `Sealer` is move-only, and that there is no constructor from raw key
// material, and every one of them would have to be given up to serve a QUIC
// crypto callback. So this is a second, small, private-by-convention surface
// beside it, and `Cipher` keeps its guarantees intact.
//
// **The nonce is an argument here, and that is the one thing to understand
// before touching this file.** It is safe for exactly one reason, and the
// reason is not "we are careful": a QUIC nonce is the packet-number space
// exclusive-ORed into the derived IV, packet numbers within one key phase never
// repeat by construction of the transport, and the key phase changes before the
// space could be exhausted. The uniqueness is the transport's invariant rather
// than this file's, which is why a caller that is not the transport has no
// business calling `Seal`. `net::Cipher` is what other code uses.
//
// **Three suites, not one, and the one that looks unnecessary is mandatory.**
// This engine's cipher is ChaCha20-Poly1305 throughout, and RFC 9001 §5.2 still
// requires AES-128-GCM for Initial packets and §5.8 for the Retry integrity tag
// whatever is negotiated afterwards - so the Initial keys are AES whether or
// not a single 1-RTT packet ever is. AES-256-GCM is here because a TLS 1.3 peer
// may select `TLS_AES_256_GCM_SHA384`; this implementation never offers it, and
// the entry exists so that a suite arriving from the wire has somewhere to go
// other than an unhandled switch.
//
// **Header protection is a raw keystream, which is the second mismatch.** Five
// bytes of ChaCha20 or one AES-ECB block over the sample, exclusive-ORed into
// the first byte and the packet-number field. Nothing else in this repository
// exposes a raw keystream and nothing else should; it is here because the
// protocol is defined in terms of one.
//
// Every function is a pure function of its arguments. Nothing here holds state,
// reads a clock, or allocates on behalf of a caller: outputs are written into
// spans the caller sized.
//
// @tier L11 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::net::quic {

	// Which AEAD protects a packet's payload.
	//
	// @since v0.19
	enum class Aead : uint8_t {
		// RFC 9001 §5.2's Initial suite and TLS 1.3's mandatory one. Present
		// whatever is negotiated, because Initial packets and the Retry tag are
		// always this.
		Aes128Gcm,

		// `TLS_AES_256_GCM_SHA384`'s. Never offered by this implementation and
		// accepted so that a suite byte from the wire has a home.
		Aes256Gcm,

		// `TLS_CHACHA20_POLY1305_SHA256`'s, and the engine's own suite - one
		// pass, no tables, constant time without AES instructions. See
		// `Cipher.hpp` for why that matters on a phone.
		ChaCha20Poly1305,
	};

	// Which cipher produces the header-protection mask.
	//
	// Paired with an `Aead` rather than derived from it, because RFC 9001 §5.4
	// names them separately and a reader should be able to see which of the two
	// a bug is in.
	//
	// @since v0.19
	enum class HeaderCipher : uint8_t {
		// AES-128 in ECB over the sample. One block, no chaining, no IV: the
		// mode is a keystream generator here rather than an encryption mode.
		Aes128,

		// AES-256 in ECB, for the suite this implementation does not offer.
		Aes256,

		// ChaCha20 with the counter and nonce taken out of the sample, over
		// five zero bytes.
		ChaCha20,
	};

	// The authentication tag every AEAD here produces. RFC 9001 fixes it at the
	// full 16 bytes; a truncated tag is a forgeable one.
	inline constexpr size_t TAG_BYTES = 16;

	// The nonce every AEAD here takes. 96 bits, and RFC 9001 §5.3 requires the
	// derived IV to be at least this long for exactly that reason.
	inline constexpr size_t NONCE_BYTES = 12;

	// The header-protection mask, RFC 9001 §5.4.1: one byte of flags plus the
	// four bytes the packet-number field can occupy.
	inline constexpr size_t MASK_BYTES = 5;

	// The sample a header cipher is given, RFC 9001 §5.4.2. Sixteen bytes taken
	// from the ciphertext at a fixed offset past the packet-number field.
	inline constexpr size_t SAMPLE_BYTES = 16;

	// The longest key any suite here uses, so a caller can size an array once.
	inline constexpr size_t MAXIMUM_KEY_BYTES = 32;

	// SHA-256's output, which is the width of every secret in this file.
	//
	// **TLS 1.3 allows SHA-384 and this does not**, which is the same decision
	// as not offering `TLS_AES_256_GCM_SHA384`: one hash means one key schedule
	// and one transcript width, and a second would be a second set of lengths
	// that only one negotiation exercises.
	inline constexpr size_t SECRET_BYTES = 32;

	// How long a key for this AEAD is.
	//
	// @param aead The suite.
	// @return The key length in bytes.
	// @since v0.19
	size_t KeyBytes(Aead aead);

	// How long a key for this header cipher is.
	//
	// @param cipher The cipher.
	// @return The key length in bytes.
	// @since v0.19
	size_t KeyBytes(HeaderCipher cipher);

	// Returns a stable, human-readable name for an AEAD.
	//
	// @param aead The suite.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(Aead aead);

	// Returns a stable, human-readable name for a header cipher.
	//
	// @param cipher The cipher.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(HeaderCipher cipher);

	// HKDF-Extract with SHA-256, RFC 5869 §2.2.
	//
	// Split out from expansion rather than folded into one `DeriveKey` call
	// because TLS 1.3 and QUIC both use the two halves independently - the
	// initial secret is an extract whose output is expanded four times, and the
	// TLS key schedule extracts three times in sequence.
	//
	// @param salt     The salt. May be empty, which RFC 5869 treats as a string
	//        of zeros the length of the hash.
	// @param material The input keying material.
	// @return The pseudorandom key.
	// @since v0.19
	std::array<std::byte, SECRET_BYTES>
	Extract(std::span<const std::byte> salt, std::span<const std::byte> material);

	// HKDF-Expand-Label with SHA-256, RFC 8446 §7.1.
	//
	// The label is prefixed with `tls13 ` and length-framed together with an
	// empty context, which is what makes two derivations under one secret
	// differ. Callers pass the bare label - `quic key`, `client in` - and this
	// adds the prefix, because a caller that spelled the prefix itself is a
	// caller that can spell it wrong.
	//
	// @param secret The pseudorandom key to expand.
	// @param label  The label without the `tls13 ` prefix.
	// @param out    Where the output goes. Its length is the expansion length,
	//        which must be at most 255 times the hash size.
	// @return `false` when `out` is longer than one expansion can produce, in
	//         which case nothing is written.
	// @since v0.19
	bool ExpandLabel(std::span<const std::byte> secret, std::string_view label, std::span<std::byte> out);

	// HKDF-Expand-Label over a caller-supplied context, RFC 8446 §7.1.
	//
	// The form the TLS key schedule needs: `derive_secret` hashes the transcript
	// and passes it here, and the traffic secrets are labelled over that hash
	// rather than over an empty context.
	//
	// @param secret  The pseudorandom key to expand.
	// @param label   The label without the `tls13 ` prefix.
	// @param context The context, usually a transcript hash. May be empty.
	// @param out     Where the output goes.
	// @return `false` when `out` is longer than one expansion can produce.
	// @since v0.19
	bool ExpandLabel(
		std::span<const std::byte> secret,
		std::string_view label,
		std::span<const std::byte> context,
		std::span<std::byte> out
	);

	// SHA-256 of some bytes, for transcript hashes and the key schedule's empty
	// hash.
	//
	// @param data The bytes.
	// @return The digest.
	// @since v0.19
	std::array<std::byte, SECRET_BYTES> Digest(std::span<const std::byte> data);

	// HMAC-SHA256, which TLS 1.3's Finished message is computed with.
	//
	// @param key  The key.
	// @param data The bytes to authenticate.
	// @return The tag.
	// @since v0.19
	std::array<std::byte, SECRET_BYTES> Hmac(std::span<const std::byte> key, std::span<const std::byte> data);

	// A comparison whose duration does not depend on where two buffers differ.
	//
	// Every tag and every Finished check goes through this. A comparison that
	// returned on the first differing byte hands the value over one byte at a
	// time to anybody willing to send a few million guesses - the property
	// `engine.net.bench.crypto` measures for `Cipher` and the reason a *faster*
	// refusal is a bug rather than good news.
	//
	// @param left  One buffer.
	// @param right The other.
	// @return `true` when they are the same length and the same bytes.
	// @since v0.19
	bool SameBytes(std::span<const std::byte> left, std::span<const std::byte> right);

	// One direction's packet protection at one encryption level.
	//
	// The three things RFC 9001 §5.1 derives from a traffic secret, kept
	// together because they are installed together and a mismatched trio is a
	// handshake that does not complete with nothing saying why.
	//
	// @since v0.19
	struct PacketKeys {
		// The AEAD key. Only the first `KeyLength` bytes are meaningful.
		std::array<std::byte, MAXIMUM_KEY_BYTES> Key{};

		// How much of `Key` this suite uses.
		size_t KeyLength = 0;

		// The initialisation vector the packet number is exclusive-ORed into.
		std::array<std::byte, NONCE_BYTES> Iv{};

		// The header-protection key. Only the first `HeaderKeyLength` bytes are
		// meaningful.
		std::array<std::byte, MAXIMUM_KEY_BYTES> HeaderKey{};

		// How much of `HeaderKey` this cipher uses.
		size_t HeaderKeyLength = 0;

		// The AEAD key as the span its suite actually uses.
		//
		// @return A view of the first `KeyLength` bytes.
		std::span<const std::byte> KeySpan() const {
			return {Key.data(), KeyLength};
		}

		// The header-protection key as the span its cipher actually uses.
		//
		// @return A view of the first `HeaderKeyLength` bytes.
		std::span<const std::byte> HeaderKeySpan() const {
			return {HeaderKey.data(), HeaderKeyLength};
		}
	};

	// Derives one direction's packet protection from a traffic secret.
	//
	// RFC 9001 §5.1: `quic key`, `quic iv` and `quic hp` expanded from the
	// secret, each to the length its algorithm wants.
	//
	// @param secret The traffic secret.
	// @param aead   Which AEAD the key is for.
	// @param header Which cipher the header key is for.
	// @return The three derived values.
	// @since v0.19
	PacketKeys DeriveKeys(std::span<const std::byte> secret, Aead aead, HeaderCipher header);

	// The next traffic secret in a key phase, RFC 9001 §6.
	//
	// `quic ku` over the current secret. A key update replaces the AEAD key and
	// the IV and **leaves the header-protection key alone**, which is why this
	// returns a secret rather than a `PacketKeys` - the caller derives the two
	// that change and keeps the third.
	//
	// @param secret The current traffic secret.
	// @return The next one.
	// @since v0.19
	std::array<std::byte, SECRET_BYTES> NextSecret(std::span<const std::byte> secret);

	// Both directions' Initial keys, derived from a connection id.
	//
	// **This is the one derivation with no secret in it.** RFC 9001 §5.2
	// extracts under a published salt from the client's original Destination
	// Connection ID, which is a value on the wire in the clear - so Initial
	// packets are authenticated against tampering and not against reading, and
	// anybody who saw the first datagram can decrypt every Initial packet of the
	// connection. That is the protocol working as designed: what Initial packets
	// carry is the TLS handshake, which protects itself.
	//
	// @since v0.19
	struct InitialKeys {
		// What the client seals with and the server opens with.
		PacketKeys Client;

		// What the server seals with and the client opens with.
		PacketKeys Server;
	};

	// Derives both directions' Initial keys from the original destination
	// connection id.
	//
	// @param destination The Destination Connection ID from the client's first
	//        Initial packet, before any Retry.
	// @return Both directions' keys, AES-128-GCM throughout.
	// @since v0.19
	InitialKeys DeriveInitialKeys(std::span<const std::byte> destination);

	// The nonce for one packet, RFC 9001 §5.3.
	//
	// The packet number, big-endian and left-padded to the IV's length,
	// exclusive-ORed into the IV. **Not a counter this file holds** - see the
	// note at the top of the header on why that is safe here and is refused in
	// `Cipher`.
	//
	// @param iv           The derived IV.
	// @param packetNumber The full packet number, not the truncated one on the
	//        wire.
	// @return The nonce.
	// @since v0.19
	std::array<std::byte, NONCE_BYTES> NonceFor(std::span<const std::byte> iv, uint64_t packetNumber);

	// Seals a packet payload.
	//
	// @param aead           The suite.
	// @param key            The AEAD key, of this suite's length.
	// @param nonce          The nonce, from `NonceFor`.
	// @param associatedData The unprotected header, which the tag covers.
	// @param plaintext      The bytes to seal. May be empty.
	// @param out            Where the ciphertext and tag go. Must be exactly
	//        `plaintext.size() + TAG_BYTES` long. May alias `plaintext` when the
	//        two start at the same address, which is how QUIC frames a packet in
	//        place.
	// @return `false` when a length is wrong, in which case `out` is untouched.
	// @since v0.19
	bool Seal(
		Aead aead,
		std::span<const std::byte> key,
		std::span<const std::byte> nonce,
		std::span<const std::byte> associatedData,
		std::span<const std::byte> plaintext,
		std::span<std::byte> out
	);

	// Opens a packet payload.
	//
	// **A refusal is constant time in the tag**, for the reason `SameBytes`
	// exists. A refusal on a length is not, and does not need to be: the length
	// is the attacker's own.
	//
	// @param aead           The suite.
	// @param key            The AEAD key, of this suite's length.
	// @param nonce          The nonce, from `NonceFor`.
	// @param associatedData The unprotected header the tag covers.
	// @param ciphertext     The sealed bytes including the tag.
	// @param out            Where the plaintext goes. Must be exactly
	//        `ciphertext.size() - TAG_BYTES` long. May alias `ciphertext`.
	// @return `false` when the tag does not verify or a length is wrong, in
	//         which case nothing in `out` may be believed.
	// @since v0.19
	bool Open(
		Aead aead,
		std::span<const std::byte> key,
		std::span<const std::byte> nonce,
		std::span<const std::byte> associatedData,
		std::span<const std::byte> ciphertext,
		std::span<std::byte> out
	);

	// The header-protection mask for one packet, RFC 9001 §5.4.
	//
	// @param cipher The cipher.
	// @param key    The header-protection key, of this cipher's length.
	// @param sample The `SAMPLE_BYTES` taken from the packet's ciphertext.
	// @param out    Where the mask goes. Must be `MASK_BYTES` long.
	// @return `false` when a length is wrong.
	// @since v0.19
	bool Mask(
		HeaderCipher cipher,
		std::span<const std::byte> key,
		std::span<const std::byte> sample,
		std::span<std::byte> out
	);

	// The integrity tag on a Retry packet, RFC 9001 §5.8.
	//
	// A fixed key and a fixed nonce, published in the RFC, over a pseudo-packet
	// that begins with the original Destination Connection ID. It authenticates
	// nothing secret and is not meant to: what it proves is that the Retry came
	// from something on the path that saw the client's first packet, which is
	// the whole of what an off-path attacker must not be able to forge.
	//
	// @param original The client's original Destination Connection ID.
	// @param retry    The Retry packet without its trailing tag.
	// @param out      Where the tag goes. Must be `TAG_BYTES` long.
	// @return `false` when `out` is the wrong length.
	// @since v0.19
	bool
	RetryTag(std::span<const std::byte> original, std::span<const std::byte> retry, std::span<std::byte> out);
}
