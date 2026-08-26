#include <engine/core/Log.hpp>

#include <algorithm>
#include <network/Directory.hpp>
#include <utility>

namespace network {

	engine::net::Endpoint Listing::Dial() const {
		// The source address with the advertised port. The header carries the
		// argument: a host binds the wildcard address and cannot know which of
		// its addresses this particular client can route to, and the address
		// the datagram arrived from is the one that demonstrably works.
		if (From.IsValid() && Session.At.Port != 0) {
			engine::net::Endpoint dial = From;
			dial.Port = Session.At.Port;
			return dial;
		}
		if (Session.At.IsValid()) {
			return Session.At;
		}
		// A row offered with neither is one nothing can act on. Returned as an
		// invalid endpoint rather than as the source's ephemeral port, which
		// would be an address that looks usable and is not.
		return {};
	}

	Directory::Directory(const DirectorySettings &settings) : Limits(settings) {
		if (Limits.ForgetAfterSeconds <= 0.0) {
			Limits.ForgetAfterSeconds = DirectorySettings{}.ForgetAfterSeconds;
		}
	}

	void Directory::Trust(SessionKey key) {
		Keys.push_back(std::move(key));
	}

	size_t Directory::Observe(engine::net::Transport &transport, double nowSeconds) {
		size_t listed = 0;

		while (true) {
			const engine::net::Transport::Inbound inbound = transport.Receive(Scratch);
			if (inbound.Status != engine::net::TransportStatus::Ok) {
				break;
			}
			Tally.Heard++;

			const std::optional<DecodedAdvert> decoded = Decode(Scratch, Keys);
			if (!decoded) {
				Tally.Malformed++;
				continue;
			}
			if (decoded->Session.Protocol != Limits.Protocol) {
				// **The single most common "I cannot see the server on my own
				// network".** Two builds on one subnet hear each other perfectly
				// and list nothing, and until this line the only evidence was a
				// counter nobody printed.
				ENGINE_DEBUG_EVERY(
					2.0,
					"ignoring a session on protocol {}; this build speaks {}",
					decoded->Session.Protocol,
					Limits.Protocol
				);
				Tally.WrongProtocol++;
				continue;
			}
			if (decoded->Session.Use != Limits.Use) {
				ENGINE_TRACE_EVERY(2.0, "ignoring a session announced for another purpose");
				Tally.WrongPurpose++;
				continue;
			}
			if (decoded->Session.Admits == Access::Private && !decoded->Authenticated) {
				// Listed anyway. "I can see it but cannot join it" and "I cannot
				// see it" are different problems, and a person reports both with
				// the same sentence - so the row exists and the counter says
				// which it was.
				Tally.Locked++;
			}

			if (Admit(decoded->Session, Reach::Lan, inbound.From, nowSeconds, decoded->Authenticated)) {
				listed++;
			}
		}

		return listed;
	}

	bool Directory::Offer(
		const Advert &advert,
		Reach via,
		const engine::net::Endpoint &from,
		double nowSeconds,
		bool authenticated
	) {
		return Admit(advert, via, from, nowSeconds, authenticated);
	}

	bool Directory::Admit(
		const Advert &advert,
		Reach via,
		const engine::net::Endpoint &from,
		double nowSeconds,
		bool authenticated
	) {
		if (!advert.IsValid()) {
			Tally.Malformed++;
			return false;
		}

		for (Listing &row : Rows) {
			if (row.Session.Session != advert.Session) {
				continue;
			}
			// One session, one row, whichever way it arrived twice. A host on
			// the subnet that is also registered at a rendezvous point is one
			// session and a browser showing it twice is a browser showing a
			// person a choice that is not one.
			//
			// The nearer reach wins, because `Reach`'s order is how much has to
			// keep working: a LAN address survives a router forgetting a
			// mapping and a punched one does not.
			if (via <= row.Via) {
				row.Via = via;
				row.From = from;
			}
			row.Session = advert;
			row.Authenticated = authenticated;
			row.LastSeenSeconds = nowSeconds;
			Tally.Refreshed++;
			return true;
		}

		if (Rows.size() >= Limits.MaximumListings) {
			// The cap drops the new one rather than the oldest. A table that
			// evicted to make room would let a flood push out every session
			// somebody is looking at, which is the outcome the cap exists to
			// prevent rather than a gentler version of it.
			// The browser stops showing anything new and looks like it stopped
			// listening, which is exactly what the cap is not.
			ENGINE_WARN_EVERY(
				5.0, "the directory is full at {} listings; new sessions are not being listed", Rows.size()
			);
			Tally.Overflowed++;
			return false;
		}

		Listing row;
		row.Session = advert;
		row.Via = via;
		row.Authenticated = authenticated;
		row.From = from;
		row.LastSeenSeconds = nowSeconds;
		ENGINE_DEBUG(
			"listed a session at {} by reach {}, {}",
			from.Text(),
			static_cast<int>(via),
			authenticated ? "authenticated" : "unauthenticated"
		);
		Rows.push_back(std::move(row));
		Tally.Listed++;
		return true;
	}

	size_t Directory::Forget(double nowSeconds) {
		const size_t before = Rows.size();
		const double oldest = nowSeconds - Limits.ForgetAfterSeconds;

		Rows.erase(
			std::remove_if(
				Rows.begin(),
				Rows.end(),
				[oldest](const Listing &row) { return row.LastSeenSeconds < oldest; }
			),
			Rows.end()
		);

		const size_t dropped = before - Rows.size();
		Tally.Forgotten += dropped;
		return dropped;
	}

	const Listing *Directory::Find(const SessionId &session) const {
		for (const Listing &row : Rows) {
			if (row.Session.Session == session) {
				return &row;
			}
		}
		return nullptr;
	}

	bool Directory::Drop(const SessionId &session) {
		for (size_t index = 0; index < Rows.size(); ++index) {
			if (Rows[index].Session.Session == session) {
				Rows.erase(Rows.begin() + static_cast<ptrdiff_t>(index));
				return true;
			}
		}
		return false;
	}

	void Directory::Clear() {
		Rows.clear();
	}
}
