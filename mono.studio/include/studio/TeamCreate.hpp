#pragma once

// arch-waiver public-header: forward studio API. Collaborative editors use
// this complete team-create contract.

// Team create's session: discovery, invitation, and the ordered edit stream
// shared by the editors in it.
//
// **What this is, exactly.** It is the session layer - an editor announces
// itself on the subnet or registers at a rendezvous point, other editors see
// it, and a `SessionKey` decides who is invited. It is the same
// `network::Presence` the client browses servers with and the origin offers a
// distribution stream with, at `Purpose::Studio` instead of `Game` or
// `Content`, which is the whole reason discovery is a member rather than three
// features.
//
// Discovery only hands over an endpoint. `EditStream` is the shared-document
// half: committed waypoints cross that endpoint in order, remote changes stay
// out of local undo, and model leases serialize edits that overlap.
//
// @tier client

#include <engine/net/Transport.hpp>

#include <cstdint>
#include <memory>
#include <network/Advert.hpp>
#include <network/Directory.hpp>
#include <network/Presence.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <studio/EditStream.hpp>

namespace studio {

	// What this editor offers when it hosts.
	//
	// @since v0.13
	struct TeamCreateSettings {
		// What to call the session. Empty uses a placeholder.
		std::string Name;

		// The project being edited, shown under the name.
		std::string Project;

		// The secret that makes this session private: 64 hex characters, or a
		// passphrase. Empty invites the whole subnet, which is a choice
		// somebody should make on purpose.
		std::string Secret;

		// A rendezvous point to register with, so editors off this subnet can
		// find it. Empty is LAN only.
		std::string RendezvousAddress;

		// The UDP port the edit stream listens on. Zero binds an ephemeral one,
		// which is what a person hosting from a laptop wants - the announcement
		// carries the port that was bound, so nothing has to be agreed in
		// advance.
		uint16_t Port = 0;

		// How many editors this session will take, or zero for no stated limit.
		uint16_t PeerLimit = 8;
	};

	// This editor's presence among the others.
	//
	// One per editor, held by `Editor` and pumped once a frame. Idle until
	// somebody asks it to host or to watch, so an editor that never opens the
	// panel opens no socket.
	//
	// @since v0.13
	class TeamCreate {
	  public:
		// @param log      The command log this editor's edits come from and
		//        arriving edits are applied through.
		// @param universe The worlds those edits touch.
		TeamCreate(CommandLog &log, engine::world::Universe &universe);
		~TeamCreate();

		TeamCreate(const TeamCreate &) = delete;
		TeamCreate &operator=(const TeamCreate &) = delete;

		// Starts announcing this editor, and watching for the others.
		//
		// Replaces whatever was running: hosting twice from one editor would be
		// two sessions somebody has to tell apart, and the second would be the
		// one nobody was invited to.
		//
		// @param settings What to offer.
		// @param[out] error Filled when this returns false.
		// @return `false` when a setting is not usable - a secret that is
		//         neither hex nor words. A subnet that will not carry a
		//         broadcast is not one of those: it is reported through
		//         `Fault` and the rest runs.
		bool Host(const TeamCreateSettings &settings, std::string &error);

		// Joins the session a listing names.
		//
		// **The two halves arrive together and that is the point.** Discovery
		// hands over an address; the edit stream is what makes joining mean
		// something. A `Join` that only connected would be a browser.
		//
		// @param at         Where the host is - `network::Listing::Dial`.
		// @param nowSeconds The current time.
		// @param[out] error Filled when this returns false.
		// @param secret     Invitation key for a private session, or empty for public.
		// @return `false` when the address is not one, or no socket could be
		//         opened.
		// @since v0.13
		bool Join(
			const engine::net::Endpoint &at,
			double nowSeconds,
			std::string &error,
			std::string_view secret = {}
		);

		// The edits crossing between this editor and the others.
		//
		// @return The stream, or null when nothing is hosted or joined.
		// @since v0.13
		EditStream *Edits() {
			return Stream.get();
		}

		// Watches for other editors without announcing this one.
		//
		// What somebody opening the panel to look gets, before they have
		// decided to invite anybody.
		//
		// @param rendezvousAddress A point to ask as well as the subnet, or
		//        empty.
		// @param secrets Keys this editor holds, so a private session it was
		//        invited to lists as joinable rather than locked.
		// @return `false` when a secret is not usable.
		bool Watch(const std::string &rendezvousAddress, std::span<const std::string> secrets);

		// Stops announcing and closes every socket.
		//
		// @param nowSeconds The current time.
		void Leave(double nowSeconds);

		// Announces what is due, drains what arrived, and carries edits.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Puts one committed waypoint on the wire.
		//
		// What `CommandLog::Watcher::Committed` is wired to. Does nothing when
		// no session is open, which is what keeps an editor that never opens
		// the panel from paying for any of this.
		//
		// @param waypoint   Which waypoint it is, so a refusal can name it.
		// @param commands   One waypoint's worth.
		// @param nowSeconds The current time.
		// @since v0.13
		void PublishEdits(uint64_t waypoint, std::span<const Command> commands, double nowSeconds);

		// Tells the world how many editors are in.
		//
		// @param count The number, including this one.
		void SetCollaborators(uint16_t count);

		// Whether this editor is announcing.
		//
		// @return `true` while hosting.
		bool Hosting() const {
			return Announcing;
		}

		// Whether anything is being listened for.
		//
		// @return `true` while hosting or watching.
		bool Watching() const {
			return Presence_ != nullptr;
		}

		// What this editor announces about itself.
		//
		// @return The advert. Its session id is what somebody is given along
		//         with the key.
		const network::Advert &Session() const {
			return Announcement;
		}

		// The key this session was opened with, as text somebody can pass on.
		//
		// @return The 64 characters, or empty for a public session.
		const std::string &Invitation() const {
			return KeyText;
		}

		// The other editors this one can see.
		//
		// **Includes this editor's own session when a rendezvous point is
		// listing it back**, which is not a bug worth hiding: a host that
		// cannot see its own registration has no way to tell "the point never
		// heard me" from "nobody else is here". `Session()` is what a caller
		// filters by.
		//
		// @return The listings.
		std::span<const network::Listing> Peers() const;

		// What could not be opened.
		//
		// @return The fault, or `None`.
		network::PresenceFault Fault() const;

	  private:
		bool Open(
			const TeamCreateSettings &settings,
			bool announce,
			std::span<const std::string> secrets,
			std::string &error,
			std::optional<network::SessionKey> key = std::nullopt
		);

		std::unique_ptr<network::Presence> Presence_;

		// The socket the edit stream runs on, and the stream over it.
		//
		// **Its own socket, not the discovery one.** Discovery announces from
		// an ephemeral port and listens on a well-known one; a session is a
		// third thing with a lifetime of its own, and a guest that joined has
		// no reason to stop browsing.
		std::unique_ptr<engine::net::Transport> Socket;
		std::unique_ptr<EditStream> Stream;

		CommandLog *Log = nullptr;
		engine::world::Universe *Worlds = nullptr;
		network::Advert Announcement;
		std::string KeyText;
		bool Announcing = false;
	};
}
