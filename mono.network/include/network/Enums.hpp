#pragma once

// The three axes a session is described along, and the port announcements use.
//
// Every one of these crosses a boundary. A row in a browser says how it was
// reached and whether it will let you in; an announcement carries what the
// session is *for*, so a client does not list the studio next door and the
// studio does not list a content origin. So each is a type rather than a bool
// that loses its meaning at the first hop - `engine::net::Enums.hpp`'s rule,
// applied one layer up.
//
// **All three are closed lists whose ordinals reach a wire.** A value may be
// added at the end and none may be reordered or removed: an announcement from
// an older build must not decode as a different kind of session in a newer one,
// which is the quietest possible version of this going wrong.
//
// @tier shared

#include <cstdint>

namespace network {

	// The UDP port a LAN announcement is broadcast to.
	//
	// Fixed rather than configurable-by-default, because discovery only works
	// when both ends already agree - a port somebody has to set on the host and
	// on every client is a port that will be set on some of them. It is still a
	// setting, for the one case that needs it: two unrelated sessions sharing a
	// subnet, which is a LAN party running two games.
	//
	// 47600 is in the ephemeral range's neighbourhood and registered to nobody.
	// It was picked rather than derived, which is the honest thing to say about
	// it.
	constexpr uint16_t DISCOVERY_PORT = 47600;

	// How a peer was found, and therefore what its address means.
	//
	// **Not how the traffic is carried.** Every one of these ends at a UDP
	// endpoint that `engine::net` treats identically; the difference is what
	// had to happen before there was one, and how much to trust that it will
	// keep working. A `Peer` address is a hole punched through two NATs and
	// stops working when either mapping expires; a `Remote` one was typed by a
	// person and is as durable as their router.
	//
	// @since v0.13
	enum class Reach : uint8_t {
		// This process, over a loopback. Single-player, and the studio's Play.
		Loopback = 0,

		// Heard announcing itself on this subnet.
		Lan = 1,

		// Reached through a rendezvous point, by punching both NATs.
		Peer = 2,

		// An address somebody supplied - a command line, a config file, a
		// server list.
		Remote = 3,
	};

	// Who is allowed to join, and it is exactly two answers.
	//
	// **A third value was deliberately not added.** "Invite only" and "friends
	// of the host" are decisions about *who a person is*, and nothing at this
	// layer knows what a person is - that belongs where accounts are, one
	// authority up. What this can honestly express is whether joining needs a
	// secret both ends already hold, and that is one bit.
	//
	// @since v0.13
	enum class Access : uint8_t {
		// Anyone who can reach the address may try. What a public distribution
		// stream is, and what a LAN game is by default.
		Public = 0,

		// A pre-shared `SessionKey` is required.
		//
		// **This authenticates; it does not hide.** A private session
		// announcing on a subnet is *visible* to everybody on that subnet and
		// joinable by nobody without the key. Saying so plainly is better than
		// letting somebody later assume the name of their session is a secret,
		// because a broadcast datagram is readable by anything on the link and
		// always will be.
		Private = 1,
	};

	// What the session is for.
	//
	// The one field that keeps three programs off each other's lists. A client
	// browsing for a game must not offer the content origin running on the same
	// machine, and a studio looking for a team-create session must not offer
	// either.
	//
	// @since v0.13
	enum class Purpose : uint8_t {
		// A world being served to players. `mono.server`, and a client hosting
		// one in its own process.
		Game = 0,

		// An editor session other editors may join. Team create.
		Studio = 1,

		// A content origin, and the distribution stream it serves. The endpoint
		// is an HTTP one rather than a UDP one, which is the one place these
		// three differ in what the address is *for*.
		Content = 2,
	};

	// Returns a stable, human-readable name for a reach.
	//
	// @param reach The reach to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Reach reach);

	// Returns a stable, human-readable name for an access policy.
	//
	// @param access The policy to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Access access);

	// Returns a stable, human-readable name for a purpose.
	//
	// @param purpose The purpose to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Purpose purpose);
}
