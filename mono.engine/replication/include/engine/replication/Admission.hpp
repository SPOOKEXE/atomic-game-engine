#pragma once

// @tier L12 · shared

#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Wire.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace engine::replication {

	// Which of the five an admission message is.
	//
	// **The ordinals reach a wire, so a value may be added at the end and none
	// may be reordered or removed.**
	//
	// @since v0.3
	enum class AdmissionKind : uint8_t {
		Hello,

		Challenge,

		Answer,

		Welcome,

		// A server saying it does not serve this wire at all.
		//
		// @since v0.19
		Refuse,
	};

	// Returns a stable, human-readable name for an admission message kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(AdmissionKind kind);

	// A client asking to connect.
	//
	// @since v0.3
	struct Hello {
		// The client's ephemeral key-exchange message.
		//
		// **Ephemeral, and freshly generated per connection.** It comes from the
		// OS source rather than `core::Random`, whose determinism is the whole
		// reason it must never produce a key.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};
	};

	// A server asking the client to prove where it is.
	//
	// @since v0.3
	struct Challenge {
		// The cookie the server issued.
		//
		// **An HMAC over the peer's address and its hello, keyed by a rotating
		// secret** - which is what makes the challenge cost no state: the server
		// remembers nothing about who it has challenged, and an answer carrying
		// a cookie it did not mint verifies against nothing.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A client answering the challenge.
	//
	// @since v0.3
	struct Answer {
		// The same key the `Hello` carried, sent back with the cookie.
		//
		// **Repeated rather than remembered, which is what makes the challenge
		// stateless.** The server keeps nothing between the two: the cookie is
		// an HMAC over this address *and* this key, so an answer that changed
		// either verifies against nothing. That is why an unanswered challenge
		// costs zero bytes however many are outstanding.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		// The cookie the server issued.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A server turning a client away because it is speaking the wrong stack.
	//
	// **The reason the fallback costs one round trip instead of a timeout.** A
	// server serving only QUIC would otherwise drop a datagram-stack hello in
	// silence, which from the client's side is indistinguishable from a firewall
	// - so it waits out a deadline and then guesses. This says so instead.
	//
	// It costs the server nothing to send and nothing to remember: it is derived
	// from the datagram that caused it and is smaller than that datagram, which
	// are `net/AGENTS.md`'s two rules for answering a stranger.
	//
	// @since v0.19
	struct Refusal {
		// Which wire this server does answer.
		//
		// Stated rather than left as "not this one", because a client that is
		// told will try the right stack next rather than the next stack in its
		// list.
		net::WireKind Wire = net::WireKind::Quic;
	};

	// A server admitting the client, and proving the keys agree.
	//
	// @since v0.3
	struct Welcome {
		// The server's half of the exchange.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		// Where the server's send counter starts.
		//
		// Sent rather than assumed zero so the two ends agree on the nonce
		// sequence from the first sealed packet - a counter that disagreed would
		// decrypt as garbage rather than as an error.
		uint64_t Counter = 0;

		// Proof the server derived the same keys.
		//
		// **Without this a mismatch shows up as the first real message failing
		// to open**, which is indistinguishable from a corrupt datagram and
		// happens a tick later than the thing that caused it.
		std::array<std::byte, net::Cipher::TAG_BYTES> Confirmation{};

		// @since v0.9
		std::array<std::byte, assets::SignatureBytes::BYTES> Identity{};
	};

	// What a successful read produced.
	//
	// @since v0.3
	struct Admission {
		// Which of the payloads below was filled in.
		AdmissionKind Kind = AdmissionKind::Hello;

		// The payloads, one per kind.
		//
		// **Only the one `Kind` names has been written**; the rest are left at
		// their defaults, so reading any other is reading a value no peer sent.
		// `replication::Message` carries the argument for every field rather
		// than a union.
		//@{
		replication::Hello Hello;
		replication::Challenge Challenge;
		replication::Answer Answer;
		replication::Welcome Welcome;
		replication::Refusal Refusal;
		//@}
	};

	// Writes a hello.
	//
	// @param writer Where the bytes go.
	// @param hello  The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Hello &hello);

	// Writes a challenge.
	//
	// @param writer    Where the bytes go.
	// @param challenge The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Challenge &challenge);

	// Writes an answer.
	//
	// @param writer Where the bytes go.
	// @param answer The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Answer &answer);

	// Writes a welcome.
	//
	// @param writer  Where the bytes go.
	// @param welcome The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Welcome &welcome);

	// Writes a refusal.
	//
	// @param writer  Where the bytes go.
	// @param refusal The message to write.
	// @since v0.19
	void WriteAdmission(core::ByteWriter &writer, const Refusal &refusal);

	// Reads an admission message, refusing anything that is not exactly one.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success, untouched otherwise.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadAdmission(core::ByteReader &reader, Admission &message);

	// Wraps an admission payload as a datagram on the handshake channel.
	//
	// @param writer  Where the datagram goes.
	// @param payload The encoded admission message.
	// @return `false` when the payload does not fit a packet, which for these
	//         fixed-size messages cannot happen and is checked anyway.
	// @since v0.3
	bool FrameAdmission(core::ByteWriter &writer, std::span<const std::byte> payload);

	// The transcript a `Welcome`'s tag is computed over.
	//
	// @param clientKey The client's key exchange message.
	// @param serverKey The server's key exchange message.
	// @param cookie    The cookie the answer carried.
	// @return The bytes to pass as associated data.
	// @since v0.3
	std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> AdmissionTranscript(
		std::span<const std::byte> clientKey,
		std::span<const std::byte> serverKey,
		std::span<const std::byte> cookie
	);

	// A peer that has answered the challenge and is asking to be let in.
	//
	// @since v0.3
	struct Applicant {
		// Where it answered from.
		//
		// **Proven rather than claimed**, which is the one thing completing the
		// handshake establishes: the peer can receive at the address it named.
		// It says nothing about whether the peer is welcome, which is what a
		// policy is for.
		net::Endpoint From;

		// How many clients are already connected, so a policy can weigh this one
		// against the room left rather than against a cap it would have to know.
		size_t Connected = 0;

		// The caller's clock, in seconds.
		//
		// Passed in for `net`'s standing rule: nothing in this subsystem reads a
		// wall clock, so a rate limit is testable in a microsecond.
		double NowSeconds = 0.0;
	};

	// Decides who is allowed to connect at all.
	//
	// @since v0.3
	using AdmissionPolicy = std::function<bool(const Applicant &)>;
}
