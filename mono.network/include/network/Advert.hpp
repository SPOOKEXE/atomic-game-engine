#pragma once

// What a session says about itself, and the one format it says it in.
//
// One record and one encoding, whether it travels as a LAN broadcast, as a
// registration at a rendezvous point, or as a row somebody typed into a config
// file. That is the whole of "unified": a browser holds `Listing`s and none of
// them knows which of the three it came from except by its `Reach`.
//
// **Every byte of a decoded advert is hostile.** It arrived on an open UDP port
// from an address anybody can write, so the strings are length-capped, the
// enums are range-checked, and a datagram that disagrees with itself is refused
// whole rather than half-read into a partly filled record. An advert is a
// *hint*: it says where to try, and nothing that arrives here is trusted to say
// who is there. `replication::ConnectorSettings::ServerIdentity` is what
// answers that, one layer up and after a connection exists.
//
// **A tag is optional and its absence is not a failure.** A public session has
// nothing to prove — its content is served to everyone who asks — so it
// announces unsigned, and a signature on it would be a signature nobody could
// check. A private session tags its advert under the `SessionKey`, and
// `Decode` reports whether the tag verified against a key the caller holds
// rather than refusing what it cannot check: a browser has to be able to *show*
// a private session it has no key for, or the person who was about to be given
// the key never sees it exists.
//
// @tier shared

#include <engine/net/Endpoint.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <network/Enums.hpp>
#include <network/SessionKey.hpp>
// For std::hash. Specialising it without the primary template in scope is a
// wall of errors pointing at the standard library rather than at this file.
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace network {

	// A session's identity, stable across every way of reaching it.
	//
	// **A random number rather than an address**, and that is the point. A
	// session heard on the subnet and the same session listed at a rendezvous
	// point are one row in a browser because they carry one of these; keyed by
	// address they would be two, and a host that moved between the two
	// announcements would be three.
	//
	// Sixteen bytes from the operating system's entropy. Not a counter and not
	// derived from the machine: a counter collides the moment two hosts start
	// at once, and anything derived from the machine identifies it.
	//
	// @since v0.13
	struct SessionId {
		// How many bytes an id is.
		static constexpr size_t BYTES = 16;

		// The bytes. All-zero is the null id and is never drawn.
		std::array<std::byte, BYTES> Value{};

		// Whether this names a session.
		//
		// @return `true` unless every byte is zero.
		bool IsValid() const;

		// Whether two ids name the same session.
		bool operator==(const SessionId &other) const = default;

		// Ordering, so an id can key a sorted container without a caller
		// inventing a comparator that disagrees with somebody else's.
		auto operator<=>(const SessionId &other) const = default;

		// The id as 32 lowercase hexadecimal characters.
		//
		// @return The text, or `none` for the null id.
		std::string Text() const;

		// Reads what Text wrote.
		//
		// @param text Exactly 32 hexadecimal characters, either case.
		// @return The id, or nothing when the text is not one.
		static std::optional<SessionId> Parse(std::string_view text);

		// Draws a fresh id from the operating system's entropy.
		//
		// @return The id, or the null one if the operating system refused. A
		//         null id fails `IsValid`, so a session that could not be
		//         named is one that cannot be announced rather than one that
		//         announces as everybody.
		static SessionId Draw();
	};

	// What one session tells the world about itself.
	//
	// @since v0.13
	struct Advert {
		// The longest a `Name` or a `Detail` may be, in bytes.
		//
		// Capped because this arrives in a datagram from a stranger and every
		// listing is held in memory until it expires. Sixty-four bytes is a
		// server name somebody typed, not a description.
		static constexpr size_t MAXIMUM_TEXT_BYTES = 64;

		// Who this is, across every reach.
		SessionId Session;

		// What the session is for. A browser lists one purpose and ignores the
		// rest.
		Purpose Use = Purpose::Game;

		// Whether a `SessionKey` is needed to join.
		Access Admits = Access::Public;

		// The caller's own protocol version.
		//
		// **Not this module's format version**, which is in the frame and is
		// checked before any of this is read. This is the number the *program*
		// changes when its wire changes: a client and a server from different
		// builds must not appear joinable to each other, and finding that out
		// during the handshake instead of in the browser costs a person a
		// connection attempt and a confusing error.
		uint32_t Protocol = 0;

		// Where to connect.
		//
		// **May legitimately name the wildcard address**, and usually does on a
		// LAN announcement: a host binds `0.0.0.0` and genuinely does not know
		// which of its addresses a given client can route to. `Listing::Dial`
		// is what resolves that, using the address the announcement arrived
		// from — which is the one address that is known to work, because a
		// datagram came over it.
		engine::net::Endpoint At;

		// What to show a person. Capped at MAXIMUM_TEXT_BYTES.
		std::string Name;

		// What to show a person underneath the name — the place being played,
		// the project being edited, the store being served. Capped the same.
		std::string Detail;

		// How many peers are in. Advisory: it is whatever the host last said.
		uint16_t Peers = 0;

		// How many peers the host will take, or zero for no stated limit.
		uint16_t PeerLimit = 0;

		// Whether this can be announced: a real id, and text within the cap.
		//
		// Says nothing about whether `At` is reachable. Nothing at this layer
		// can answer that without sending something and waiting.
		//
		// @return Whether it is well formed.
		bool IsValid() const;

		// Whether the host said it is full.
		//
		// @return `true` when a limit was stated and has been reached.
		bool IsFull() const {
			return PeerLimit != 0 && Peers >= PeerLimit;
		}
	};

	// A decoded advert, and whether its tag was one we could check.
	//
	// @since v0.13
	struct DecodedAdvert {
		// The record.
		Advert Session;

		// Whether a tag was present and verified against a key the caller
		// holds.
		//
		// **False covers three different situations and deliberately does not
		// distinguish them**: no tag, a tag under a key we do not hold, and a
		// tag that failed. A caller's decision is the same in all three — a
		// `Private` session that is not authenticated is one to show and refuse
		// to dial — and a field that told a stranger which of the three it was
		// would answer "is this the right key" one guess at a time.
		bool Authenticated = false;
	};

	// Encodes an advert into a datagram.
	//
	// @param advert The record. Text past MAXIMUM_TEXT_BYTES is truncated
	//        rather than refused, because a name is cosmetic and dropping a
	//        whole announcement over one is worse than a shortened name.
	// @param key    The session key to tag under, or null for an untagged
	//        advert. Tagging a `Public` advert is allowed and pointless; not
	//        tagging a `Private` one produces an advert no browser will treat
	//        as authenticated, which is why `Beacon` refuses to send one.
	// @return The bytes.
	// @since v0.13
	std::vector<std::byte> Encode(const Advert &advert, const SessionKey *key);

	// Decodes a datagram into an advert.
	//
	// @param datagram The bytes, from anywhere and trusted for nothing.
	// @param keys     Every key the caller holds. Tried in order; the first
	//        that verifies wins, and none verifying is a decoded advert that is
	//        not authenticated rather than a refusal.
	// @return The advert, or nothing when the bytes are not one — a wrong
	//         magic, an unknown version, a length that contradicts the frame,
	//         an enum outside its list, or text past the cap.
	// @since v0.13
	std::optional<DecodedAdvert>
	Decode(std::span<const std::byte> datagram, std::span<const SessionKey> keys);
}

namespace std {

	// Hashing, so a SessionId keys an unordered container.
	template <> struct hash<network::SessionId> {
		// @param id The id to hash.
		// @return Its hash.
		size_t operator()(const network::SessionId &id) const noexcept;
	};
}
