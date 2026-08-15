#pragma once

// Every session this process can see, however it came to see them.
//
// **This is the unified part.** A session heard broadcasting on the subnet, one
// listed by a rendezvous point, and one somebody typed into a config file are
// three rows in one table, differing in a `Reach` and in nothing else. Whatever
// consumes this - a server browser, a content source list, the studio's team
// panel - walks one container and never learns which of the three it is looking
// at unless it wants to show a person.
//
// The alternative is three lists, three orderings and three sets of expiry
// rules, and the first thing anybody would write on top of them is this class.
//
// ## What it is not
//
// **Not a connection, and it opens none.** What comes out of here is an
// `engine::net::Endpoint`, which is what `replication::Connector` takes, what a
// `delivery::Source` names and what an `http::Client` dials. Everything after
// that address is somebody else's module - see `mono.network/CMakeLists.txt`
// for why that seam is where it is.
//
// **Not a trust boundary.** A listing says a datagram claiming these things
// arrived from that address. `Listing::Joinable` is a *filter for a user
// interface* - it hides rows a person cannot use - and never the check that a
// peer is who it says. That check happens after a connection exists, against a
// pinned identity, and one layer up.
//
// ## The table is bounded and the bound is the point
//
// An open UDP port receives whatever is sent to it, so a table that grew with
// what arrived would be a table a stranger fills for the cost of one datagram
// each. `MaximumListings` caps it; past the cap a *new* session is dropped and
// counted, and an existing one still refreshes - which is the right way round,
// because the sessions somebody is already looking at should not vanish
// because somebody else started flooding.
//
// @tier shared

#include <engine/net/Transport.hpp>

#include <cstddef>
#include <cstdint>
#include <network/Advert.hpp>
#include <network/Enums.hpp>
#include <network/SessionKey.hpp>
#include <span>
#include <vector>

namespace network {

	// One session, and what is known about how to get to it.
	//
	// @since v0.13
	struct Listing {
		// What the session said about itself.
		Advert Session;

		// How this was found.
		Reach Via = Reach::Lan;

		// Whether the advert's tag verified against a key this process holds.
		bool Authenticated = false;

		// Where the announcement arrived from.
		//
		// For a `Lan` row this is the host's address on the subnet the datagram
		// crossed, which is the one address known to work between these two
		// machines - see `Dial`. For a row that was offered rather than heard,
		// it is whatever the offerer supplied, and may be invalid.
		engine::net::Endpoint From;

		// When this was last heard from, on the caller's clock.
		double LastSeenSeconds = 0.0;

		// The address to actually connect to.
		//
		// **The source address with the advertised port, not the advertised
		// address**, whenever the source is a real one. A host binds `0.0.0.0`
		// and genuinely does not know which of its addresses this particular
		// client can route to - a machine with a VPN, a container bridge and a
		// wireless interface has three plausible answers and announcing any one
		// of them is a coin flip. The address the datagram *came from* needs no
		// guess: something already travelled over it in the direction that
		// matters.
		//
		// Falls back to the advertised address when there is no source, which
		// is what a row from a config file is.
		//
		// @return Where to dial, or an invalid endpoint when neither is usable.
		engine::net::Endpoint Dial() const;

		// Whether a person could act on this row.
		//
		// @return `false` for a private session with no key, and for one the
		//         host said is full.
		bool Joinable() const {
			return !Session.IsFull() && (Session.Admits == Access::Public || Authenticated);
		}
	};

	// How long a session stays listed, and how many may be.
	//
	// @since v0.13
	struct DirectorySettings {
		// How long since the last announcement before a session is dropped.
		//
		// Several announcement intervals, because broadcast is the traffic a
		// busy switch drops first and a browser whose rows flicker is a browser
		// nobody can click.
		double ForgetAfterSeconds = 5.0;

		// The protocol this process speaks. Announcements carrying any other
		// number are dropped and counted.
		//
		// **Dropped rather than listed as incompatible.** A row a person cannot
		// join and cannot fix is noise, and the case it would help with - "why
		// can I not see my friend" - is answered by the counter, which is where
		// somebody debugging looks.
		uint32_t Protocol = 0;

		// Which sessions to keep. Announcements for anything else are ignored
		// before they are stored, so a client browsing for games never holds a
		// row for the content origin on the same machine.
		Purpose Use = Purpose::Game;

		// The most sessions to hold.
		size_t MaximumListings = 256;
	};

	// What a directory has seen.
	//
	// @since v0.13
	struct DirectoryCounters {
		// Datagrams taken off a transport.
		uint64_t Heard = 0;

		// Sessions listed for the first time.
		uint64_t Listed = 0;

		// Announcements that refreshed a session already listed.
		uint64_t Refreshed = 0;

		// Datagrams that were not an advert at all.
		uint64_t Malformed = 0;

		// Adverts from a build speaking another protocol.
		uint64_t WrongProtocol = 0;

		// Adverts for a purpose this directory does not collect.
		uint64_t WrongPurpose = 0;

		// Private sessions listed without a key to verify them.
		//
		// **Listed, not refused** - see the file header. The counter exists
		// because "I can see it but cannot join it" and "I cannot see it" are
		// different problems and a person will report both as the same
		// sentence.
		uint64_t Locked = 0;

		// New sessions dropped because the table was full.
		uint64_t Overflowed = 0;

		// Sessions dropped for going quiet.
		uint64_t Forgotten = 0;
	};

	// The table of sessions, and the expiry that keeps it honest.
	//
	// @since v0.13
	class Directory {
	  public:
		// @param settings What to collect, and how much of it.
		explicit Directory(const DirectorySettings &settings = {});

		// Adds a key this process holds, for verifying private adverts.
		//
		// Several may be held: somebody in two private sessions is in two
		// private sessions, and a browser that could only check one of them
		// would show the other as locked.
		//
		// @param key The key. Moved in.
		void Trust(SessionKey key);

		// How many keys are held.
		//
		// @return The count.
		size_t Trusted() const {
			return Keys.size();
		}

		// Drains a transport and lists what it heard.
		//
		// Polls until the transport is empty, which is how every transport in
		// this repository is read. Nothing here blocks and nothing sends.
		//
		// @param transport  A discovery socket, bound to the announcement port.
		// @param nowSeconds The current time.
		// @return How many datagrams became a listing, new or refreshed.
		size_t Observe(engine::net::Transport &transport, double nowSeconds);

		// Lists a session that was not heard on the subnet.
		//
		// The way a rendezvous point's reply and a config file's row enter the
		// same table as a broadcast. The advert is taken as given - a caller
		// that decoded it from a wire has already checked it, and a caller that
		// built it from a config file is the local operator.
		//
		// @param advert     What the session is.
		// @param via        How it was found.
		// @param from       Where it was heard from, or an invalid endpoint.
		// @param nowSeconds The current time.
		// @param authenticated Whether the caller verified it against a key.
		// @return Whether it was listed. `false` when the table was full or the
		//         advert was not well formed.
		bool Offer(
			const Advert &advert,
			Reach via,
			const engine::net::Endpoint &from,
			double nowSeconds,
			bool authenticated = false
		);

		// Drops every session that has gone quiet.
		//
		// Called by whoever calls `Observe`, at the same barrier. Separate from
		// it because a directory fed only by `Offer` still has to expire, and a
		// caller with no transport to drain would otherwise have nowhere to say
		// so.
		//
		// @param nowSeconds The current time.
		// @return How many were dropped.
		size_t Forget(double nowSeconds);

		// Every session, in the order they were first listed.
		//
		// **First-listed order rather than sorted**, so a browser's rows do not
		// jump under the pointer when a player count changes. A caller wanting
		// them by name or by ping sorts a copy, which is a decision about a
		// user interface and does not belong here.
		//
		// @return A view valid until the next call that changes the table.
		std::span<const Listing> Listings() const {
			return Rows;
		}

		// One session by id.
		//
		// @param session The id.
		// @return The listing, or null.
		const Listing *Find(const SessionId &session) const;

		// Removes one session.
		//
		// For the caller that has just failed to connect to it: a row that is
		// still being announced comes back on the next announcement, which is
		// correct, and a row for a host that has gone stays gone rather than
		// waiting out the expiry.
		//
		// @param session The id.
		// @return Whether anything was removed.
		bool Drop(const SessionId &session);

		// Empties the table, keeping the keys and the settings.
		void Clear();

		// What this directory has seen.
		//
		// @return The counters.
		const DirectoryCounters &Counters() const {
			return Tally;
		}

	  private:
		// Files one decoded advert into the table.
		bool Admit(
			const Advert &advert,
			Reach via,
			const engine::net::Endpoint &from,
			double nowSeconds,
			bool authenticated
		);

		DirectorySettings Limits;
		std::vector<SessionKey> Keys;
		std::vector<Listing> Rows;
		DirectoryCounters Tally;

		// Reused across polls so a directory drained every tick stops
		// allocating after the first datagram that reached its high-water mark.
		std::vector<std::byte> Scratch;
	};
}
