// What encryption costs per packet, and what a connection costs to establish.
//
// **These two numbers have completely different consequences and are measured
// together for exactly that reason.** Sealing is paid per packet, so a
// nanosecond here is six thousand nanoseconds a second on a hundred-player
// server. A handshake is paid per connection, so a *millisecond* there is
// invisible until a hundred players reconnect at once after a server restart —
// at which point it is the thundering-herd cost that decides whether the
// server comes back or falls over.
//
// The handshake rows are the ones to watch. X25519 is a scalar multiplication
// and there is no making it cheap; what matters is knowing the figure, because
// it is also the figure an unauthenticated attacker can make the server spend.
// `Cookie.hpp` exists to keep that spend off the table before a peer has proved
// anything, and the honest way to size a cookie policy is against a measured
// handshake cost rather than a guessed one.
//
// **Opening a forged frame is measured next to opening a real one**, and they
// must cost the same. Poly1305 verification is constant-time by construction;
// a refusal that returned early on the first bad byte would be both faster and
// a timing oracle on the tag. So a *faster* refusal row here is a bug report,
// not good news — which is the opposite of how every other refusal row in this
// repository reads, and the reason it is spelled out.

#include <engine/net/Cipher.hpp>
#include <engine/net/Enums.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Packet.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.net.bench.crypto")

using engine::net::Cipher;
using engine::net::Handshake;
using engine::net::HandshakeRole;
using engine::net::Packet;
using engine::testing::Consume;

namespace crypto_bench {

	// Frames per row. One second of a 100-player server at 60 Hz is 6000, so
	// 10 000 keeps the division easy and the sample long enough to time.
	constexpr size_t FRAMES = 10'000;

	// The header a frame is sealed under, as associated data. Its length is what
	// matters to Poly1305, and it is fixed by the wire format.
	const std::vector<std::byte> &AssociatedData() {
		static const std::vector<std::byte> header = [] {
			std::vector<std::byte> built(Packet::HEADER_BYTES);
			for (size_t index = 0; index < built.size(); index++) {
				built[index] = static_cast<std::byte>(index * 13u);
			}
			return built;
		}();
		return header;
	}

	// Plaintexts at three sizes, because ChaCha20 is a stream cipher and its
	// cost is very nearly linear in the bytes — so a suite that measured only
	// full-size frames would say nothing about the many small ones a reliable
	// channel sends.
	const std::vector<std::byte> &Plaintext(size_t bytes) {
		static std::vector<std::pair<size_t, std::vector<std::byte>>> built;
		for (const auto &[size, data] : built) {
			if (size == bytes) {
				return data;
			}
		}
		std::vector<std::byte> made(bytes);
		for (size_t index = 0; index < made.size(); index++) {
			made[index] = static_cast<std::byte>(index * 11u);
		}
		built.emplace_back(bytes, std::move(made));
		return built.back().second;
	}

	// Both ends of one completed agreement.
	//
	// A `Sealer` has no constructor taking a key — that absence is load-bearing,
	// per `Cipher.hpp` — so every sealing row starts from a real handshake. It
	// is driven from fixed secrets rather than OS entropy so that two runs of
	// this suite seal under the same keys and a difference between them is the
	// code.
	struct Ends {
		Handshake::Session Initiator;
		Handshake::Session Responder;
	};

	Ends Agree() {
		std::array<std::byte, Handshake::SECRET_BYTES> initiatorSecret{};
		std::array<std::byte, Handshake::SECRET_BYTES> responderSecret{};
		initiatorSecret.fill(static_cast<std::byte>(0x11));
		responderSecret.fill(static_cast<std::byte>(0x22));

		std::optional<Handshake> initiator =
			Handshake::BeginFromSecret(HandshakeRole::Initiator, initiatorSecret);
		std::optional<Handshake> responder =
			Handshake::BeginFromSecret(HandshakeRole::Responder, responderSecret);

		const std::span<const std::byte> fromInitiator = initiator->Message();
		const std::vector<std::byte> initiatorMessage(fromInitiator.begin(), fromInitiator.end());
		const std::span<const std::byte> fromResponder = responder->Message();
		const std::vector<std::byte> responderMessage(fromResponder.begin(), fromResponder.end());

		initiator->Consume(responderMessage);
		responder->Consume(initiatorMessage);

		std::optional<Handshake::Session> initiatorKeys = initiator->TakeKeys();
		std::optional<Handshake::Session> responderKeys = responder->TakeKeys();
		return Ends{std::move(*initiatorKeys), std::move(*responderKeys)};
	}

	// One agreement, made once and reused by every sealing row.
	//
	// Lazily rather than at static-initialisation time, because a `Sealer` holds
	// a counter that only moves forward and a benchmark that re-agreed per
	// sample would be measuring the handshake inside the sealing rows.
	Ends &Session() {
		static Ends ends = Agree();
		return ends;
	}

	// A sealed frame at each size, so the opening rows do not re-seal.
	//
	// The counter is captured with it: an `Opener` refuses any counter other
	// than the one the frame was sealed under, so the pair travels together the
	// same way it travels in `PacketHeader::Counter` on the wire.
	struct Frame {
		uint64_t Counter = 0;
		std::vector<std::byte> Bytes;
	};

	const Frame &SealedOf(size_t plaintextBytes) {
		static std::vector<std::pair<size_t, Frame>> built;
		for (const auto &[size, frame] : built) {
			if (size == plaintextBytes) {
				return frame;
			}
		}

		const std::optional<Cipher::Sealed> sealed =
			Session().Initiator.Sending.Seal(Plaintext(plaintextBytes), AssociatedData());
		Frame frame;
		frame.Counter = sealed->Counter;
		// Copied, because `Sealed::Bytes` is a view into the Sealer and only
		// valid until its next Seal — and every row below seals again.
		frame.Bytes.assign(sealed->Bytes.begin(), sealed->Bytes.end());
		built.emplace_back(plaintextBytes, std::move(frame));
		return built.back().second;
	}

	// The same frame with one ciphertext byte flipped. Authentic length, wrong
	// tag — which is what a forgery attempt looks like and what an on-path
	// bit-flip looks like too.
	const Frame &ForgedOf(size_t plaintextBytes) {
		static std::vector<std::pair<size_t, Frame>> built;
		for (const auto &[size, frame] : built) {
			if (size == plaintextBytes) {
				return frame;
			}
		}
		Frame forged = SealedOf(plaintextBytes);
		forged.Bytes[0] = static_cast<std::byte>(static_cast<uint8_t>(forged.Bytes[0]) ^ 0x01u);
		built.emplace_back(plaintextBytes, std::move(forged));
		return built.back().second;
	}
}

using namespace crypto_bench;

// --- sealing ------------------------------------------------------------------
//
// Per packet, per direction. Multiply by the packet rate before deciding
// whether a figure is small.

BENCH("Seal · 10k full-size frames", FRAMES) {
	Ends &ends = Session();
	const std::vector<std::byte> &plaintext = Plaintext(Packet::MAXIMUM_MESSAGE_BYTES);
	const std::vector<std::byte> &header = AssociatedData();
	for (size_t index = 0; index < FRAMES; index++) {
		Consume(ends.Initiator.Sending.Seal(plaintext, header)->Counter);
	}
}

BENCH("Seal · 10k 256-byte frames", FRAMES) {
	Ends &ends = Session();
	const std::vector<std::byte> &plaintext = Plaintext(256);
	const std::vector<std::byte> &header = AssociatedData();
	for (size_t index = 0; index < FRAMES; index++) {
		Consume(ends.Initiator.Sending.Seal(plaintext, header)->Counter);
	}
}

BENCH("Seal · 10k empty frames", FRAMES) {
	// A frame carrying only authenticated associated data is 16 bytes, and it
	// is what a keep-alive is. The gap between this and the full-size row is
	// ChaCha20 over the payload; this row on its own is Poly1305 over the header
	// plus the per-call fixed cost, which is the floor no packet gets under.
	Ends &ends = Session();
	const std::vector<std::byte> &header = AssociatedData();
	for (size_t index = 0; index < FRAMES; index++) {
		Consume(ends.Initiator.Sending.Seal({}, header)->Counter);
	}
}

// --- opening ------------------------------------------------------------------

BENCH("Open · 10k full-size frames", FRAMES) {
	Ends &ends = Session();
	const Frame &frame = SealedOf(Packet::MAXIMUM_MESSAGE_BYTES);
	const std::vector<std::byte> &header = AssociatedData();
	size_t bytes = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		const std::optional<std::span<const std::byte>> opened =
			ends.Responder.Receiving.Open(frame.Counter, frame.Bytes, header);
		bytes += opened ? opened->size() : 0;
	}
	Consume(bytes);
}

BENCH("Open · 10k 256-byte frames", FRAMES) {
	Ends &ends = Session();
	const Frame &frame = SealedOf(256);
	const std::vector<std::byte> &header = AssociatedData();
	size_t bytes = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		const std::optional<std::span<const std::byte>> opened =
			ends.Responder.Receiving.Open(frame.Counter, frame.Bytes, header);
		bytes += opened ? opened->size() : 0;
	}
	Consume(bytes);
}

// --- refusing -----------------------------------------------------------------
//
// **A refusal here should cost the same as an accept, not less.** Poly1305
// verification compares the whole tag in constant time; an implementation that
// bailed on the first differing byte would report a smaller number on these
// rows and would be leaking the tag one byte at a time to anybody willing to
// send a few million guesses. Read these against the `Open` rows above and
// expect them to match.

BENCH("Open · 10k forged full-size frames", FRAMES) {
	Ends &ends = Session();
	const Frame &frame = ForgedOf(Packet::MAXIMUM_MESSAGE_BYTES);
	const std::vector<std::byte> &header = AssociatedData();
	uint32_t accepted = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		accepted += ends.Responder.Receiving.Open(frame.Counter, frame.Bytes, header).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Open · 10k frames with rewritten associated data", FRAMES) {
	// The header changed in flight — a rewritten channel or sequence. Same
	// answer as a forged tag, deliberately, because which check failed is
	// information about the key and is not owed to whoever sent the bytes. It
	// should also cost the same, for the same reason.
	Ends &ends = Session();
	const Frame &frame = SealedOf(Packet::MAXIMUM_MESSAGE_BYTES);
	static const std::vector<std::byte> tampered = [] {
		std::vector<std::byte> built = AssociatedData();
		built[3] = static_cast<std::byte>(static_cast<uint8_t>(built[3]) ^ 0xFFu);
		return built;
	}();

	uint32_t accepted = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		accepted += ends.Responder.Receiving.Open(frame.Counter, frame.Bytes, tampered).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Open · 10k frames under the wrong counter", FRAMES) {
	Ends &ends = Session();
	const Frame &frame = SealedOf(Packet::MAXIMUM_MESSAGE_BYTES);
	const std::vector<std::byte> &header = AssociatedData();
	uint32_t accepted = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		accepted +=
			ends.Responder.Receiving.Open(frame.Counter + 1, frame.Bytes, header).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Open · 10k frames shorter than the tag", FRAMES) {
	// The one refusal that *is* allowed to be cheap: a frame under 16 bytes
	// cannot contain a tag, so there is nothing to compare in constant time
	// against. Rejecting on the length leaks the length, which the attacker
	// chose and already knows.
	Ends &ends = Session();
	static const std::vector<std::byte> runt(4);
	const std::vector<std::byte> &header = AssociatedData();
	uint32_t accepted = 0;
	for (size_t index = 0; index < FRAMES; index++) {
		accepted += ends.Responder.Receiving.Open(0, runt, header).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

// --- the handshake ------------------------------------------------------------
//
// **Per connection, not per packet — and that is what makes it dangerous.** A
// figure here is invisible in a steady-state server and decisive in the second
// after a restart, when everybody reconnects at once. It is also the amount of
// work an unauthenticated peer can ask a server to do by sending 32 bytes,
// which is why `Cookie.hpp` exists; sizing that defence needs this number.

BENCH("Handshake · 200 full agreements", 200) {
	// Both sides, end to end: two ephemeral key pairs, two X25519 scalar
	// multiplications, and the key derivation. One iteration is one *complete*
	// agreement, so the figure is what a connection costs in total CPU across
	// both peers.
	for (size_t index = 0; index < 200; index++) {
		Ends ends = Agree();
		Consume(ends.Initiator.Sending.NextCounter());
	}
}

BENCH("Handshake::BeginFromSecret · 500", 500) {
	// Half of it: the ephemeral public key, which is one scalar multiplication
	// against the base point. This is what a server spends *before* it has any
	// reason to believe the peer is real, and therefore the number a flood
	// multiplies.
	static std::array<std::byte, Handshake::SECRET_BYTES> secret{};
	secret.fill(static_cast<std::byte>(0x33));
	for (size_t index = 0; index < 500; index++) {
		std::optional<Handshake> responder = Handshake::BeginFromSecret(HandshakeRole::Responder, secret);
		Consume(responder->Message().size());
	}
}

BENCH("Cipher::Opener::FromKey · 10k", FRAMES) {
	// Cheap by design — an `Opener` chooses no nonce, so building one from key
	// material is safe and is just a copy. If this ever becomes expensive,
	// something has started deriving per-Opener state that the handshake should
	// have derived once.
	static std::array<std::byte, Cipher::KEY_BYTES> key{};
	static std::array<std::byte, Cipher::NONCE_PREFIX_BYTES> prefix{};
	key.fill(static_cast<std::byte>(0x44));
	prefix.fill(static_cast<std::byte>(0x55));
	for (size_t index = 0; index < FRAMES; index++) {
		Consume(Cipher::Opener::FromKey(key, prefix).has_value());
	}
}
