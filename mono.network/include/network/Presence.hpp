#pragma once

// The one object a program owns to be findable and to find, sockets included.
//
// `Beacon`, `Directory` and `RendezvousClient` each take a transport rather
// than opening one, which is what makes all three testable over a loopback with
// no network at all. Somebody still has to open the real sockets and decide
// which of the three exist, and doing that in `mono.server`, `mono.client`,
// `mono.studio` and `mono.cdn` separately would be the same forty lines four
// times - and four places for the discovery port to be got wrong.
//
// So this is the composition, and it is deliberately the only thing in this
// module that opens a socket.
//
// ## Two sockets, and why not one
//
// A host announces from an **ephemeral** port and a browser listens on the
// **well-known** one. A process that does both - the studio, which hosts team
// create and lists other people's; a client that hosts a session for its
// friends and still browses - needs both, because one socket cannot be both
// ephemeral and well-known.
//
// The rendezvous client shares the announcing socket rather than opening a
// third. That is not tidiness: **a punched hole belongs to the port that
// punched it**, so a registration sent from one socket and a poke sent from
// another are two different mappings, and the peer was told about the first.
//
// ## Everything is optional and nothing is required
//
// A dedicated server with no LAN worth announcing on opens no sockets at all
// and `Pump` does nothing. A client that only ever types an address in gets a
// `Directory` with no transport behind it, which still holds the rows it was
// offered - so the code that walks a session list is the same code whether
// discovery is on or off. That is the property worth having: no caller has an
// "is discovery enabled" branch.
//
// @tier shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>

#include <cstdint>
#include <memory>
#include <network/Advert.hpp>
#include <network/Beacon.hpp>
#include <network/Directory.hpp>
#include <network/Enums.hpp>
#include <network/Rendezvous.hpp>
#include <network/SessionKey.hpp>
#include <optional>
#include <span>
#include <string>

namespace network {

	// What a process wants from the network around it.
	//
	// @since v0.13
	struct PresenceSettings {
		// Whether to announce on the subnet. Needs an advert.
		bool Announce = false;

		// Whether to listen for announcements on the subnet.
		bool Discover = false;

		// The rendezvous point, as `host:port`, or empty for none.
		//
		// Text rather than an `Endpoint` because that is what a command line
		// and a config file hold, and `Endpoint::Parse` refuses a host name for
		// a reason `net` gives at length - resolving one blocks. An operator
		// with a name rather than an address resolves it themselves and passes
		// the result.
		std::string RendezvousAddress;

		// The port announcements are broadcast to and listened for on.
		uint16_t DiscoveryPort = DISCOVERY_PORT;

		// The protocol this process speaks. Both halves: what is announced, and
		// what is listed.
		uint32_t Protocol = 0;

		// What this process is, and what it collects. Both halves again - a
		// studio announces `Studio` and lists `Studio`.
		Purpose Use = Purpose::Game;

		// How often to announce.
		BeaconSettings Beacon;

		// How long a session stays listed.
		DirectorySettings Directory;

		// How the rendezvous client paces itself.
		RendezvousSettings Rendezvous;
	};

	// What opening a presence produced, when it did not produce one.
	//
	// @since v0.13
	enum class PresenceFault : uint8_t {
		// Nothing went wrong.
		None = 0,

		// A socket for announcing could not be opened, or would not broadcast.
		NoBeaconSocket = 1,

		// The discovery port could not be bound. Almost always another program
		// holding it without `ReuseAddress`.
		NoDiscoverySocket = 2,

		// `RendezvousAddress` was not an address and a port.
		BadRendezvousAddress = 3,
	};

	// Returns a stable, human-readable name for a fault.
	//
	// @param fault The fault to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(PresenceFault fault);

	// Being findable, and finding - over whichever of the three reaches are
	// configured.
	//
	// @since v0.13
	class Presence {
	  public:
		// Opens whatever the settings asked for.
		//
		// **A partial success is a success.** A machine with no route to the
		// subnet broadcast address still wants its rendezvous registration, and
		// a discovery port already held by another program still leaves an
		// announcement worth making. So a socket that could not be opened turns
		// off the half that needed it, records a `Fault`, and the rest runs -
		// the alternative is a program that refuses to start over a feature
		// nobody asked to be essential.
		//
		// @param settings What to open.
		// @param advert   What to announce, when announcing. Ignored otherwise.
		// @param key      The session key, for a `Private` advert. Moved in.
		// @param session  The transport this process's session traffic uses, or
		//        null. **Given one, the rendezvous runs over it rather than
		//        over the announcing socket** - which is the only arrangement
		//        in which a punched hole is worth anything, because a router's
		//        mapping belongs to a port. It is borrowed and never drained
		//        here: its owner drains it and routes through `Deliver`. See
		//        `replication::Listener::SetForeign`.
		// @return The presence. Never null: one that opened nothing is a
		//         presence that does nothing, which is what a program with
		//         discovery switched off should get.
		static std::unique_ptr<Presence> Open(
			const PresenceSettings &settings,
			const Advert &advert = {},
			std::optional<SessionKey> key = std::nullopt,
			engine::net::Transport *session = nullptr
		);

		~Presence();

		Presence(const Presence &) = delete;
		Presence &operator=(const Presence &) = delete;

		// Announces what is due, drains what arrived, and expires what went
		// quiet.
		//
		// Called once per tick, at the barrier, with the same clock everything
		// else uses.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Routes one datagram the session's owner took off a shared transport.
		//
		// Only meaningful when `Open` was given a session transport. Anything
		// that is not one of this module's messages is left alone, which is
		// what makes sharing the socket possible at all.
		//
		// @param datagram   The bytes.
		// @param from       Where they came from.
		// @param nowSeconds The current time.
		// @return Whether this took the datagram.
		bool
		Deliver(std::span<const std::byte> datagram, const engine::net::Endpoint &from, double nowSeconds);

		// Every session this process can see.
		//
		// @return The directory, whether or not any transport feeds it.
		Directory &Seen() {
			return Table;
		}

		// Every session this process can see.
		//
		// @return The directory.
		const Directory &Seen() const {
			return Table;
		}

		// Replaces what is announced and registered.
		//
		// @param advert The new record.
		void SetAdvert(const Advert &advert);

		// What is being announced.
		//
		// @return The advert.
		const Advert &Advertised() const {
			return Saying;
		}

		// Says goodbye on every channel that is open, best effort.
		//
		// @param nowSeconds The current time.
		void Withdraw(double nowSeconds);

		// Asks the rendezvous point for what it holds.
		//
		// Does nothing when no point is configured. A LAN-only presence has
		// nothing to ask.
		//
		// @param nowSeconds The current time.
		void Browse(double nowSeconds);

		// Starts an attempt to reach a session through the rendezvous point.
		//
		// @param session    Which session.
		// @param key        Its key, when it is private. Moved in.
		// @param nowSeconds The current time.
		// @return `false` when no point is configured, or the id is null.
		bool Reach(const SessionId &session, std::optional<SessionKey> key, double nowSeconds);

		// Where the current attempt has got to.
		//
		// @return The state, or `Idle` when there is no rendezvous client.
		ReachState Reaching() const;

		// The address the peer answered on.
		//
		// @return The endpoint, or an invalid one unless the state is
		//         `Reached`.
		engine::net::Endpoint Reached() const;

		// This process's address as the rendezvous point saw it.
		//
		// @return The endpoint, or an invalid one.
		engine::net::Endpoint Reflexive() const;

		// The address this process announces from, for a log line that has to
		// say something true about which socket is open.
		//
		// @return The endpoint, or an invalid one when nothing is announced.
		engine::net::Endpoint AnnouncingFrom() const;

		// Whether announcements are going out.
		//
		// @return `true` when a beacon exists and will send.
		bool Announcing() const;

		// Whether the subnet is being listened to.
		//
		// @return `true` when a discovery socket is open.
		bool Discovering() const {
			return Listening != nullptr;
		}

		// Whether a rendezvous point is configured and reachable to talk to.
		//
		// @return `true` when a client exists.
		bool Rendezvousing() const {
			return Meeting != nullptr;
		}

		// What could not be opened.
		//
		// @return The fault, or `None`.
		PresenceFault Fault() const {
			return Trouble;
		}

	  private:
		explicit Presence(const DirectorySettings &table) : Table(table) {}

		Advert Saying;

		// The ephemeral socket announcements and rendezvous traffic leave from.
		std::unique_ptr<engine::net::Transport> Announcer;

		// The well-known socket announcements arrive on.
		std::unique_ptr<engine::net::Transport> Listening;

		std::optional<Beacon> Broadcasting;
		std::unique_ptr<RendezvousClient> Meeting;

		// Whether the rendezvous shares somebody else's transport. When it
		// does, `Pump` must not drain it - its owner does, and routes here.
		bool Sharing = false;

		// Held by value and always present, so a caller walking sessions has
		// the same code whether or not anything feeds it.
		Directory Table;

		PresenceFault Trouble = PresenceFault::None;
	};
}
