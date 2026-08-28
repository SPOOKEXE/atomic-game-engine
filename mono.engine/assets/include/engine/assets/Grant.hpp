#pragma once

// Who is allowed to fetch what, and who decides.
//
// **The server decides. The origin checks a token and serves.** Two jobs, two
// programs. The server is the only thing that knows the session, the
// player, what they have loaded and what they are entitled to; the origin knows
// none of it and must not learn it. An origin with a player database is a second
// authority, and two authorities that can disagree eventually do.
//
// A grant is a scope plus a MAC over it, under a key the server and the origin
// share and the client never sees. The client carries the token and cannot alter
// it.
//
// Three choices in here, and each is the reason the design works:
//
// - **A MAC rather than a signature.** The origin verifies one per request, and
//   HMAC-SHA256 is cheap. Ed25519 verification per request would put an
//   asymmetric operation on the hot path to buy nothing - the server and the
//   origin are one trust domain. The asymmetric signature is on the *manifest*,
//   where the trust boundary actually is. Signature.hpp.
// - **A grant names content hashes, never paths.** A path has to be re-checked
//   against traversal rules at the request layer, and every such check is
//   somewhere to get it wrong. A hash is self-limiting: there is nothing to
//   walk, and naming content whose hash you were not given means finding a
//   collision.
// - **Expiry is tuned to metering, not secrecy.** Game content is not secret; it
//   ships to everyone who plays. A leaked grant is bandwidth theft, so the
//   window is set by what an operator will accept being billed for. Saying so
//   plainly stops somebody later treating a grant as a capability that protects
//   data, which it does not.
//
// This is the *format*. Deciding what a client needs is the server's, and how
// the bytes travel is `net`'s.
//
// @tier L8 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::assets {

	// The secret a server and its origin share.
	//
	// Never sent to a client. Wipes itself on destruction, for the reason
	// SigningKey does - the floor rather than the ceiling, and a deployment that
	// cares about swap files and core dumps has more to do.
	class GrantKey {
	  public:
		// Key length. 32 bytes, matching HMAC-SHA256's block-derived strength;
		// a longer key buys nothing and a shorter one is a weaker MAC.
		static constexpr size_t BYTES = 32;

		// Builds a key from shared secret material.
		//
		// Where the secret comes from is the deployment's problem.
		// `core::Random` is not a cryptographic generator and must not be used.
		//
		// @param secret Exactly BYTES of shared, high-entropy material.
		// @return The key, or nothing if the secret is the wrong length.
		static std::optional<GrantKey> FromSecret(std::span<const std::byte> secret);

		// Zeroes the secret.
		~GrantKey();

		GrantKey(const GrantKey &) = delete;
		GrantKey &operator=(const GrantKey &) = delete;

		// Moves the key, leaving the source zeroed.
		GrantKey(GrantKey &&other) noexcept;

		// Moves the key, zeroing both the destination's old secret and the
		// source's.
		GrantKey &operator=(GrantKey &&other) noexcept;

	  private:
		friend class Grant;

		GrantKey() = default;

		std::array<uint8_t, BYTES> Secret{};
	};

	// What a grant permits. Everything the origin is allowed to know.
	//
	// Note what is absent: no player, no account, no address. The origin serves
	// against this and learns nothing about who asked.
	struct GrantScope {
		// Which session this belongs to. An opaque number the server allocates;
		// the origin only ever compares it, never resolves it to anything.
		uint64_t Session = 0;

		// **What may be fetched**, by bundle root. Held sorted, so one set of
		// bundles has one encoding and therefore one MAC.
		std::vector<ContentHash> Bundles;

		// When this stops being honoured, as absolute seconds on whatever clock
		// the server and origin agree on.
		//
		// Absolute rather than a duration, for the reason the server's pacing is
		// absolute: a relative window has to be resolved against a clock
		// somewhere, and the two ends would resolve it against different ones.
		uint64_t ExpiresAtSeconds = 0;

		// How many bytes may be drawn against this grant. Enforcement is the
		// origin's; the number is the server's.
		uint64_t ByteBudget = 0;

		// Whether this scope can be issued: a session, at least one bundle, an
		// expiry and a budget. An empty bundle list is refused rather than
		// treated as "everything", because a grant that permits nothing and a
		// grant that permits all of it must never be one value.
		bool IsValid() const;
	};

	// A scope and the MAC that makes it unforgeable.
	//
	// Issued by a server, carried by a client, opened by an origin. The client
	// can read it - none of it is secret - and cannot change a byte of it.
	class Grant {
	  public:
		// The token's magic, so a wrong blob fails at its first four bytes.
		static constexpr uint32_t MAGIC = 0x31475441; // "ATG1"

		// The token version. Refused when unknown, for the reason a manifest's
		// is: a reader that guesses at a version mis-parses hostile bytes.
		static constexpr uint16_t VERSION = 1;

		// MAC length. The full HMAC-SHA256 output, untruncated - truncation
		// saves 16 bytes on a token that is already dwarfed by its bundle list.
		static constexpr size_t MAC_BYTES = 32;

		// Issues a grant over `scope`.
		//
		// Sorts the bundle list, so the caller's ordering cannot change the
		// token.
		//
		// @param scope What to permit.
		// @param key The secret shared with the origin.
		// @return The grant, or nothing if the scope is not valid.
		static std::optional<Grant> Issue(GrantScope scope, const GrantKey &key);

		// The token bytes a client carries.
		std::vector<std::byte> Encode() const;

		// Opens a token: checks the MAC, then the expiry.
		//
		// **The MAC is checked before anything in the scope is believed**, and
		// in constant time. Reading a field out of an unverified token and
		// acting on it - even to reject it - is how a parser becomes the attack
		// surface the MAC was supposed to remove.
		//
		// @param token The bytes a client presented.
		// @param key The secret shared with the server.
		// @param nowSeconds The current time, on the clock the two ends share.
		//        Passed in rather than read from a clock here, so that expiry is
		//        testable and so this module holds no notion of "now".
		// @return The grant, or nothing. Nothing means serve nothing.
		static std::optional<Grant>
		Open(std::span<const std::byte> token, const GrantKey &key, uint64_t nowSeconds);

		// What this grant permits.
		const GrantScope &Scope() const {
			return Permitted;
		}

		// Whether this grant covers a bundle.
		//
		// The one question the request path asks. A binary search over the
		// sorted list, so a grant naming a thousand bundles costs ten compares
		// rather than a thousand.
		//
		// @param bundleRoot The bundle being requested.
		// @return Whether it is named by this grant.
		bool Permits(const ContentHash &bundleRoot) const;

		// Whether this grant has expired at `nowSeconds`.
		//
		// Asked again after Open because a long-lived connection outlives the
		// check that admitted it - a stream that started inside the window must
		// not run indefinitely outside it.
		//
		// @param nowSeconds The current time.
		// @return Whether the grant is no longer honoured.
		bool HasExpired(uint64_t nowSeconds) const;

	  private:
		Grant() = default;

		GrantScope Permitted;
		std::array<uint8_t, MAC_BYTES> Mac{};
	};
}
