#pragma once

// The ways this engine's halves can be wired to each other.
//
// **A module's own suite runs one arrangement, because it only has one half.**
// `replication` can put an authority beside a replica; it cannot put a *client*
// beside a *server* with a `cdn` publication behind them and a `network`
// directory watching, because linking any of those from `engine/` is the layer
// violation the tier system exists to refuse. So the arrangements live here,
// where every tier is already on the link line, and the axes below are the
// choices a deployment actually makes.
//
// **Three axes, and each one is a thing that is either present or absent in a
// real deployment**:
//
// - `Transport` - what is between the serialiser and the deserialiser. Nothing,
//   a real link, or a real link that loses datagrams.
// - `Content` - whether the server also carries content for its clients, which
//   is `--content relay` in `mono.server`.
// - `Discovery` - whether the session announces itself and something is
//   listening, which is what a LAN browser does.
//
// **The cross product is the point.** Content over a lossy link and content
// over no link at all are different arrangements that exercise the same
// `ContentRelay`, and the bug this module has caught four times - a message
// that does not fit in a datagram - is invisible in one of them and fatal in
// the other. `AllArrangements` is the whole product, and `just unified --all`
// runs it.
//
// @tier client · escapes to server

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unified {

	// What sits between `Authority::Outgoing` and `Replica::Receive`.
	//
	// @since v0.18
	enum class Transport {
		// Nothing at all. A byte vector the authority produced is handed
		// straight to the replica, in order, complete.
		//
		// **The bisection**, and the arrangement this module was built for: a
		// failure that reproduces here is above `net` and a failure that does
		// not is below it.
		Direct,

		// A real `net` link: loopback sockets, `Packet` framing, a `Link` with
		// its budgets and its acknowledgement window, and a real cipher from a
		// real `net::Handshake`.
		//
		// Nothing is lost, so what this adds over `Direct` is *framing* - the
		// MTU, the reliable channel's ordering and the per-tick byte budget.
		Loopback,

		// `Loopback` with a `net::LossyTransport` on each end.
		//
		// Loss is nominated by ordinal and never by percentage, for the reason
		// `net::LossSettings` gives: "the fortieth datagram never arrived" is a
		// test and "ten percent loss" is a flake with a story attached.
		Lossy,
	};

	// Whether the server also carries content for its client.
	//
	// @since v0.18
	enum class Content {
		// No content path at all. The world crosses and nothing else does.
		None,

		// A `cdn` publication on disk, a `server::ContentRelay` answering out
		// of it, and a `client::ContentLink` reassembling on the other side.
		//
		// **The one flow that touches four modules**: `cdn` published it,
		// `delivery` fetches it, `server` rations it, `client` reassembles it.
		// Every one of those has a suite of its own and none of them can check
		// the seam to the next.
		Relayed,
	};

	// Whether the session announces itself.
	//
	// @since v0.18
	enum class Discovery {
		// Nothing announces and nothing listens.
		None,

		// A `network::Beacon` announcing the session on a broadcast subnet and
		// a `network::Directory` collecting it.
		//
		// Beside the session rather than through it, which is what a LAN
		// browser is: discovery is a second socket and its failure mode is a
		// host nobody can find rather than a world nobody can see.
		Advertised,
	};

	// One way of standing the engine up.
	//
	// @since v0.18
	struct Arrangement {
		// What is between the two halves.
		Transport Carrying = Transport::Direct;

		// Whether content crosses too.
		Content Serving = Content::None;

		// Whether the session is announced.
		Discovery Finding = Discovery::None;

		// The name `--arrangement` takes and `Parse` returns.
		//
		// Axes joined with `+`, defaults omitted: `direct`,
		// `lossy+relayed+advertised`. A name round-trips through `Parse`.
		//
		// @return The name.
		std::string Name() const;

		bool operator==(const Arrangement &) const = default;
	};

	// The name of one transport, as `Arrangement::Name` spells it.
	//
	// @param carrying The transport.
	// @return Its name.
	// @since v0.18
	std::string_view Name(Transport carrying);

	// The name of one content mode.
	//
	// @param serving The mode.
	// @return Its name.
	// @since v0.18
	std::string_view Name(Content serving);

	// The name of one discovery mode.
	//
	// @param finding The mode.
	// @return Its name.
	// @since v0.18
	std::string_view Name(Discovery finding);

	// Reads a name back.
	//
	// **Every field is hostile**, because this parses a command line: an
	// unknown axis, an axis named twice and an empty field are each refused
	// rather than defaulted, so a typo is an error and not a run of something
	// else.
	//
	// @param name The text, as `Arrangement::Name` produces it.
	// @return The arrangement, or nothing when the text names none.
	// @since v0.18
	std::optional<Arrangement> ParseArrangement(std::string_view name);

	// Every arrangement, in a stable order.
	//
	// **The whole cross product and not a chosen subset.** A matrix that
	// skipped the combinations somebody thought were uninteresting is a matrix
	// that tests what somebody already believed.
	//
	// @return All of them, `Direct` first.
	// @since v0.18
	std::vector<Arrangement> AllArrangements();
}
