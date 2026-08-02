#pragma once

// What actually moves the bytes, and the one thing a caller may assume about it.
//
// A transport carries whole datagrams between endpoints. That is all it does. It
// holds no connection state, knows no `ConnectionId`, and has no opinion about
// what a payload means — `Link` owns the lifecycle and says what *should* be
// sent, this puts it on the wire and reports what arrived. Keeping the two apart
// is what makes the lifecycle testable without a socket, and it is why a
// transport can be swapped without touching a line of the state machine.
//
// **A caller cannot tell which implementation it holds**, exactly as with
// `parallel::Channel`. The loopback and the UDP socket answer the same calls
// with the same statuses, so `repo_layout.md` §16.6 is honest: single-player
// rides the loopback through **real encoding** — the same `Packet::Write`, the
// same bytes, the same `Packet::Read` — rather than through a shortcut that
// skips framing. A path only one configuration exercises is a path that breaks
// in the other one.
//
// **Never blocks, in either direction.** A tick occupies a job worker, so a send
// that waited for room would stall the world it runs in. A full send buffer
// refuses and says `Full`; the caller decides whether to drop it, retry next
// tick, or complain. Receive returns `Empty` rather than waiting for something to
// arrive, and a caller drains it in a loop until it does.
//
// **Delivery is not promised and no status implies it was.** `Ok` means the
// datagram left, not that it arrived — this is UDP underneath and the whole
// design above it assumes loss. So a datagram sent to an endpoint nobody is
// listening on is `Ok` on both implementations: the loopback drops it exactly as
// the socket would, because a loopback that reported a delivery failure would
// let single-player code branch on something the real network never says.
// `Unreachable` is the different case — an endpoint that cannot be addressed
// from here at all, which is a caller's mistake rather than a lost packet.
//
// **Every inbound datagram is hostile and an unknown sender is normal.** A port
// receives whatever is sent to it, so the endpoint a datagram came from is
// reported rather than filtered: deciding whether that sender is a connection, a
// new handshake or something to ignore belongs above, where the connection table
// is. An oversized datagram is dropped whole rather than truncated, because half
// a frame parses as a corrupt one.
//
// **No count of what is waiting.** `Channel` can answer that because it owns its
// queue; a socket cannot, and a figure that means "datagrams" on one
// implementation and "bytes of the next datagram" on the other is worse than no
// figure. Poll until `Empty` instead.
//
// **One owner, one thread.** Unlike `Channel`, a transport is not thread-safe: a
// socket does not survive two threads calling into it at once, and pretending
// otherwise here would make the loopback pass a test the socket fails. A
// transport is polled by whoever owns it.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Packet.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::net {

	// Why a send or receive did not happen.
	//
	// @since v0.3
	enum class TransportStatus : uint8_t {
		// The datagram was handed to the network, or one was received.
		//
		// **Not a delivery receipt.** Nothing under an unreliable transport can
		// promise that, and a status that read as one would be believed.
		Ok,

		// Nothing was waiting. Not an error: a transport polled every tick is
		// empty most ticks.
		Empty,

		// The send buffer is full. Refused rather than waited on, because a
		// blocking send stalls the tick that made it.
		Full,

		// This end is closed, or was never opened.
		Closed,

		// The datagram is larger than the maximum. Refused whole rather than
		// fragmented — a fragmented datagram is lost entirely when any one
		// fragment is, which multiplies the loss rate this design assumes small.
		TooLarge,

		// The endpoint cannot be addressed from here — no address, or the wrong
		// family for this socket. A caller's mistake, not a lost packet.
		Unreachable,
	};

	// Returns a stable, human-readable name for a transport status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(TransportStatus status);

	// One end of a datagram network.
	//
	// Abstract so the loopback and the socket are interchangeable at the call
	// site, which is the point rather than a nicety: single-player and a real
	// server differ in which one is constructed and in nothing else.
	//
	// @since v0.3
	class Transport {
	  public:
		// The largest datagram anything here will send or accept.
		//
		// A full packet: `Packet`'s header plus its maximum payload, which is
		// the 1200 bytes that survive the smallest path MTU in real use. Derived
		// from those rather than restated, so the transport cannot drift from
		// the framing it carries.
		static constexpr size_t MAXIMUM_DATAGRAM_BYTES = Packet::HEADER_BYTES + Packet::MAXIMUM_PAYLOAD_BYTES;

		virtual ~Transport() = default;

		// What a receive produced.
		struct Inbound {
			// Whether anything was received, and why not when nothing was.
			TransportStatus Status = TransportStatus::Empty;

			// Who sent it. Only meaningful when `Status` is `Ok`.
			//
			// **An unknown sender is not an error.** Anyone may send to an open
			// port, so this is reported rather than filtered and the connection
			// table above decides what the address means.
			Endpoint From;
		};

		// Sends one datagram, whole or not at all.
		//
		// Never blocks.
		//
		// @param to The destination. An endpoint nobody is listening on is `Ok`
		//        and dropped, because that is what the network does.
		// @param datagram The bytes to send. An empty datagram is legal and
		//        arrives as an empty one.
		// @return `Ok`, `Full`, `TooLarge`, `Unreachable`, or `Closed`.
		virtual TransportStatus Send(const Endpoint &to, std::span<const std::byte> datagram) = 0;

		// Takes the next datagram, if one is waiting.
		//
		// Never blocks, so a driver polls it at the barrier until it reports
		// `Empty` and then moves on. The vector is resized to the datagram and
		// keeps its capacity, so a caller reusing one across ticks stops
		// allocating.
		//
		// @param[out] datagram Filled with the bytes. Untouched unless the
		//        status is `Ok`.
		// @return `Ok` and the sender, `Empty`, or `Closed`.
		virtual Inbound Receive(std::vector<std::byte> &datagram) = 0;

		// The address this end receives on.
		//
		// Worth asking for even when the port was chosen: a port of zero binds
		// an ephemeral one, and this is the only way to learn which.
		//
		// @return The local endpoint, or an invalid one once closed.
		virtual Endpoint Local() const = 0;

		// Whether this end can still be used.
		//
		// @return `false` once closed.
		virtual bool Open() const = 0;

		// Closes this end. Anything still queued for it is dropped.
		//
		// Sends to a closed peer keep reporting `Ok` at the far side, because a
		// sender on a real network is not told that a port went away.
		virtual void Close() = 0;
	};

	// How a transport is sized.
	//
	// @since v0.3
	struct TransportSettings {
		// The largest datagram, clamped to `Transport::MAXIMUM_DATAGRAM_BYTES`.
		//
		// Lower it to test a small MTU. It cannot be raised: past that figure a
		// datagram fragments, and the loss rate stops being the one every budget
		// here was chosen against.
		size_t MaximumDatagram = Transport::MAXIMUM_DATAGRAM_BYTES;

		// Bytes that may wait to be received before a send is refused.
		//
		// The receive buffer, and on the socket it is set as exactly that. A
		// producer that outruns its consumer is bounded rather than allowed to
		// grow until the host dies, which is a crash a long way from its cause.
		size_t ReceiveQueueBytes = 256u * 1024u;
	};

	// Builds a loopback network with `peerCount` ends attached to it.
	//
	// **A routed network rather than a wired-up pair, and deliberately.** A pair
	// would make the destination argument a no-op — whatever you addressed, the
	// bytes would go to the one other end — and the two paths a socket has that a
	// pair cannot express are exactly the two this layer must get right: a
	// datagram from a sender this end has never heard of, and a datagram
	// addressed to somewhere nobody is listening. With routing, both are the same
	// code on both implementations and the suite runs the same cases over each.
	// It also costs nothing: single-player asks for two and uses them as a pair.
	//
	// Each end gets a synthetic address, `127.0.0.1:1` upwards, so `Local` and
	// the sender of a received datagram are real values a log can print.
	//
	// @param peerCount How many ends to attach. Clamped to 65535, since the
	//        numbering is a port.
	// @param settings How to size every end.
	// @return One transport per peer, in address order.
	// @since v0.3
	std::vector<std::unique_ptr<Transport>>
	MakeLoopbackTransport(size_t peerCount = 2, const TransportSettings &settings = {});

	// Binds a UDP socket on every IPv4 interface.
	//
	// Non-blocking, and polled rather than run on a thread of its own: an
	// asynchronous socket would deliver on somebody else's thread, which is the
	// one thing a tick that has to stay reproducible cannot have.
	//
	// @param port The port to bind, or zero for an ephemeral one. `Local` says
	//        which was chosen.
	// @param settings How to size the socket's buffers and its datagrams.
	// @return The transport, or nothing when the socket could not be created or
	//         the port could not be bound. A caller with no network gets a null
	//         rather than an exception, because "no socket" is an ordinary
	//         outcome on a machine that is offline or a port already in use.
	// @since v0.3
	std::unique_ptr<Transport> MakeUdpTransport(uint16_t port, const TransportSettings &settings = {});
}
