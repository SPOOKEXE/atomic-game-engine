#pragma once

// A host saying where it is, on an interval, to whoever is listening.
//
// The whole of LAN discovery from the host's side, and it is one direction:
// this sends and never receives. That is a decision rather than an omission,
// and the alternative is worth writing down because it is the obvious one.
//
// ## Why announcements and not probes
//
// The other arrangement is a browser broadcasting "who is there" and hosts
// answering. It lists a session the instant a browser opens instead of within
// one announcement interval, which is the only thing it is better at, and it
// costs the property that matters more: **every host would have to bind the
// well-known port**, so a machine could only ever host one session. A person
// running two servers to test against, or a studio hosting team create beside a
// client, is ordinary here. So the well-known port is the *listener's*, hosts
// announce from an ephemeral one, and a machine can host as many sessions as it
// likes.
//
// It also means a host answers nothing it was not going to say anyway, which
// removes an amplifier: there is no datagram a stranger can send this that
// produces a reply.
//
// ## What it refuses to do
//
// **A `Private` advert with no key is never sent.** An untagged private advert
// is a public one wearing a label — anybody could forge it and no browser could
// tell — so the beacon announces nothing and says so through
// `BeaconCounters::Refused`. A host that has misconfigured its key finds out by
// being invisible, which is the loud failure; the quiet one would be a private
// session anybody can impersonate.
//
// **Time is passed in, never read.** `engine::net`'s rule, and it earns the
// same keep here: an announcement interval is something a suite states rather
// than waits for, so the whole discovery suite runs in microseconds.
//
// @tier shared

#include <engine/net/Transport.hpp>

#include <cstdint>
#include <network/Advert.hpp>
#include <network/SessionKey.hpp>
#include <optional>

namespace network {

	// How often a host says the same thing again.
	//
	// @since v0.13
	struct BeaconSettings {
		// The port to announce to. `DISCOVERY_PORT` unless two unrelated
		// sessions are sharing a subnet on purpose.
		uint16_t Port = DISCOVERY_PORT;

		// How often to announce.
		//
		// Shorter than `DirectorySettings::ForgetAfterSeconds` by a wide margin
		// on purpose: at a whisker under it, one lost broadcast drops a healthy
		// session out of every browser on the subnet, and UDP broadcast is the
		// traffic a busy switch drops first.
		double AnnounceEverySeconds = 1.0;
	};

	// What a beacon has done.
	//
	// @since v0.13
	struct BeaconCounters {
		// Announcements put on the wire.
		uint64_t Announcements = 0;

		// What they weighed.
		uint64_t Bytes = 0;

		// Announcements this beacon declined to make.
		//
		// **Counted apart from a failed send**, because they are different
		// problems: this one is a misconfiguration here — a private session
		// with no key, or an advert that is not well formed — and the other is
		// the network.
		uint64_t Refused = 0;

		// Sends the transport would not take.
		uint64_t Undelivered = 0;
	};

	// Announces one advert on one transport.
	//
	// Borrows the transport rather than owning it: a host that also browses
	// needs two sockets and a host that only announces needs one, and deciding
	// which is `Presence`'s job. The transport must have been opened with
	// `TransportSettings::Broadcast`, or every send is refused as unreachable
	// and shows up in `Undelivered`.
	//
	// @since v0.13
	class Beacon {
	  public:
		// Starts announcing.
		//
		// @param transport The wire. Borrowed, not owned.
		// @param advert    What to say. Copied.
		// @param key       The session key, for a `Private` advert. Moved in.
		// @param settings  How often, and to which port.
		Beacon(
			engine::net::Transport &transport,
			const Advert &advert,
			std::optional<SessionKey> key = std::nullopt,
			const BeaconSettings &settings = {}
		);

		// Whether this beacon will send anything at all.
		//
		// @return `false` for a malformed advert, or a `Private` one with no
		//         key.
		bool Announcing() const;

		// What is being announced.
		//
		// @return The advert.
		const Advert &Advertised() const {
			return Saying;
		}

		// Replaces what is announced, keeping the schedule.
		//
		// The player count and the place being played change while a session
		// runs, and re-announcing on the spot for each would let a host that
		// changes every tick broadcast every tick.
		//
		// @param advert The new record.
		void SetAdvert(const Advert &advert);

		// Announces if it is time to.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Announces now, whatever the schedule says, and resets it.
		//
		// For the two moments an interval is too slow: the first announcement
		// after a host comes up, and the last one before it goes away.
		//
		// @param nowSeconds The current time.
		// @return Whether anything went out.
		bool Announce(double nowSeconds);

		// What this beacon has done.
		//
		// @return The counters.
		const BeaconCounters &Counters() const {
			return Tally;
		}

	  private:
		engine::net::Transport &Wire;
		Advert Saying;
		std::optional<SessionKey> Key;
		BeaconSettings Limits;

		// When the next announcement is due, and whether one has ever been
		// scheduled. A `double` cannot carry "never" without a caller agreeing
		// that some value means it, and zero is a legitimate time.
		double DueAt = 0.0;
		bool Scheduled = false;

		BeaconCounters Tally;
	};
}
