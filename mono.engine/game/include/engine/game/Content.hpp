#pragma once

// How a host and one of its clients talk about content, and the two modes that
// makes possible.
//
// **A deployment decides which mode it is in, and both are the same programs.**
// The origin's three deployments are `cdn::CDNSettings` field combinations
// rather than three classes, and this is the same argument one layer up: a host
// is in one of two content modes and moving between them is a setting rather
// than a rebuild.
//
// - **Relay.** The host holds the origin connection and the client holds none.
//   A client asks for the routes an origin serves - `/manifest`,
//   `/dictionary`, `/bundle/<hex>` - and the host answers with the bytes over
//   the link they already share. `ContentRequest`, `ContentChunk` and
//   `ContentRefusal` are that conversation. **The client has no authority in
//   it**: it may ask and it may ask again, and how often it may do either is
//   decided by the host, because a client is not trusted to be reasonable.
// - **Redirect.** The host tells the client where the origins are, hands it the
//   grant that admits it to them, and names the publisher whose signature to
//   trust. `ContentDirectory` is that one message, and after it the client
//   fetches for itself.
//
// **Relaying does not move the trust boundary and neither does redirecting.**
// Whatever arrives is verified against a manifest root the publisher signed -
// `delivery/AGENTS.md`'s three checks, in that order - so a host that relayed
// the wrong bytes and a host that named a hostile origin are refused by the
// same code, at the same place, for the same reason.
//
// **Here rather than in `replication`, for `Play.hpp`'s reason.** `game` is the
// highest module a client and a server both link, which makes it the only place
// one definition can serve both; `replication` carries these as
// `MessageKind::User` and must not learn what a content route is.
//
// **One tag space, and it is `PlayMessage`.** Both directions of the user
// channel are already shared - a join notice goes down it and a move goes up it
// - so a second enum starting at one again is the day an untagged payload is
// read as the wrong message and a wrong answer looks like a working one.
//
// @tier L10 · shared

#include <engine/game/Play.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::game {

	// The most endpoints one directory may name.
	//
	// Bounds what an inbound message can make a client allocate. A host naming
	// more origins than this has a deployment nobody can reason about anyway.
	//
	// @since v0.16
	inline constexpr size_t MAXIMUM_CONTENT_ENDPOINTS = 16;

	// The longest route a request may name.
	//
	// A bundle route is a prefix and 64 hex characters, so this is generous
	// twice over and is here to bound the parse rather than to be reached.
	//
	// @since v0.16
	inline constexpr size_t MAXIMUM_ROUTE_BYTES = 128;

	// The most content bytes one chunk may carry.
	//
	// **Sized so the encoded message fits a sealed datagram**, which this module
	// cannot say in code: `net::Packet::MAXIMUM_MESSAGE_BYTES` is L11 and `game`
	// deliberately links no `net`. The arithmetic is 1200 bytes of datagram less
	// a 25-byte header, less the cipher's 16-byte tag, less seven bytes of
	// `replication::User` framing and twenty-one of this message's own - which
	// leaves 1131, and this is comfortably under it. `mono.server` links `net`
	// and holds the `static_assert` that keeps the two in step.
	//
	// @since v0.16
	inline constexpr size_t MAXIMUM_CONTENT_CHUNK_BYTES = 1024;

	// One place a host says content can be fetched from.
	//
	// **Every field is a string, and that is rule 4 rather than convenience.**
	// A kind written as an ordinal is a number derived from the order somebody
	// declared an enum in, and this crosses a wire between two builds that may
	// not be the same build.
	//
	// @since v0.16
	struct ContentEndpoint {
		// What it is called, in a log line and in a preferences row.
		std::string Name;

		// `dir` for a published tree on a filesystem, `http` for an origin.
		//
		// A kind this build does not know is skipped by the receiver rather
		// than refused, so a host of a later build naming one more kind does not
		// cost a client the endpoints it *can* use.
		std::string Kind;

		// The directory path, or `host:port`.
		//
		// **Untrusted, and pre-resolved.** A client checks it against its own
		// allow-list before it is fetched from, and `net::Endpoint::Parse`
		// refuses a host *name* on purpose - resolving one blocks, and nothing
		// on the fetch path may block.
		std::string Location;
	};

	// Where content is, and what a client needs to get at it.
	//
	// @since v0.16
	struct ContentDirectory {
		// The origins, in the order the host wants them tried.
		std::vector<ContentEndpoint> Endpoints;

		// The grant that admits this client's requests to them.
		//
		// **Opaque here and carried, never inspected.** `assets::Grant` issues
		// and `cdn::Gate` opens; a message format that read one would be a
		// second implementation of a security check.
		std::vector<std::byte> Grant;

		// The publisher whose signature to trust, as 64 hex characters, or
		// empty when the host has none to name.
		//
		// **Carried as its text**, which keeps `Engine::assets` off this
		// module's link line for one field, and is the same spelling
		// `--publisher-key` and `cdn --signing-key` already use.
		std::string PublisherKey;
	};

	// A client asking for one content route.
	//
	// @since v0.16
	struct ContentRouteRequest {
		// What the answer will be named by, in the client's own numbering.
		uint64_t Ticket = 0;

		// The route, exactly as an origin serves it.
		//
		// **Hostile text.** A host checks it against the closed list of routes a
		// relay carries before anything else happens to it.
		std::string Route;
	};

	// One piece of a route's bytes.
	//
	// **The same shape `replication::SnapshotChunk` has, for the same reasons.**
	// A route is megabytes and a sealed datagram is about a kilobyte, so the
	// total is carried on every piece rather than only the first - a receiver
	// sizes its buffer once and refuses a total it will not accept before any of
	// it arrives - and the offset is explicit rather than implied by arrival
	// order, because an appending receiver would assemble bytes out of order and
	// never know.
	//
	// @since v0.16
	struct ContentChunk {
		// Which request this answers.
		uint64_t Ticket = 0;

		// How long the whole route is.
		uint32_t TotalBytes = 0;

		// Where this piece starts within that total.
		uint32_t Offset = 0;

		// This piece's bytes.
		std::vector<std::byte> Bytes;
	};

	// A host saying it will not answer a request.
	//
	// **A boolean and not a reason**, which is `cdn::Gate`'s rule arriving one
	// hop later: the client falls through to its next source either way, and a
	// reason handed back over a wire is an oracle. The host's own log carries
	// the detail its operator needs.
	//
	// @since v0.16
	struct ContentRefusal {
		// Which request is refused.
		uint64_t Ticket = 0;
	};

	// Packs a content directory.
	//
	// @param directory What the host is offering.
	// @return The bytes to hand to a user-message send.
	// @since v0.16
	std::vector<std::byte> EncodeContentDirectory(const ContentDirectory &directory);

	// Unpacks a content directory.
	//
	// **Returns false for anything that is not one**, including a message with
	// another tag, which is what makes it safe to feed every user message
	// through this and ignore the ones it refuses.
	//
	// @param message The bytes that arrived.
	// @param out     Filled on success, untouched otherwise.
	// @return `false` when the message is not a content directory.
	// @since v0.16
	bool DecodeContentDirectory(std::span<const std::byte> message, ContentDirectory &out);

	// Packs a route request.
	//
	// @param request What is being asked for.
	// @return The bytes to submit.
	// @since v0.16
	std::vector<std::byte> EncodeContentRequest(const ContentRouteRequest &request);

	// Unpacks a route request.
	//
	// @param message The bytes that arrived.
	// @param out     Filled on success, untouched otherwise.
	// @return `false` when the message is not a route request, or names a route
	//         longer than `MAXIMUM_ROUTE_BYTES`.
	// @since v0.16
	bool DecodeContentRequest(std::span<const std::byte> message, ContentRouteRequest &out);

	// Packs one piece of a route.
	//
	// @param chunk The piece.
	// @return The bytes to send.
	// @since v0.16
	std::vector<std::byte> EncodeContentChunk(const ContentChunk &chunk);

	// Unpacks one piece of a route.
	//
	// @param message The bytes that arrived.
	// @param out     Filled on success, untouched otherwise.
	// @return `false` when the message is not a chunk, carries more than
	//         `MAXIMUM_CONTENT_CHUNK_BYTES`, or describes a piece that does not
	//         fit inside the total it names.
	// @since v0.16
	bool DecodeContentChunk(std::span<const std::byte> message, ContentChunk &out);

	// Packs a refusal.
	//
	// @param refusal Which request is refused.
	// @return The bytes to send.
	// @since v0.16
	std::vector<std::byte> EncodeContentRefusal(const ContentRefusal &refusal);

	// Unpacks a refusal.
	//
	// @param message The bytes that arrived.
	// @param out     Filled on success, untouched otherwise.
	// @return `false` when the message is not a refusal.
	// @since v0.16
	bool DecodeContentRefusal(std::span<const std::byte> message, ContentRefusal &out);
}
