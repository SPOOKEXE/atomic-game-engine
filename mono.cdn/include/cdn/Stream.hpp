#pragma once

// A distribution stream: this origin, the reach it is offered over, and who may
// draw from it.
//
// **A stream is not a second kind of origin.** `cdn::Service` serves the same
// six routes to everybody; what a stream adds is *how a client finds it* and
// *whether it was invited*. The four an operator can build out of two settings
// are the four ROADMAP.md v0.13 names:
//
// | Stream | Announce | Rendezvous | Access |
// |---|---|---|---|
// | LAN | yes | - | public |
// | Peer-to-peer | - | yes | public |
// | Private network | either | either | private, key required |
// | Public distribution | - | yes | public, on an address people are given |
//
// That table is the whole feature, and it is a table rather than four code
// paths because the origin cannot tell the difference. A stream that changed
// what was served would be a second answer to what content is, and CDN.md
// refuses that twice.
//
// ## Why the origin can host the meeting place
//
// `network::RendezvousPoint` is not a program of its own - it holds an id, an
// address and a timestamp, and it needs to be somewhere already reachable. An
// origin is exactly that: a machine an operator has already put on an address
// and already opened a port on. So `--rendezvous-listen` runs one here rather
// than shipping a fifth executable to deploy, configure and forget to restart.
//
// It shares nothing with the content path. The point holds no keys, sees no
// bundles, and answers on a UDP port of its own.
//
// @tier shared

#include <engine/delivery/Source.hpp>
#include <engine/net/Transport.hpp>

#include <cstdint>
#include <memory>
#include <network/Advert.hpp>
#include <network/Presence.hpp>
#include <network/Rendezvous.hpp>
#include <string>
#include <vector>

namespace cdn {

	// What kind of stream this origin offers, and how it is found.
	//
	// @since v0.13
	struct StreamSettings {
		// Announce on the local subnet.
		bool Announce = false;

		// A rendezvous point to register with, as `host:port`, or empty.
		std::string RendezvousAddress;

		// Run a rendezvous point on this UDP port, or zero for none.
		//
		// Independent of registering with one: an origin can be the meeting
		// place without being a stream anybody meets *at*, which is what a
		// coordination service for other people's games looks like.
		uint16_t RendezvousListenPort = 0;

		// The secret that makes this a private stream: 64 hex characters, or a
		// passphrase. Empty is a public one.
		//
		// **What it gates is discovery, not delivery.** A client holding it can
		// find and verify this origin; the origin still checks a `cdn::Gate`
		// grant before serving a bundle. Two different questions with two
		// different answers, and collapsing them would make the key that finds
		// a stream also the key that draws from it - which is not what an
		// operator metering bandwidth wants.
		std::string Secret;

		// What to call this stream. Empty uses a placeholder.
		std::string Name;

		// What to show underneath the name - the publication being served.
		std::string Detail;

		// The HTTP port clients fetch from. **The one that was bound**, not the
		// one that was configured: zero binds an ephemeral one and an
		// announcement carrying the wrong number sends every client nowhere.
		uint16_t Port = 0;
	};

	// This origin, as something a client can find.
	//
	// @since v0.13
	class Stream {
	  public:
		// Opens whatever the settings asked for.
		//
		// @param settings What to offer, and how.
		// @param[out] error Filled when this returns null.
		// @return The stream, or null when a setting is not usable - a secret
		//         that is neither hex nor words, a rendezvous port already
		//         held. A subnet that will not carry a broadcast is not one of
		//         those: it is recorded and the rest runs.
		static std::unique_ptr<Stream> Open(const StreamSettings &settings, std::string &error);

		~Stream();

		Stream(const Stream &) = delete;
		Stream &operator=(const Stream &) = delete;

		// Announces what is due, serves the point when there is one, and
		// expires what went quiet.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Says goodbye, best effort.
		//
		// @param nowSeconds The current time.
		void Withdraw(double nowSeconds);

		// What this origin announces about itself.
		//
		// @return The advert.
		const network::Advert &Advertised() const {
			return Announcement;
		}

		// Whether announcements are going out.
		//
		// @return `true` when a beacon exists and will send.
		bool Announcing() const;

		// Whether a rendezvous point is running in this process.
		//
		// @return `true` when one was asked for and its port was bound.
		bool Meeting() const {
			return Point != nullptr;
		}

		// How many sessions the hosted point holds.
		//
		// @return The count, or zero when no point is running here.
		size_t Hosting() const;

		// What the hosted point has served.
		//
		// @return The counters, all zero when no point is running here.
		const network::PointCounters &MeetingCounters() const {
			return PointTally;
		}

	  private:
		Stream() = default;

		network::Advert Announcement;
		std::unique_ptr<network::Presence> Finding;

		// The meeting place, when this origin is one. Its own socket, because
		// it answers a protocol nothing else here speaks.
		std::unique_ptr<engine::net::Transport> PointSocket;
		std::unique_ptr<network::RendezvousPoint> Point;
		network::PointCounters PointTally;
	};

	// How to look for streams other people are offering.
	//
	// @since v0.13
	struct StreamSearch {
		// Listen for origins announcing on the local subnet.
		bool Browse = true;

		// A rendezvous point to ask, as `host:port`, or empty.
		std::string RendezvousAddress;

		// Secrets this process holds, so a private stream verifies rather than
		// listing as locked. Each is 64 hex characters or a passphrase.
		std::vector<std::string> Secrets;

		// How long a stream stays listed after it goes quiet.
		double ForgetAfterSeconds = 5.0;
	};

	// The streams this process can see, as a delivery source list.
	//
	// **The reason this lives in `mono.cdn` rather than in `mono.network`**: it
	// is the one place that knows a content origin's endpoint is an HTTP one
	// and that the thing to turn it into is a `delivery::Source`. The discovery
	// module deliberately knows neither - it would have to link `delivery` to
	// find out, and then a LAN game would be carrying a content-delivery
	// dependency.
	//
	// @since v0.13
	class StreamFinder {
	  public:
		// Opens whatever the search asked for.
		//
		// @param search What to look for.
		// @return The finder. Never null, for `network::Presence::Open`'s
		//         reason: one that opened nothing sees nothing, which is what a
		//         program with discovery switched off should get.
		static std::unique_ptr<StreamFinder> Open(const StreamSearch &search);

		~StreamFinder();

		StreamFinder(const StreamFinder &) = delete;
		StreamFinder &operator=(const StreamFinder &) = delete;

		// Drains what arrived and expires what went quiet.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Asks the rendezvous point for what it holds. Does nothing without
		// one.
		//
		// @param nowSeconds The current time.
		void Ask(double nowSeconds);

		// Every stream found, as sources a delivery client can be given.
		//
		// **In `Reach` order - nearest first - which is the order
		// `delivery::AssetClient` walks and therefore the order that means
		// "local first, then the origin next door, then the one across the
		// internet".** A list in any other order would be a preference nobody
		// stated.
		//
		// A private stream this process cannot verify is left out: a source
		// that cannot be drawn from is a source every fetch pays a timeout for.
		//
		// @return The sources, read-only.
		std::vector<engine::delivery::Source> Sources() const;

		// Everything seen, including what `Sources` left out.
		//
		// @return The directory.
		const network::Directory &Seen() const;

		// Everything seen, and the way a caller adds a row of its own.
		//
		// A stream from a config file is the same kind of thing as one heard on
		// the subnet - `network::Directory::Offer` is how it says so - and a
		// finder that could only be fed by a socket would need a second list
		// beside it for the origins somebody typed.
		//
		// @return The directory.
		network::Directory &Seen();

	  private:
		StreamFinder() = default;

		std::unique_ptr<network::Presence> Looking;
	};
}
