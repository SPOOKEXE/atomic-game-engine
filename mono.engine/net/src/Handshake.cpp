#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Handshake.hpp>

#include <cryptopp/donna.h>
#include <cryptopp/hkdf.h>
#include <cryptopp/misc.h>
#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>
#include <cstring>
#include <string_view>

namespace engine::net {

	namespace {
		// Zeroes secret bytes.
		//
		// SecureWipeBuffer rather than a loop or memset: an ordinary store to
		// memory nothing reads again is exactly what a compiler is allowed to
		// delete, and it does.
		//
		// The same three lines as `assets/src/SecureWipe.hpp` and as
		// `net/src/Cipher.cpp`. Duplicated because the first is another module's
		// private header and the second is a translation unit — a module may
		// include neither. Promoting the helper to `core` is the fix and is a
		// change to a module this file does not own.
		void SecureWipe(std::span<uint8_t> bytes) {
			CryptoPP::SecureWipeBuffer(bytes.data(), bytes.size());
		}

		// The salt every key in this protocol is derived under.
		//
		// Domain separation, for the reason `assets/src/Signature.cpp` tags what
		// it signs: material derived from an X25519 agreement is otherwise
		// interchangeable with material derived from any other X25519 agreement
		// in the product, and two protocols sharing a derivation is how a key
		// from one becomes usable in the other.
		//
		// **The version is inside the string.** A v2 derivation is a different
		// string and therefore a different key, so a v1 peer and a v2 peer fail
		// to talk rather than half-talking.
		constexpr std::string_view AGREEMENT_SALT = "atomic-net-handshake-v1";

		// One key and one nonce prefix per direction, from one expansion.
		constexpr size_t DERIVED_BYTES = 2 * (Cipher::KEY_BYTES + Cipher::NONCE_PREFIX_BYTES);

		const CryptoPP::byte *Bytes(std::string_view text) {
			return reinterpret_cast<const CryptoPP::byte *>(text.data());
		}
	}

	std::optional<Handshake> Handshake::Begin(HandshakeRole role) {
		std::array<uint8_t, SECRET_BYTES> secret{};

		try {
			CryptoPP::OS_GenerateRandomBlock(false, secret.data(), secret.size());
		} catch (const CryptoPP::Exception &) {
			// Protecting against one thing: the operating system refusing
			// entropy — no /dev/urandom inside a sandbox, or a handle exhausted.
			// The answer is to refuse the connection. A fallback to a weaker
			// source here would be a session anybody can decrypt, reported as a
			// success.
			SecureWipe(secret);
			core::Metrics::Count("net.handshake.no_entropy", 1.0);
			return std::nullopt;
		}

		auto handshake = BeginFromSecret(role, std::as_bytes(std::span<const uint8_t>(secret)));
		SecureWipe(secret);
		return handshake;
	}

	std::optional<Handshake>
	Handshake::BeginFromSecret(HandshakeRole role, std::span<const std::byte> ephemeralSecret) {
		if (ephemeralSecret.size() != SECRET_BYTES) {
			return std::nullopt;
		}

		Handshake handshake;
		handshake.Side = role;
		std::memcpy(handshake.Secret.data(), ephemeralSecret.data(), SECRET_BYTES);

		// Donna clamps the scalar itself, which is why any 32 bytes are a valid
		// secret and why the published RFC 7748 vectors — whose private keys are
		// written unclamped — produce the public keys the RFC prints.
		if (CryptoPP::Donna::curve25519_mult(handshake.Public.data(), handshake.Secret.data()) != 0) {
			handshake.Forget();
			handshake.Phase = HandshakeState::Refused;
			return std::nullopt;
		}

		return handshake;
	}

	Handshake::~Handshake() {
		Forget();
	}

	Handshake::Handshake(Handshake &&other) noexcept
		: Side(other.Side), Phase(other.Phase), Secret(other.Secret), Public(other.Public), Keys(other.Keys) {
		// The source keeps neither the secret nor the keys, for the reason
		// SigningKey does not: a moved-from object whose secret is still in its
		// storage is a copy of that secret nobody believes exists.
		other.Forget();
		other.Phase = HandshakeState::Refused;
	}

	Handshake &Handshake::operator=(Handshake &&other) noexcept {
		if (this != &other) {
			Forget();
			Side = other.Side;
			Phase = other.Phase;
			Secret = other.Secret;
			Public = other.Public;
			Keys = other.Keys;
			other.Forget();
			other.Phase = HandshakeState::Refused;
		}
		return *this;
	}

	std::span<const std::byte> Handshake::Message() const {
		return std::as_bytes(std::span<const uint8_t>(Public));
	}

	bool Handshake::Consume(std::span<const std::byte> peerMessage) {
		ENGINE_PROFILE("Handshake::Consume");

		if (Phase != HandshakeState::AwaitingPeer) {
			// A replayed message, or two code paths both believing they own the
			// transition. An established handshake is left alone rather than
			// torn down: the caller's answer to false is a protocol error, and
			// destroying a working session on a duplicate packet would be worse
			// than the duplicate.
			return false;
		}

		const auto refuse = [this]() {
			Forget();
			Phase = HandshakeState::Refused;
			core::Metrics::Count("net.handshake.refused", 1.0);
			return false;
		};

		if (peerMessage.size() != MESSAGE_BYTES) {
			return refuse();
		}

		std::array<uint8_t, MESSAGE_BYTES> peer{};
		std::memcpy(peer.data(), peerMessage.data(), MESSAGE_BYTES);

		// Our own key, echoed back. Either a loopback wired to itself or somebody
		// reflecting, and neither is a peer worth deriving a key with.
		if (CryptoPP::VerifyBufsEqual(peer.data(), Public.data(), MESSAGE_BYTES)) {
			return refuse();
		}

		std::array<uint8_t, MESSAGE_BYTES> shared{};
		if (CryptoPP::Donna::curve25519_mult(shared.data(), Secret.data(), peer.data()) != 0) {
			SecureWipe(shared);
			return refuse();
		}

		// Every low-order point on the curve agrees to all zeros against a
		// clamped scalar, so this one check refuses the whole family of them —
		// the SHOULD in RFC 7748 §6.1. Without it a peer can force a session key
		// it knows in full, because it knows the agreement was zero.
		const std::array<uint8_t, MESSAGE_BYTES> zero{};
		if (CryptoPP::VerifyBufsEqual(shared.data(), zero.data(), zero.size())) {
			SecureWipe(shared);
			return refuse();
		}

		Derive(peer, shared);
		SecureWipe(shared);

		// The scalar has done the only job it has. Held any longer it is forward
		// secrecy waiting to be lost to whatever reads this process's memory.
		SecureWipe(Secret);

		Phase = HandshakeState::Established;
		core::Metrics::Count("net.handshake.established", 1.0);
		return true;
	}

	void Handshake::Derive(
		std::span<const uint8_t, MESSAGE_BYTES> peerPublic, std::span<const uint8_t> sharedSecret
	) {
		// The transcript, in one order both sides can agree on without
		// negotiating it: the initiator's key first. Binding it costs one hash
		// and makes the session keys a function of the exact bytes exchanged, so
		// a field added to the message later is covered by construction rather
		// than by somebody remembering to add it here.
		const bool initiator = Side == HandshakeRole::Initiator;
		std::array<uint8_t, 2 * MESSAGE_BYTES> transcript{};
		std::memcpy(transcript.data(), initiator ? Public.data() : peerPublic.data(), MESSAGE_BYTES);
		std::memcpy(
			transcript.data() + MESSAGE_BYTES, initiator ? peerPublic.data() : Public.data(), MESSAGE_BYTES
		);

		std::array<uint8_t, DERIVED_BYTES> derived{};
		CryptoPP::HKDF<CryptoPP::SHA256> expand;
		expand.DeriveKey(
			derived.data(),
			derived.size(),
			sharedSecret.data(),
			sharedSecret.size(),
			Bytes(AGREEMENT_SALT),
			AGREEMENT_SALT.size(),
			transcript.data(),
			transcript.size()
		);

		// One expansion, split by position: the initiator's sending key and its
		// nonce prefix, then the responder's. Two halves of one output rather
		// than two expansions, because the split is what makes them different
		// and a second call would only move that fact into an info string.
		constexpr size_t HALF = Cipher::KEY_BYTES + Cipher::NONCE_PREFIX_BYTES;
		const uint8_t *toResponder = derived.data();
		const uint8_t *toInitiator = derived.data() + HALF;
		const uint8_t *sending = initiator ? toResponder : toInitiator;
		const uint8_t *receiving = initiator ? toInitiator : toResponder;

		std::memcpy(Keys.SendKey.data(), sending, Cipher::KEY_BYTES);
		std::memcpy(Keys.SendNoncePrefix.data(), sending + Cipher::KEY_BYTES, Cipher::NONCE_PREFIX_BYTES);
		std::memcpy(Keys.ReceiveKey.data(), receiving, Cipher::KEY_BYTES);
		std::memcpy(
			Keys.ReceiveNoncePrefix.data(), receiving + Cipher::KEY_BYTES, Cipher::NONCE_PREFIX_BYTES
		);

		SecureWipe(derived);
	}

	std::optional<Handshake::Session> Handshake::TakeKeys() {
		if (Phase != HandshakeState::Established) {
			return std::nullopt;
		}

		Session session{
			Cipher::Sealer(
				std::as_bytes(std::span<const uint8_t>(Keys.SendKey)),
				std::as_bytes(std::span<const uint8_t>(Keys.SendNoncePrefix))
			),
			Cipher::Opener(
				std::as_bytes(std::span<const uint8_t>(Keys.ReceiveKey)),
				std::as_bytes(std::span<const uint8_t>(Keys.ReceiveNoncePrefix))
			),
		};

		// Taken once and gone. A handshake a caller keeps around holds no key
		// material afterwards, and a second call answers nothing rather than
		// handing a second Sealer the same key — which is the one thing that
		// would let a nonce repeat.
		Forget();
		Phase = HandshakeState::Complete;
		return session;
	}

	void Handshake::Forget() {
		SecureWipe(Secret);
		SecureWipe(Keys.SendKey);
		SecureWipe(Keys.SendNoncePrefix);
		SecureWipe(Keys.ReceiveKey);
		SecureWipe(Keys.ReceiveNoncePrefix);
	}
}
