#pragma once

// The TLS 1.3 handshake QUIC carries in its CRYPTO frames, in tree.
//
// `docs/QUIC.md` §4 puts this at the bottom of a table of five backends and
// calls it "the fallback, and the only option with zero new build
// dependencies". It is what got taken, and the reason is the rule
// `mono.vendor/AGENTS.md` states and `THIRD_PARTY_NOTICES.md` repeats: **a
// fresh clone needs CMake, Ninja and a C++ compiler and nothing else.** Every
// other candidate costs that. AWS-LC's pre-generated build files were the one
// that might not have, and betting the transport on a claim that has to hold on
// three platforms is a bet made before the work rather than after it.
//
// **What this gives up is interoperability, and it is worth being precise about
// what that means.** Nothing here will talk to a browser, to curl, or to
// anything that is not this engine, and the HTTP/3 half of D00014's
// second-consumer argument is therefore not served by this file. What it is is
// TLS 1.3 on the wire - RFC 8446's messages, RFC 8446's key schedule, RFC 9001's
// levels - rather than a private protocol wearing the name, so the day a
// certificate-verifying backend is wanted, what changes is which object fills
// this interface and not one line of the transport above it.
//
// **The subset is small and each omission closes something. Do not widen it
// casually**, which is the same sentence `net/AGENTS.md` writes about `http/`
// and for the same reason - a parser behind a port is not the place for
// Postel's rule:
//
// - **One key exchange group: X25519.** No HelloRetryRequest follows, because
//   there is nothing to retry with. A ClientHello with a share this end cannot
//   use is refused rather than negotiated down.
// - **Two cipher suites**, `TLS_CHACHA20_POLY1305_SHA256` first because it is
//   the engine's own - see `Cipher.hpp` on why that matters without AES
//   instructions - and `TLS_AES_128_GCM_SHA256` because TLS 1.3 requires it.
//   Both are SHA-256, so there is one key schedule and one transcript width.
// - **One signature scheme: Ed25519**, which `assets::SigningKey` already uses,
//   over the same Donna implementation.
// - **Raw public keys, RFC 7250, not certificates.** There is no chain to
//   build, no name to match and no authority to trust, because the deployment
//   model has none of those: `replication::ConnectorSettings::ServerIdentity`
//   pins one key, which is D00006's answer, and this is that answer spelled in
//   TLS's own extension rather than beside it. **The pinning must survive any
//   later swap of this file** - an unauthenticated agreement is safe against a
//   listener and not against a relay, which can hold one exchange with each
//   side and read everything.
// - **No 0-RTT, no session tickets, no resumption, no client certificates, no
//   renegotiation.** Early data is a replayable request, and this transport
//   carries inputs to a simulation.
//
// **Nothing here reads a clock**, which matters more than it looks: TLS
// certificate validity is the usual reason a handshake needs one, and a raw
// public key has no validity period to check. So `net`'s standing rule survives
// into the crypto, and `just determinism` and `just replay-check` are unaffected
// for the same two reasons they already were.
//
// The output is a list of events rather than a set of callbacks. A handshake
// step produces bytes to send, keys to install and a completion, always in the
// order they must be acted on, and the caller drains them - which is what makes
// this testable with no socket, no `ngtcp2_conn` and no transport at all.
//
// @tier L11 · shared

#include <engine/net/quic/Crypto.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::net::quic {

	// Which encryption level a handshake message or a key belongs to.
	//
	// RFC 9001 §4.1's three, minus 0-RTT which this does not offer. They are an
	// ordering as much as a naming: Initial keys are derived from a connection
	// id anybody can read, Handshake keys from the key exchange, and Application
	// keys only once the peer has been authenticated.
	//
	// @since v0.19
	enum class Level : uint8_t {
		// Derived from the client's original destination connection id, and
		// therefore readable by anybody who saw the first datagram. Carries the
		// ClientHello and the ServerHello.
		Initial,

		// Derived from the X25519 agreement. Carries the rest of the handshake.
		Handshake,

		// The 1-RTT keys everything after the handshake travels under.
		Application,
	};

	// Returns a stable, human-readable name for an encryption level.
	//
	// @param level The level.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(Level level);

	// An Ed25519 public key - a server's identity, and the whole of what a
	// client pins.
	inline constexpr size_t IDENTITY_BYTES = 32;

	// The seed a server holds. The private half, and never on the wire.
	inline constexpr size_t IDENTITY_SEED_BYTES = 32;

	// An Ed25519 signature, over the handshake transcript.
	inline constexpr size_t IDENTITY_SIGNATURE_BYTES = 64;

	// The Ed25519 public key a seed produces.
	//
	// A server's identity is derived rather than stored beside its seed, so the
	// two cannot drift apart - and an operator publishing a key can produce it
	// from the seed it already has rather than from a note somebody kept.
	//
	// @param seed The `IDENTITY_SEED_BYTES` a server holds.
	// @return The public key. All zeros when the seed is the wrong length, which
	//         is not a valid key and fails every check downstream.
	// @since v0.19
	std::array<std::byte, IDENTITY_BYTES> IdentityFor(std::span<const std::byte> seed);

	// Draws a fresh identity seed from the operating system's entropy.
	//
	// **What a server with no operator-supplied key gets, now that QUIC is the
	// default wire.** A QUIC server proves who it is inside its own handshake,
	// so unlike the datagram wire there is no anonymous mode to fall back to -
	// the handshake needs *a* key whether or not anybody pinned it. An ephemeral
	// one gives the same guarantee the datagram wire's anonymous mode gives and
	// no more: encrypted against a listener, open to a relay. It changes every
	// run, so it is never what an operator publishes.
	//
	// @param out Exactly `IDENTITY_SEED_BYTES`.
	// @return `false` when the length is wrong or the operating system refused
	//         entropy, in which case `out` is untouched and the caller must not
	//         serve QUIC.
	// @since v0.19
	bool DrawIdentitySeed(std::span<std::byte> out);

	// What a handshake needs before it can start.
	//
	// @since v0.19
	struct TlsSettings {
		// The application protocol, offered by the client and confirmed by the
		// server. A mismatch is a refusal rather than a negotiation.
		//
		// **Not optional.** ALPN exists so that two protocols cannot end up
		// sharing a port and a key schedule and differing only in what the first
		// byte after the handshake happens to mean.
		std::string Protocol = "atomic/1";

		// The server's Ed25519 seed. Servers only, and required for them.
		//
		// Wiped when this settings object dies, which is why it is an array here
		// rather than a span into somebody else's storage.
		std::array<std::byte, IDENTITY_SEED_BYTES> Seed{};

		// Whether `Seed` holds anything. A server without one cannot answer a
		// handshake and says so at construction rather than three messages in.
		bool HasSeed = false;

		// The server identity a client insists on. `D00006`'s pinned key.
		std::array<std::byte, IDENTITY_BYTES> Expected{};

		// Whether `Expected` is checked.
		//
		// **A client that leaves this off has an encrypted connection to
		// something rather than to this server**, which is safe against a
		// listener and not against a relay. The default is on so that turning it
		// off is a decision somebody wrote down.
		bool PinIdentity = true;
	};

	// One TLS 1.3 handshake, driven by whoever owns the CRYPTO stream.
	//
	// Move-only and not copyable: it holds an ephemeral private key and, on a
	// server, the identity seed. A second object counting from the same state is
	// the shape `Cipher.hpp` refuses for the same reason.
	//
	// @since v0.19
	class Tls {
	  public:
		// Which end of the handshake this is.
		//
		// @since v0.19
		enum class Role : uint8_t {
			// Offers, and checks the server's signature.
			Client,

			// Answers, and signs the transcript.
			Server,
		};

		// One thing the handshake wants done.
		//
		// **The order in `Pending` is the order they must be acted on**, which is
		// the whole reason this is a queue rather than a set of getters. A write
		// key installed before the bytes that must go out under the old one is a
		// packet the peer cannot open.
		//
		// @since v0.19
		struct Event {
			// What kind of thing this is.
			//
			// @since v0.19
			enum class Kind : uint8_t {
				// `Bytes` are handshake bytes to put in a CRYPTO frame at `At`.
				Send,

				// `Secret` is the traffic secret to open incoming packets at
				// `At` with.
				ReadKey,

				// `Secret` is the traffic secret to seal outgoing packets at
				// `At` with.
				WriteKey,

				// `Bytes` are the peer's QUIC transport parameters.
				//
				// **Emitted when the peer is authenticated, not when the bytes
				// arrive.** A client reads the server's parameters out of
				// EncryptedExtensions, which is three messages before the
				// signature that says who wrote them; acting on them earlier
				// would let an unauthenticated party set this end's flow-control
				// limits.
				PeerParameters,

				// The handshake finished. Nothing follows it.
				Complete,
			};

			// What this event is.
			Kind What = Kind::Complete;

			// Which encryption level it concerns.
			Level At = Level::Initial;

			// The bytes, for `Send` and `PeerParameters`.
			std::vector<std::byte> Bytes;

			// The traffic secret, for `ReadKey` and `WriteKey`.
			std::array<std::byte, SECRET_BYTES> Secret{};
		};

		// Opens a handshake.
		//
		// @param role     Which end this is.
		// @param settings The identity, the protocol and the pinning.
		Tls(Role role, TlsSettings settings);

		~Tls();

		Tls(const Tls &) = delete;
		Tls &operator=(const Tls &) = delete;

		// Takes over another handshake's state.
		//
		// @param other The handshake to move from. Left holding nothing.
		Tls(Tls &&other) noexcept;

		// Takes over another handshake's state.
		//
		// @param other The handshake to move from. Left holding nothing.
		// @return This handshake.
		Tls &operator=(Tls &&other) noexcept;

		// Supplies this end's QUIC transport parameters.
		//
		// They travel inside the handshake - in the ClientHello for a client and
		// in EncryptedExtensions for a server - which is what binds them to the
		// key exchange.
		//
		// A client sets them before `Begin`. **A server cannot**, and
		// `NeedsParameters` is why.
		//
		// @param parameters The encoded parameters, from the transport.
		// @since v0.19
		void SetTransportParameters(std::span<const std::byte> parameters);

		// Whether the handshake is waiting for this end's transport parameters.
		//
		// **Only ever true on a server, and only once.** A server's parameters
		// depend on the client's - RFC 9368's version information names the
		// version in force, and a transport does not know which that is until it
		// has read what the client offered - so the handshake stops after the
		// ServerHello, hands the client's parameters over, and waits to be told
		// this end's before it writes EncryptedExtensions. Encoding them any
		// earlier produces a chosen version of zero, which the far end rejects as
		// malformed with nothing saying which of the two ends was wrong.
		//
		// @return `true` when `SetTransportParameters` and then `Resume` are what
		//         the caller owes it.
		// @since v0.19
		bool NeedsParameters() const {
			return Phase == Stage::AwaitingParameters;
		}

		// Writes the server's flight, once its parameters are known.
		//
		// @return `false` when the handshake was not waiting, or was refused
		//         while writing.
		// @since v0.19
		bool Resume();

		// Produces the ClientHello. Clients only.
		//
		// @return `false` on a server, or when the operating system refused
		//         entropy - which is a connection to refuse rather than to open
		//         with a weaker key, the same answer `Handshake::Begin` gives.
		// @since v0.19
		bool Begin();

		// Takes handshake bytes that arrived in CRYPTO frames.
		//
		// **Every byte of it is hostile.** A message for a level this end is not
		// at, a length running past the buffer, an extension that is not
		// understood, a signature that does not verify - all are refusals, and a
		// refused handshake stays refused.
		//
		// Fragmentation is expected: CRYPTO frames split messages wherever the
		// sender's packets ended, so bytes are buffered per level until a whole
		// message is there.
		//
		// @param level The encryption level the frame arrived at.
		// @param data  The bytes.
		// @return `false` when the handshake is now failed, in which case
		//         `Alert` says what to tell the peer.
		// @since v0.19
		bool Receive(Level level, std::span<const std::byte> data);

		// What the handshake wants done, in order.
		//
		// @return The events, valid until the next call that changes state.
		// @since v0.19
		std::span<const Event> Pending() const {
			return Events;
		}

		// Drops the events, once the caller has acted on them.
		//
		// @since v0.19
		void ClearPending();

		// Whether the handshake finished successfully.
		//
		// @return `true` once both ends' Finished messages have verified.
		// @since v0.19
		bool Complete() const {
			return Phase == Stage::Done;
		}

		// Whether the handshake was refused.
		//
		// @return `true` once anything has been rejected.
		// @since v0.19
		bool Failed() const {
			return Phase == Stage::Failed;
		}

		// The TLS alert to close the connection with.
		//
		// QUIC carries it as a `CRYPTO_ERROR` code of `0x100` plus this, which
		// is RFC 9001 §4.8. Zero when nothing has gone wrong.
		//
		// @return The alert description.
		// @since v0.19
		uint8_t Alert() const {
			return AlertCode;
		}

		// Why the handshake was refused, for a log line.
		//
		// @return A view valid for the lifetime of the process. Empty when
		//         nothing has gone wrong.
		// @since v0.19
		const char *Failure() const {
			return Reason;
		}

		// The AEAD both ends agreed on.
		//
		// @return The suite. Meaningless before the ServerHello.
		// @since v0.19
		Aead Suite() const {
			return Chosen;
		}

		// The header-protection cipher that goes with the suite.
		//
		// @return The cipher. Meaningless before the ServerHello.
		// @since v0.19
		HeaderCipher Header() const {
			return ChosenHeader;
		}

		// Derives a value both ends can compute and nobody else can.
		//
		// RFC 8446 §7.5's exporter. **What it is for is binding something to
		// this connection**: a client's identity claim is signed over an
		// exported value, so a signature captured from one connection proves
		// nothing on another, and a relay that holds one handshake with each
		// side cannot carry the claim across. The hand-rolled stack got the same
		// property out of `AdmissionTranscript`, which QUIC has no equivalent of
		// - this is what replaces it.
		//
		// @param label The exporter label. Two callers with different labels get
		//        unrelated values from one connection.
		// @param out   Where the value goes.
		// @return `false` before the handshake completes, or when `out` is
		//         longer than one expansion can produce.
		// @since v0.19
		bool Export(std::string_view label, std::span<std::byte> out) const;

		// The peer's Ed25519 public key, as it proved possession of it.
		//
		// **On a client this is the identity check's subject and not its
		// result.** When `TlsSettings::PinIdentity` is on the handshake has
		// already refused anything else; when it is off, this is what a caller
		// would have to check for itself.
		//
		// @return The key. Empty on a server, which asks for no client
		//         certificate.
		// @since v0.19
		std::span<const std::byte> PeerIdentity() const;

	  private:
		enum class Stage : uint8_t {
			Fresh,
			AwaitingParameters,
			AwaitingServerHello,
			AwaitingServerHandshake,
			AwaitingClientFinished,
			Done,
			Failed,
		};

		bool Refuse(uint8_t alert, const char *reason);
		bool ConsumeMessages(Level level);
		bool OnClientHello(std::span<const std::byte> body, std::span<const std::byte> whole);
		bool OnServerHello(std::span<const std::byte> body, std::span<const std::byte> whole);
		bool OnEncryptedExtensions(std::span<const std::byte> body, std::span<const std::byte> whole);
		bool OnCertificate(std::span<const std::byte> body, std::span<const std::byte> whole);
		bool OnCertificateVerify(std::span<const std::byte> body, std::span<const std::byte> whole);
		bool OnFinished(std::span<const std::byte> body, std::span<const std::byte> whole);

		void Absorb(std::span<const std::byte> message);
		std::array<std::byte, SECRET_BYTES> TranscriptHash() const;
		std::array<std::byte, SECRET_BYTES>
		DeriveSecret(std::span<const std::byte> secret, std::string_view label) const;
		std::array<std::byte, SECRET_BYTES> FinishedTag(std::span<const std::byte> base) const;
		bool DeriveHandshakeSecrets();
		void DeriveApplicationSecrets();
		void Wipe();

		Role Side = Role::Client;
		Stage Phase = Stage::Fresh;
		TlsSettings Settings;

		Aead Chosen = Aead::ChaCha20Poly1305;
		HeaderCipher ChosenHeader = HeaderCipher::ChaCha20;

		// The ephemeral X25519 pair, and the peer's public half.
		//@{
		std::array<std::byte, 32> EphemeralSecret{};
		std::array<std::byte, 32> EphemeralPublic{};
		std::array<std::byte, 32> PeerShare{};
		//@}

		// The identity a server proved, or that a client is talking to.
		std::array<std::byte, IDENTITY_BYTES> Identity{};
		bool HasIdentity = false;

		// Every handshake message, in order, header included. The key schedule
		// hashes prefixes of this at four different points, which is why it is
		// kept whole rather than folded into a running hash.
		std::vector<std::byte> Transcript;

		// The key schedule.
		//@{
		std::array<std::byte, SECRET_BYTES> HandshakeSecret{};
		std::array<std::byte, SECRET_BYTES> ClientHandshakeSecret{};
		std::array<std::byte, SECRET_BYTES> ServerHandshakeSecret{};
		std::array<std::byte, SECRET_BYTES> ClientApplicationSecret{};
		std::array<std::byte, SECRET_BYTES> ServerApplicationSecret{};
		std::array<std::byte, SECRET_BYTES> ExporterSecret{};
		//@}

		std::vector<std::byte> LocalParameters;
		std::vector<std::byte> RemoteParameters;

		// Bytes that arrived and do not yet make a whole message, one buffer per
		// level. A CRYPTO frame splits wherever the sender's packet ended.
		std::array<std::vector<std::byte>, 3> Incoming;

		std::vector<Event> Events;
		uint8_t AlertCode = 0;
		const char *Reason = "";
	};
}
