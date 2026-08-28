#pragma once

// Fetching content over the connection this client already has.
//
// **The client half of relay mode, and it holds no authority at all.** A server
// in `ContentMode::Relay` keeps the origin connection to itself; this asks it
// for the routes an origin serves - `/manifest`, `/dictionary`,
// `/bundle/<hex>` - and hands whatever comes back to `delivery::AssetClient`,
// which verifies it against the publisher's signature exactly as it verifies
// bytes off a socket. **Relaying moves the transport and not the trust
// boundary**, so a server that relayed the wrong bytes is refused by the same
// three checks a compromised origin would be.
//
// **Retries are the server's to allow.** This end may ask again - a route that
// was refused falls through to the next source, and a delivery client that runs
// out of sources fails the request - and how often it may ask is decided by
// `server::ContentRelay`, because a limiter that lived here would be a limiter
// the interesting clients do not run.
//
// **Here rather than in `Engine::delivery`, because it names a game link.** A
// connector is L12 and delivery is L11, so the seam is `delivery::RelayChannel`
// and this is the one implementation of it that knows what a replication
// session is - which is a client attachment, and attachments are what this
// directory holds.

#include <engine/delivery/Relay.hpp>
#include <engine/game/Content.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace client {

	// What survived the checks a server-named source has to pass.
	//
	// @since v0.16
	struct OfferedContent {
		// The sources this client may use, in the order the server named them.
		std::vector<engine::delivery::Source> Permitted;

		// How many the allow-list refused.
		size_t RefusedByAllowList = 0;

		// How many named a host *name* rather than an address.
		//
		// Counted so the caller can say it once. `net::Endpoint::Parse` refuses a
		// name on purpose - resolving one blocks on a network service and nothing
		// on the fetch path may block - and a stream of individually plausible
		// fetch failures says the same thing far less usefully.
		size_t UnresolvedNames = 0;

		// How many named a kind this build has no row for.
		size_t UnknownKinds = 0;
	};

	// Turns what a server said into sources this client may use.
	//
	// **Every field of a directory is untrusted**, because a client that is
	// told to fetch from an arbitrary host is a request-forgery primitive. So
	// an HTTP endpoint passes
	// `HostPermitted` against this client's own allow-list and then has to be an
	// address rather than a name, and anything that fails either is dropped and
	// counted rather than fetched from and reported later.
	//
	// @param directory What the server said.
	// @param allowedHosts The hosts this client permits, or empty for no
	//        restriction.
	// @return What survived, and how much did not.
	// @since v0.16
	OfferedContent AcceptOfferedContent(
		const engine::game::ContentDirectory &directory, const std::vector<std::string> &allowedHosts
	);

	// Assembles the ordered list a client fetches through.
	//
	// **The order is the whole policy** - `delivery/AGENTS.md` refuses a strategy
	// enum beside the list - and the order is:
	//
	// 1. What this client was configured with, because that came from a config
	//    file or a command line, which is to say from the person running it.
	// 2. What the server named, appended rather than substituted.
	// 3. The server itself, last, when there is a link to relay over. It is the
	//    source that always exists, so it is the one that answers when nothing
	//    before it did.
	//
	// @param configured This client's own sources, as `dir:PATH` or `HOST:PORT`.
	// @param offered What survived `AcceptOfferedContent`.
	// @param relayLabel What to call the link, or empty when there is none.
	// @return The list, in priority order.
	// @since v0.16
	std::vector<engine::delivery::Source> MergeContentSources(
		const std::vector<std::string> &configured,
		const std::vector<engine::delivery::Source> &offered,
		std::string_view relayLabel
	);

	// The most bytes one relayed route may claim to be.
	//
	// **A bound on what a message can make this process allocate.** A server is
	// something anybody can run, so a total arriving on
	// the first chunk of a route is attacker-controlled, and a buffer sized from
	// it without a ceiling is a few bytes on the wire becoming a gigabyte in the
	// allocator. Comfortably above any group a publisher produces.
	//
	// @since v0.16
	inline constexpr uint32_t MAXIMUM_RELAYED_ROUTE_BYTES = 64u * 1024u * 1024u;

	// The most routes this end will have outstanding.
	//
	// @since v0.16
	inline constexpr size_t MAXIMUM_RELAYED_ROUTES = 4;

	// Asks a game server for content routes and reassembles the answers.
	//
	// **One owner, one thread**, like everything else on the delivery path.
	//
	// @since v0.16
	class ContentLink final : public engine::delivery::RelayChannel {
	  public:
		// How a request leaves this process.
		//
		// **A callback rather than a connector**, so the suite that pins the
		// reassembly opens no socket and states no timeout - `net`'s rule about
		// a `Link` doing no I/O, one layer along.
		//
		// @return Whether the link took it. `false` is ordinary backpressure and
		//         the request is offered again on a later pump.
		using Sender = std::function<bool(std::span<const std::byte>)>;

		// @param send How a request leaves. Must outlive this object's use.
		explicit ContentLink(Sender send);

		// Asks the server for one route.
		bool Ask(uint64_t ticket, std::string_view route) override;

		// Takes whatever has been reassembled since the last call.
		void Collect(std::vector<engine::delivery::RelayAnswer> &into) override;

		// Says a ticket will never be taken.
		void Abandon(uint64_t ticket) override;

		// Takes one message the server sent.
		//
		// **Every field of it is hostile.** A chunk that does not fit the total
		// it names, one that claims a total past `MAXIMUM_RELAYED_ROUTE_BYTES`,
		// one for a ticket this end never issued, and one that arrives out of
		// order are each dropped and counted rather than acted on.
		//
		// @param message The payload, exactly as it arrived.
		// @return Whether this was a content message at all, which is how a
		//         caller knows not to offer it to anybody else.
		bool Receive(std::span<const std::byte> message);

		// What this link has done.
		//
		// @since v0.16
		struct Counters {
			// Routes asked for.
			uint64_t Requests = 0;

			// Pieces accepted.
			uint64_t Chunks = 0;

			// Routes reassembled in full.
			uint64_t Completed = 0;

			// Routes the server said it would not answer.
			uint64_t Refused = 0;

			// Pieces dropped: a ticket nobody asked for, a total this end will
			// not hold, or a piece out of order.
			//
			// **Counted apart from a refusal**, `assets::Grant`'s rule: a server
			// that has no content and a server sending something this end
			// refuses to assemble are two incidents, and one number for both
			// buries the second.
			uint64_t Discarded = 0;
		};

		// What this link has done.
		const Counters &Stats() const {
			return Tally;
		}

		// How many routes are being assembled.
		size_t Assembling() const {
			return Live.size();
		}

	  private:
		// One route being reassembled.
		struct Arriving {
			uint64_t Ticket = 0;
			std::vector<std::byte> Bytes;

			// How much is held, which is also where the next piece must start.
			//
			// **Contiguous rather than a set of ranges**, because the user
			// channel is reliable and ordered - `replication::User` says so -
			// so a piece that does not start here is a bug or an attack and
			// there is nothing to gain by assembling it.
			uint32_t Filled = 0;

			bool Sized = false;
		};

		Arriving *Find(uint64_t ticket);

		Sender Send;
		std::vector<Arriving> Live;
		std::vector<engine::delivery::RelayAnswer> Finished;
		Counters Tally;
	};
}
