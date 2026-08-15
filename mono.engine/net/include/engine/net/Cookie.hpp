#pragma once

// The challenge a stranger answers before this end allocates anything for it.
//
// A server on an open port receives whatever is sent to it, from whatever
// address the sender cared to write in the header. Two things follow. A peer
// that has not answered anything has not proved it can *receive* at the address
// it claims, so believing it lets one machine open a connection in somebody
// else's name. And anything remembered per unanswered attempt is a table an
// attacker fills for the cost of one datagram each.
//
// **So nothing is remembered.** The cookie is derived from a secret this end
// already holds plus the bytes the peer already sent, and it is verified by
// deriving it again - never by looking it up. An unanswered challenge costs
// **zero bytes** of state: this object is two 32-byte secrets and a deadline
// whether one peer is mid-handshake or a hundred thousand are. That is the
// whole reason it exists, and it is the mistake it exists to prevent - a server
// that stored a pending challenge per source address would have moved the
// exhaustion target rather than removed it. DTLS's `HelloVerifyRequest`
// (RFC 6347 §4.2.1) is the same construction for the same reason.
//
// **What a returned cookie proves, exactly:** somebody at that address received
// a datagram this end sent there, recently. That is return routability and it
// is all of it. It is not identity, it is not authorisation, and a peer on the
// path can read a cookie off the wire and use it. Who is allowed to connect is
// a decision one layer up - see `replication::Listener::SetAdmission`.
//
// **The answer is non-amplifying.** A challenge is the same size as the hello
// that asked for it, so this cannot be pointed at a third party as a reflector.
// An answer that were larger than its question is what turns a stateless
// responder into somebody else's problem.
//
// **Time is passed in, never read** - the module rule. The secret rotates on a
// deadline the caller states, which is what gives a cookie a lifetime without a
// clock in here and makes that lifetime something a suite states rather than
// waits for.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::net {

	// How long a cookie stays good for.
	//
	// @since v0.3
	struct CookieSettings {
		// How often the secret behind the cookies is replaced.
		//
		// A cookie is accepted under the current secret and the one before it,
		// so the real lifetime is between this and twice it. Long enough that a
		// peer on a slow link can answer, short enough that a cookie lifted off
		// the wire stops working while the address it was issued to is still
		// the address that has to use it.
		double RotateEverySeconds = 10.0;
	};

	// A challenge a peer answers, held as a secret rather than as a table.
	//
	// One per listener. Move-only, like every other holder of key material
	// here: two copies of the secret is one more place for it to be read out of
	// than there needs to be.
	//
	// @since v0.3
	class Cookie {
	  public:
		// How many bytes a cookie is. HMAC-SHA256's output, untruncated.
		static constexpr size_t COOKIE_BYTES = 32;

		// Starts issuing cookies under a secret from the operating system.
		//
		// @param settings How often to rotate the secret.
		// @return The issuer, or nothing if the operating system refused to
		//         provide entropy - which is a refusal to admit anybody, never
		//         a fallback to a weaker source. A guessable secret here is a
		//         cookie anybody can forge, which is the whole protection gone
		//         while every counter still reads as healthy.
		static std::optional<Cookie> Begin(const CookieSettings &settings = {});

		// Zeroes both secrets.
		~Cookie();

		Cookie(const Cookie &) = delete;
		Cookie &operator=(const Cookie &) = delete;

		// Moves the secrets, leaving the source zeroed.
		Cookie(Cookie &&other) noexcept;

		// Moves the secrets, zeroing both this issuer's and the source's.
		Cookie &operator=(Cookie &&other) noexcept;

		// Derives the cookie for one peer, remembering nothing.
		//
		// @param nowSeconds The current time, which is what rotates the secret.
		// @param peer       Where the answer will have to come from. In the
		//                   cookie, so an answer from anywhere else fails.
		// @param evidence   Whatever else the answer has to repeat unchanged -
		//                   the peer's key exchange message, so a relay cannot
		//                   swap its own in and keep the cookie.
		// @return The cookie to send back.
		std::array<std::byte, COOKIE_BYTES>
		Issue(double nowSeconds, const Endpoint &peer, std::span<const std::byte> evidence);

		// Whether a cookie is one this end issued to this peer for this
		// evidence, recently.
		//
		// Compared in constant time. A comparison that stopped at the first
		// wrong byte would let the cookie be recovered a byte at a time by
		// timing the refusals, which is a forgery with more steps.
		//
		// @param nowSeconds The current time.
		// @param peer       Where the answer came from.
		// @param evidence   What the answer repeated.
		// @param cookie     The cookie the peer sent back.
		// @return `true` when it verifies under the current secret or the one
		//         before it. A cookie older than that is refused, which is what
		//         stops one captured off the wire being useful for ever.
		bool Answers(
			double nowSeconds,
			const Endpoint &peer,
			std::span<const std::byte> evidence,
			std::span<const std::byte> cookie
		);

	  private:
		// The MAC key length. SHA-256's block is larger, but a key the size of
		// the output is the whole of the security this construction has.
		static constexpr size_t SECRET_BYTES = 32;

		Cookie() = default;

		// Replaces the secret when the deadline has passed, keeping the old one
		// as the second acceptable answer.
		void Rotate(double nowSeconds);

		// HMAC-SHA256 over the peer's address and the evidence, under `secret`.
		std::array<std::byte, COOKIE_BYTES> Derive(
			const std::array<uint8_t, SECRET_BYTES> &secret,
			const Endpoint &peer,
			std::span<const std::byte> evidence
		) const;

		// Zeroes both secrets.
		void Forget();

		std::array<uint8_t, SECRET_BYTES> Current{};
		std::array<uint8_t, SECRET_BYTES> Previous{};

		double RotatePeriod = 10.0;
		double RotateAt = 0.0;

		// Whether a time has ever been seen. The first call sets the deadline
		// rather than rotating against a zero it never agreed to - otherwise a
		// server started at any wall clock past its period would burn both
		// secrets before the first peer had answered.
		bool Timed = false;
	};
}
