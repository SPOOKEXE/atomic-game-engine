#pragma once

// What a stranger and a server say to each other before a slot exists.
//
// Four messages, two round trips, and **the server holds nothing until the
// third one arrives**:
//
//     client  --- Hello     (its key exchange message)       --> server
//     client  <-- Challenge (a cookie, derived not stored)   --- server
//     client  --- Answer    (the same message, plus cookie)  --> server
//     client  <-- Welcome   (its key exchange message, and a
//                            tag proving both sides derived
//                            the same keys)                  --- server
//
// **The first two cost the server nothing.** `net::Cookie` derives the cookie
// from a secret the server already holds and the bytes the client already sent,
// so a `Hello` allocates no slot, no session, no link, no reliability window
// and no map entry — the answer to "how much state does an unanswered challenge
// cost" is zero bytes, and it is zero however many are outstanding. A server
// that remembered a pending challenge per source address would have moved the
// exhaustion target rather than removed it.
//
// **Only an `Answer` reaches anything expensive**, and in this order: the
// cookie, then `MaximumClients`, then the game's admission policy, then the
// X25519 agreement, and only then the slot. Every step is cheaper than the one
// after it and each is a reason to stop.
//
// **The `Welcome`'s tag is the exchange proving itself.** Both sides derive
// `net::Cipher` keys from the agreement; the server seals an empty frame over
// the transcript — client key, server key, cookie — and the client opens it.
// A tampered key in either direction produces different keys and the tag does
// not verify, so the exchange fails *here* rather than being half-accepted and
// noticed later. The two ciphers are destroyed immediately afterwards: **this
// engine does not encrypt the stream yet**, and saying so plainly beats keeping
// a `Sealer` around that nothing seals with. Stream encryption is `net`'s own
// outstanding item, not something this file quietly implies.
//
// **What none of this proves is who the peer is.** The cookie proves it can
// receive at the address it wrote, and the agreement proves it can do
// arithmetic. Neither is a reason to let it into a game — that decision is
// `Listener::SetAdmission`'s, and `net::Handshake`'s own header says the
// agreement is unauthenticated against a relay.
//
// **These are deliberately not `MessageKind`s.** A `Protocol.hpp` message goes
// through `Session::Send`, which needs a `Link` that is `Connected` — the state
// this exchange exists to reach. Adding them there would put four messages in
// front of every caller that can only be sent when they are already pointless.
//
// @tier L12 · shared

#include <engine/core/Bytes.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace engine::replication {

	// Which of the four an admission message is.
	//
	// @since v0.3
	enum class AdmissionKind : uint8_t {
		// Client to server: I would like to connect, here is my key exchange
		// message. Costs the server nothing.
		Hello,

		// Server to client: prove you can receive at that address, here is the
		// cookie to send back. Derived rather than remembered.
		Challenge,

		// Client to server: the same key exchange message, and the cookie.
		Answer,

		// Server to client: my key exchange message, and a tag over the
		// transcript proving I derived the same keys you did.
		Welcome,
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
		// The client's `net::Handshake` message — its ephemeral public key.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};
	};

	// A server asking the client to prove where it is.
	//
	// @since v0.3
	struct Challenge {
		// What to send back, unchanged.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A client answering the challenge.
	//
	// @since v0.3
	struct Answer {
		// The same key exchange message as the `Hello`. **The cookie covers
		// it**, so a relay that swapped its own key in would have to have been
		// issued its own cookie, from its own address.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		// The cookie the server issued.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A server admitting the client, and proving the keys agree.
	//
	// @since v0.3
	struct Welcome {
		// The server's `net::Handshake` message — its ephemeral public key.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		// The nonce counter the tag below was sealed under. Zero in practice,
		// since the `Sealer` is used once and destroyed, but carried rather
		// than assumed — an `Opener` is told the counter, never left to guess.
		uint64_t Counter = 0;

		// The tag over an empty frame, with the transcript as associated data.
		//
		// Sixteen bytes that say "I reached the same keys". Without it a
		// substituted or corrupted public key is accepted and the failure moves
		// to whatever first depends on the keys, which is nothing at all today.
		std::array<std::byte, net::Cipher::TAG_BYTES> Confirmation{};
	};

	// What a successful read produced.
	//
	// A tagged union by hand, for the reason `Message` is one: exactly one body
	// is meaningful and `Kind` says which.
	//
	// @since v0.3
	struct Admission {
		// Which body is filled in.
		AdmissionKind Kind = AdmissionKind::Hello;

		// Meaningful when `Kind` is `Hello`.
		replication::Hello Hello;

		// Meaningful when `Kind` is `Challenge`.
		replication::Challenge Challenge;

		// Meaningful when `Kind` is `Answer`.
		replication::Answer Answer;

		// Meaningful when `Kind` is `Welcome`.
		replication::Welcome Welcome;
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

	// Reads an admission message, refusing anything that is not exactly one.
	//
	// **Every field is hostile and this one is read before anything is known
	// about the sender at all**, so it is stricter than `ReadMessage`: an
	// unknown version, a kind outside the enum, a short body *and a body with
	// bytes left over* are each refused. Every one of these messages is a fixed
	// length, so trailing bytes mean the sender is not speaking this protocol
	// and there is nothing to be gained by reading the part that parsed.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success, untouched otherwise.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadAdmission(core::ByteReader &reader, Admission &message);

	// Wraps an admission payload as a datagram on the handshake channel.
	//
	// One function so that both ends frame it identically. The header carries no
	// sequence and no acknowledgement, because neither exists yet: there is no
	// `Link` on the responder's side and the initiator's is still `Connecting`.
	//
	// @param writer  Where the datagram goes.
	// @param payload The encoded admission message.
	// @return `false` when the payload does not fit a packet, which for these
	//         fixed-size messages cannot happen and is checked anyway.
	// @since v0.3
	bool FrameAdmission(core::ByteWriter &writer, std::span<const std::byte> payload);

	// The transcript a `Welcome`'s tag is computed over.
	//
	// The client's key, the server's key and the cookie, in that order. Binding
	// all three means a tag lifted from one exchange verifies in no other, and
	// that the cookie the client answered with is the one the server checked.
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
		// Where it answered from. **Proven** rather than claimed: the cookie is
		// bound to this address, so a peer that could not receive there cannot
		// have got this far.
		net::Endpoint From;

		// How many clients are already connected.
		size_t Connected = 0;

		// The current time, passed in.
		//
		// Here so a policy can rate-limit or expire a ban without reading a
		// clock — `replication/AGENTS.md` bans that inside this module and a
		// hook that had to break the rule to be useful would be a bad hook.
		double NowSeconds = 0.0;
	};

	// Decides who is allowed to connect at all.
	//
	// **The engine has no opinion and must not invent one.** A ban list, an
	// allow list, a session token from a matchmaker and "friends only" are all
	// answers to this, and every one of them is a fact about a *game* — the same
	// argument `Authority::SetInterest` and `Authority::SetPriority` are built
	// on. What this module owes is the seam and the point in the sequence where
	// it is asked.
	//
	// @since v0.3
	using AdmissionPolicy = std::function<bool(const Applicant &)>;
}
