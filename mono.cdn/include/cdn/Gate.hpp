#pragma once

// The only decision the origin makes about who may have what.
//
// It is a small decision on purpose. **The server decides; the origin checks a
// token and serves** - CDN.md §4. Everything about *who* is the server's: the
// session, the player, what they have loaded, what they are entitled to. This
// class knows none of it and must never learn it.
//
// So the surface is one question. Given a token a client presented and a bundle
// it is asking for, may that bundle be served right now? Nothing here resolves
// a session to a person, and there is nowhere for it to look one up.
//
// An origin that grows an account table is a second authority, and two
// authorities that can disagree eventually do - usually under load, which is
// when it is hardest to see.
//
// @tier shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Grant.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cdn {

	// Admits or refuses a request against the grant it carries.
	//
	// Holds the secret shared with the server and nothing else. One per origin,
	// built at start-up.
	class Gate {
	  public:
		// @param key The secret shared with the server that issues grants.
		explicit Gate(engine::assets::GrantKey key);

		// Whether `token` permits `bundleRoot` at `nowSeconds`.
		//
		// Three checks, in this order and no other: the MAC, the expiry, then
		// the scope. The MAC is first because nothing in a token means anything
		// until it has been verified - acting on an unverified field, even to
		// reject it, is how a parser becomes the attack surface the MAC was
		// meant to remove.
		//
		// A refusal says nothing about which check failed. The counters
		// distinguish them for an operator; the caller gets a boolean, because a
		// reason returned to a client is an oracle.
		//
		// @param token The bytes the client presented.
		// @param bundleRoot The bundle being requested, by content hash. There
		//        is no overload taking a path, and there must not be - a hash
		//        cannot be walked.
		// @param nowSeconds The current time, on the clock shared with the
		//        server. Passed in rather than read here, so the origin holds no
		//        notion of "now" of its own to drift.
		// @return Whether to serve.
		bool Admits(
			std::span<const std::byte> token,
			const engine::assets::ContentHash &bundleRoot,
			uint64_t nowSeconds
		) const;

	  private:
		engine::assets::GrantKey Shared;
	};
}
