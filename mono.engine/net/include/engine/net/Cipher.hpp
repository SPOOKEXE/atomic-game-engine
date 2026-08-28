#pragma once

// Frame encryption, and the one rule that makes it safe to use.
//
// ChaCha20-Poly1305, RFC 8439, which is what this transport seals with. One
// pass over the bytes, no lookup tables, and constant time on
// hardware with no AES instructions - a phone, which is exactly where AES-GCM in
// software is both slow and a timing risk.
//
// **That constant time is now measured rather than asserted.**
// `engine.net.bench.crypto` opens the same frame authentic, forged, and under
// rewritten associated data, and reports 1656, 1664 and 1728 nanoseconds - the
// same figure to within the noise of the machine. A refusal that came back
// *faster* than an accept would be the bug, not the good news it looks like: it
// would mean the tag comparison returns on the first differing byte, which
// hands the tag over one byte at a time to anybody willing to send a few
// million guesses. Those rows are the regression test for that, and they are
// the one place in this repository where a smaller number is a failure.
//
// The one refusal that is allowed to be cheap is a frame shorter than the tag -
// 31 ns against 1656 - because there is nothing to compare in constant time
// against, and the length it leaks is the length the attacker chose.
//
// **A nonce must never repeat under one key, and this file is built so that it
// cannot rather than asking you to remember.** A repeat is not a degraded mode:
// two frames sealed under the same key and nonce leak the XOR of their
// plaintexts, and for Poly1305 they also hand over the material to forge tags
// under that key. The three things that make it impossible here:
//
// - **The nonce is not an argument.** A Sealer owns a 64-bit counter that only
//   moves forward, that nothing outside can set, reset or reach, and that
//   refuses to seal at its end rather than wrapping.
// - **A Sealer is move-only**, so there is never a second object counting from
//   the same place under the same key, and a moved-from one seals nothing.
// - **A Sealer cannot be built from raw key material at all.** The only source
//   of one is Handshake::TakeKeys, and every handshake derives its keys from a
//   fresh ephemeral X25519 pair. A counter is only unique under one key because
//   the key itself is never handed to a second Sealer.
//
// An Opener is the other half and its rules are the opposite ones. It never
// chooses a nonce - the counter arrives on the wire from a peer assumed hostile
// - so accepting key material from a caller costs nothing that is not already
// the attacker's to choose. That asymmetry is why Opener::FromKey exists and
// Sealer::FromKey deliberately does not.
//
// **This refuses a forgery, not a replay.** A frame captured and sent again is
// authentic by construction and opens cleanly. Discarding it is the sequence
// window's job one layer out - `Packet::IsNewer` and the channel's counter -
// because that layer is the one that knows whether a repeat is an attack or an
// ordinary reliable resend.
//
// Nothing here reads a clock and nothing here does I/O.
//
// @tier L11 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::net {

	// ChaCha20-Poly1305, and the two halves of it a session holds.
	//
	// Sizes and nested types rather than something to construct: the state
	// belongs to a Sealer and an Opener, one of each per direction per
	// connection.
	//
	// @since v0.3
	class Cipher {
	  public:
		// Key length. The algorithm's, not a choice.
		static constexpr size_t KEY_BYTES = 32;

		// Nonce length. RFC 8439 fixes it at 96 bits.
		static constexpr size_t NONCE_BYTES = 12;

		// The fixed part of the nonce, which the counter is appended to.
		//
		// RFC 8439 §2.8 builds a nonce this way - a fixed 32-bit part plus a
		// 64-bit counter - and the fixed part comes out of the handshake's key
		// derivation alongside the key, one per direction. It buys nothing on its
		// own, since distinct keys already make the nonce spaces disjoint; it
		// costs four bytes of derived material and means the two directions do
		// not run identical nonce sequences even when something upstream has gone
		// wrong with the keys.
		static constexpr size_t NONCE_PREFIX_BYTES = 4;

		// Authentication tag length. Poly1305 produces 16 bytes and truncating it
		// is how a tag becomes forgeable, so it is a constant rather than a
		// setting.
		static constexpr size_t TAG_BYTES = 16;

		// What sealing costs on the wire, over the plaintext, per frame.
		static constexpr size_t OVERHEAD_BYTES = TAG_BYTES;

		// A sealed frame, and the one number the peer needs to open it.
		struct Sealed {
			// The nonce counter this frame was sealed under.
			//
			// **It has to reach the peer**, because the receiver cannot derive
			// it: frames are lost and reordered on the unreliable channel, so a
			// counter the receiver maintained itself would drift out of step with
			// the sender's on the first drop. `PacketHeader::Counter` is where it
			// goes, whole rather than truncated against anything that wraps.
			//
			// It is not a secret and it does not need authenticating on its own:
			// rewriting it changes the nonce the receiver derives, so the tag
			// fails. It is covered anyway, because the framing above passes the
			// whole header as associated data.
			uint64_t Counter = 0;

			// Ciphertext followed by the 16-byte tag.
			//
			// A view into the Sealer that produced it, not a copy - the frame is
			// written into an outgoing buffer within the same call chain, and a
			// per-packet allocation here would be one per connection per tick.
			//
			// @warning Valid until the next Seal on that Sealer, or until it
			//          dies. Copy the bytes if they have to outlive either.
			std::span<const std::byte> Bytes;
		};

		// The sending half of one direction. Seals, and cannot do anything else.
		//
		// One per direction per connection, obtained from Handshake::TakeKeys.
		// There is no constructor taking a key, and that absence is load-bearing
		// - see the file comment.
		class Sealer {
		  public:
			// Zeroes the key.
			~Sealer();

			Sealer(const Sealer &) = delete;
			Sealer &operator=(const Sealer &) = delete;

			// Moves the key and the counter, leaving the source zeroed and
			// permanently unable to seal.
			//
			// Not merely emptied. Two Sealers that both believe they hold this
			// key and this counter is the one way an outside caller could produce
			// a repeated nonce, so the source stops being one.
			Sealer(Sealer &&other) noexcept;

			// Moves the key and the counter, zeroing both this Sealer's old key
			// and the source's.
			Sealer &operator=(Sealer &&other) noexcept;

			// The counter the next Seal will use.
			//
			// **Reading a counter cannot make one repeat**, which is why this
			// exists while there is still no way to set, reset or reach one. A
			// frame's associated data is the packet header it travels under and
			// that header carries the counter, so the header has to be written
			// before the frame is sealed - and this is the only way to write it
			// under the counter the seal will actually use. A caller that reads
			// this and then does not seal has skipped one value out of 2^64 and
			// repeated nothing.
			//
			// @return The next counter, or UINT64_MAX for a Sealer that has been
			//         moved from or has exhausted its counter - both of which
			//         make the next Seal refuse.
			uint64_t NextCounter() const {
				return Counter;
			}

			// Seals a frame under the next counter.
			//
			// @param plaintext The bytes to encrypt. May be empty - a frame
			//        carrying only authenticated associated data is 16 bytes.
			// @param associatedData Authenticated but not encrypted, and the
			//        place to put the packet header so that a rewritten channel
			//        or sequence fails the tag. Pass an empty span to cover
			//        nothing, which means nothing outside the ciphertext is
			//        protected.
			// @return The frame and its counter, or nothing if this Sealer has
			//         been moved from or has exhausted its counter.
			std::optional<Sealed>
			Seal(std::span<const std::byte> plaintext, std::span<const std::byte> associatedData);

		  private:
			friend class Handshake;

			// Built by Handshake and by nothing else. The parameters are assumed
			// to be the right lengths, which is the caller's job because the
			// caller is the key derivation.
			Sealer(std::span<const std::byte> key, std::span<const std::byte> noncePrefix);

			// The counter value that means "this Sealer is finished".
			//
			// Reserving the last one costs a single counter out of 2^64 and turns
			// exhaustion into a refusal instead of a wrap. At sixty frames a
			// second the real end is further away than the age of the universe;
			// this exists so that "never repeats" is a property of the type
			// rather than an assumption about how long a session runs, and it is
			// what a moved-from Sealer is set to.
			static constexpr uint64_t EXHAUSTED = UINT64_MAX;

			std::array<uint8_t, KEY_BYTES> Key{};
			std::array<uint8_t, NONCE_PREFIX_BYTES> NoncePrefix{};
			uint64_t Counter = 0;

			// Reused across frames so that sealing does not allocate per packet.
			// Holds ciphertext, which is not secret.
			std::vector<std::byte> Frame;
		};

		// The receiving half of one direction. Opens, and cannot seal.
		//
		// Every byte handed to it is hostile: a frame that fails authentication
		// is refused whole, and the caller never sees a plaintext that was not
		// verified first.
		class Opener {
		  public:
			// Builds an Opener from key material.
			//
			// The direction where this is safe. An Opener chooses no nonce, so a
			// caller holding its key cannot cause a nonce to repeat under it -
			// the counter is the sender's, and on the wire it is the attacker's
			// anyway. That is why the sealing half has no equivalent.
			//
			// @param key Exactly KEY_BYTES.
			// @param noncePrefix Exactly NONCE_PREFIX_BYTES.
			// @return The Opener, or nothing if either length is wrong.
			static std::optional<Opener>
			FromKey(std::span<const std::byte> key, std::span<const std::byte> noncePrefix);

			// Zeroes the key.
			~Opener();

			Opener(const Opener &) = delete;
			Opener &operator=(const Opener &) = delete;

			// Moves the key, leaving the source zeroed.
			Opener(Opener &&other) noexcept;

			// Moves the key, zeroing both this Opener's old key and the source's.
			Opener &operator=(Opener &&other) noexcept;

			// Opens a frame, refusing anything that is not one.
			//
			// Refuses a frame shorter than the tag, a tag that does not verify,
			// associated data that differs by a byte from the sender's, and a
			// counter other than the one the frame was sealed under. All four are
			// the same answer, deliberately: which check failed is information
			// about the key, and it is not owed to whoever sent the bytes.
			//
			// **A refusal writes nothing anywhere the caller can see.** The
			// plaintext is produced into this Opener's own buffer and only
			// becomes visible once the tag has verified, because a decrypted-but
			// -unauthenticated buffer is attacker-chosen plaintext and handing
			// one out is how "we check the tag afterwards" becomes a bug.
			//
			// @param counter The counter the sender reported for this frame.
			// @param sealed Ciphertext followed by the 16-byte tag.
			// @param associatedData Exactly what the sender covered, byte for
			//        byte.
			// @return The plaintext as a view into this Opener, valid until the
			//         next Open, or nothing at all.
			std::optional<std::span<const std::byte>> Open(
				uint64_t counter, std::span<const std::byte> sealed, std::span<const std::byte> associatedData
			);

		  private:
			friend class Handshake;

			// Built by Handshake, or by FromKey once it has checked the lengths.
			Opener(std::span<const std::byte> key, std::span<const std::byte> noncePrefix);

			std::array<uint8_t, KEY_BYTES> Key{};
			std::array<uint8_t, NONCE_PREFIX_BYTES> NoncePrefix{};

			// Where a frame is opened before it is known to be genuine. Reused,
			// so opening does not allocate per packet.
			std::vector<std::byte> Plain;
		};
	};
}
