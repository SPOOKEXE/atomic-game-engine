#pragma once

// Team create's networking half: an editor saying it is here, and seeing the
// others.
//
// **What this is, exactly.** It is the session layer — an editor announces
// itself on the subnet or registers at a rendezvous point, other editors see
// it, and a `SessionKey` decides who is invited. It is the same
// `network::Presence` the client browses servers with and the origin offers a
// distribution stream with, at `Purpose::Studio` instead of `Game` or
// `Content`, which is the whole reason discovery is a member rather than three
// features.
//
// **What this is not, and saying so plainly matters more than usual here.**
// There is no shared document. Two editors that can see each other cannot yet
// edit one place together — that needs a change model with an ordering, and
// this repository has one (`replication::Authority`) that is built for a
// server's world rather than for two people's undo stacks. So what exists is
// the part that has to exist first and the part that was actually asked for at
// v0.13: **the network**. A panel that pretended otherwise would be the worse
// outcome, because "half-added is worse than not started" is exactly about a
// feature nobody can tell the shape of.
//
// The address a peer is listed at is the editor's hosted server — the one
// `RunMode::Play` already stands up — so when the shared document arrives it
// has somewhere to arrive.
//
// @tier client

#include <cstdint>
#include <memory>
#include <network/Advert.hpp>
#include <network/Directory.hpp>
#include <network/Presence.hpp>
#include <span>
#include <string>

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

		// The port peers would connect to. Zero when nothing is hosted yet, and
		// an announcement carrying zero is one nothing can act on — which is
		// why `Hosting` reports it rather than hiding it.
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
		TeamCreate();
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
		// @return `false` when a setting is not usable — a secret that is
		//         neither hex nor words. A subnet that will not carry a
		//         broadcast is not one of those: it is reported through
		//         `Fault` and the rest runs.
		bool Host(const TeamCreateSettings &settings, std::string &error);

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

		// Announces what is due and drains what arrived.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

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
			std::string &error
		);

		std::unique_ptr<network::Presence> Presence_;
		network::Advert Announcement;
		std::string KeyText;
		bool Announcing = false;
	};
}
