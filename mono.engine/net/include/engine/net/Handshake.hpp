#pragma once

// The key agreement, as a state machine a session can drive without knowing any
// crypto.
//
// X25519 (RFC 7748), one message each way, and out the other side come the two
// directional ciphers ready to use. A caller sends Message(), feeds the peer's
// to Consume(), and takes the keys — it never sees a key, a scalar or a shared
// secret, which is the point: the parts of this that are easy to get wrong are
// the parts a caller is never handed.
//
// **Both sides are ephemeral.** The private key exists for the life of one
// Handshake object and is wiped the moment it has been used, so a key lifted off
// a machine afterwards decrypts nothing anybody recorded earlier. That forward
// secrecy is the reason for an agreement here rather than a configured shared
// key, which would make every past session readable from one file on one server.
//
// **This authenticates nobody on its own, and the layer above closes that.** An
// unauthenticated agreement is safe against a listener and not against a relay:
// whoever carries the two messages can substitute their own key and hold a
// session with each side.
//
// It is closed at v0.9, and **not here** — which was always the plan: the fix is
// to bind the exchange to a *server identity*, and this module has no idea what
// a server is. `replication::Admission` signs its transcript with the Ed25519
// key `assets` verifies manifests with, and `ConnectorSettings::ServerIdentity`
// is the pin a client checks it against. What this file supplies is the
// transcript that makes such a signature meaningful: the session keys are
// already a function of the exact bytes exchanged, so a signature over those
// bytes commits to this exchange and no other.
//
// **A handshake with no identity above it is still unauthenticated.** That is
// the default, both ends log it, and it is a deployment choice rather than a
// property of this code.
//
// Time is passed in, never read — the module rule, and here it costs nothing.
// No step of this has a deadline of its own; a handshake that never finishes is
// a `Link` sitting in `Connecting` until the handshake timeout it was given.
//
// @tier L11 · shared

#include <engine/net/Cipher.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::net {

	// Which side of the agreement this is.
	//
	// The two sides run the same arithmetic and differ only in bookkeeping: who
	// is written first into the transcript, and which of the two derived keys is
	// for sending. Somebody has to be first, and connecting is the natural way to
	// decide it.
	enum class HandshakeRole : uint8_t {
		Initiator, ///< The side that dials. Its public key leads the transcript.
		Responder, ///< The side that answers.
	};

	// Where a handshake is in its one-way lifecycle.
	//
	// Forward only, like the connection lifecycle above it. Nothing here returns
	// to an earlier state and a refused handshake stays refused — a peer that
	// gets one message wrong does not get to try again on the same keys.
	enum class HandshakeState : uint8_t {
		AwaitingPeer, ///< Our message is ready to send; the peer's has not arrived.
		Established,  ///< Both messages exchanged and the keys derived, waiting to be taken.
		Complete,	  ///< The keys have been taken. Nothing of value is left here.
		Refused,	  ///< The peer's message was not a usable one. Terminal.
	};

	// One X25519 exchange, from a fresh key pair to the two ciphers it produces.
	//
	// Move-only and single-use. A second session is a second Handshake, for the
	// same reason a reconnect is a new `ConnectionId`: an object that can be
	// restarted is one every caller has to re-check after every step.
	//
	// @since v0.3
	class Handshake {
	  public:
		// The message each side sends: its ephemeral public key, and nothing
		// else.
		//
		// No magic and no version field, deliberately. This rides inside a
		// `Packet`, which already refuses a wrong magic and an unknown version,
		// and a second framing that could disagree with the first is exactly the
		// dialect `Packet` exists to prevent.
		static constexpr size_t MESSAGE_BYTES = 32;

		// The ephemeral secret a deterministic handshake is built from.
		static constexpr size_t SECRET_BYTES = 32;

		// What a completed handshake hands over: one cipher for each direction.
		//
		// Already the right way round for the side that took them. There is no
		// key to wire up and no chance of sealing under the receiving key,
		// because a Cipher::Opener has no way to seal.
		struct Session {
			// Seals what this side sends.
			Cipher::Sealer Sending;

			// Opens what the peer sends.
			Cipher::Opener Receiving;
		};

		// Starts a handshake on a fresh ephemeral key pair from the operating
		// system's entropy.
		//
		// This is the call production code makes. `core::Random` is a
		// deterministic simulation generator and using it here would make every
		// session's keys predictable from a seed; the entropy comes from the OS
		// and from nowhere else.
		//
		// @param role Which side this is.
		// @return The handshake, or nothing if the operating system refused to
		//         provide entropy — which is a refusal to connect, never a
		//         fallback to a weaker source.
		static std::optional<Handshake> Begin(HandshakeRole role);

		// Starts a handshake on a caller-supplied ephemeral secret.
		//
		// For a test that needs the same keys twice, and for the published RFC
		// 7748 vectors. The scalar is clamped as X25519 requires, so any 32 bytes
		// are usable.
		//
		// @param role Which side this is.
		// @param ephemeralSecret Exactly SECRET_BYTES of high-entropy secret.
		// @return The handshake, or nothing if the secret is the wrong length.
		// @warning Whatever is passed here is the whole secret for this session.
		//          Prefer Begin, which cannot be handed a bad one.
		static std::optional<Handshake>
		BeginFromSecret(HandshakeRole role, std::span<const std::byte> ephemeralSecret);

		// Zeroes the ephemeral secret and whatever keys have not been taken.
		~Handshake();

		Handshake(const Handshake &) = delete;
		Handshake &operator=(const Handshake &) = delete;

		// Moves the exchange, leaving the source zeroed and refused.
		Handshake(Handshake &&other) noexcept;

		// Moves the exchange, zeroing both this handshake's secrets and the
		// source's.
		Handshake &operator=(Handshake &&other) noexcept;

		// Which side this is.
		HandshakeRole Role() const {
			return Side;
		}

		// Where in the lifecycle this handshake is.
		HandshakeState State() const {
			return Phase;
		}

		// The message to send to the peer: MESSAGE_BYTES of public key.
		//
		// Ready before anything has been received, so both sides can send
		// immediately rather than one waiting for the other. A view into this
		// handshake, valid while it lives.
		std::span<const std::byte> Message() const;

		// Consumes the peer's message and derives the keys.
		//
		// Every byte of it is hostile. Refuses a message that is not exactly
		// MESSAGE_BYTES, a public key equal to our own — a reflection, which is
		// either a loopback wired to itself or somebody echoing — and a key that
		// agrees to the all-zero secret, which is what every low-order point on
		// the curve produces and what RFC 7748 §6.1 asks implementations to
		// check. Each refusal is terminal.
		//
		// Answers false to a second call as well. Consuming twice is either a
		// replayed packet or two code paths both believing they own the
		// transition, and accepting it quietly hides both —
		// `Link::CompleteHandshake` is not idempotent for the same reason. An
		// already established handshake is left as it is rather than torn down,
		// because losing a working session to a duplicate packet would be the
		// worse of the two outcomes.
		//
		// @param peerMessage The MESSAGE_BYTES the peer sent.
		// @return Whether the handshake is now established. A message that was
		//         not a usable one leaves it Refused and yielding no keys, ever.
		bool Consume(std::span<const std::byte> peerMessage);

		// Hands over the two ciphers, once.
		//
		// Moves them out rather than copying: after this the handshake holds no
		// key material at all, which is the state a completed handshake should
		// sit in if a caller keeps it around.
		//
		// @return The two ciphers, or nothing unless this is the first call on an
		//         Established handshake.
		std::optional<Session> TakeKeys();

	  private:
		Handshake() = default;

		// The key material derived from one exchange, before it becomes ciphers.
		//
		// Kept as bytes for the moment between deriving and taking, and wiped
		// whether or not anybody takes it.
		struct Derived {
			std::array<uint8_t, Cipher::KEY_BYTES> SendKey{};
			std::array<uint8_t, Cipher::NONCE_PREFIX_BYTES> SendNoncePrefix{};
			std::array<uint8_t, Cipher::KEY_BYTES> ReceiveKey{};
			std::array<uint8_t, Cipher::NONCE_PREFIX_BYTES> ReceiveNoncePrefix{};
		};

		// Turns the agreement into the two directional keys, in `Keys`.
		void
		Derive(std::span<const uint8_t, MESSAGE_BYTES> peerPublic, std::span<const uint8_t> sharedSecret);

		// Zeroes the secret and every derived key. The state is the caller's to
		// set afterwards: a refusal, a move and a completion all wipe, and all
		// three land somewhere different.
		void Forget();

		HandshakeRole Side = HandshakeRole::Initiator;
		HandshakeState Phase = HandshakeState::AwaitingPeer;

		std::array<uint8_t, SECRET_BYTES> Secret{};
		std::array<uint8_t, MESSAGE_BYTES> Public{};
		Derived Keys;
	};
}
