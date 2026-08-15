#pragma once

// What a connection cost, and what it lost.
//
// This is a userland-visible datatype: `Player:GetConnectionStats()` returns one
// - three layers for one call, which is what keeps `net` free of Luau. So the
// field names here are the ones a game developer reads, and renaming one is a
// breaking change to a scripting surface rather than an internal tidy-up.
//
// **The overflow counters are the reason this type is not optional.**
// DATATYPES_LIBRARIES.md §15.1 asks for a per-player byte budget "enforced, with
// the overflow visible in `ConnectionStats` rather than as a mystery stall". A
// budget that silently drops traffic is indistinguishable from a network that
// silently drops traffic, and the two want completely different fixes.
//
// @tier L11 · shared

#include <cstdint>

namespace engine::net {

	// One connection's running totals.
	//
	// @since v0.3
	struct ConnectionStats {
		// Payload bytes accepted from the far side, over the connection's life.
		uint64_t BytesReceived = 0;

		// Payload bytes handed to the transport, over the connection's life.
		uint64_t BytesSent = 0;

		// Packets accepted from the far side.
		uint64_t PacketsReceived = 0;

		// Packets handed to the transport.
		uint64_t PacketsSent = 0;

		// Packets refused because they were not this protocol - a bad magic, an
		// unknown version, a length that contradicted the frame.
		//
		// Counted apart from loss. A rate that climbs here is somebody probing
		// the port or two builds disagreeing about the format, and neither reads
		// anything like a lossy network.
		uint64_t PacketsRefused = 0;

		// Unreliable packets discarded because a newer one had already arrived.
		//
		// **Not a fault.** This is the unreliable channel doing its job: a
		// position update that arrives after a newer one would move the world
		// backwards. A rate near zero on a real network is the surprising
		// reading, not a high one.
		//
		// Judged against the unreliable channel's own high-water mark, which is
		// the only sequence space this counter has ever claimed to be about. It
		// used to be judged against a mark the reliable channel could move, and
		// the packets that inflated it were not stale at all.
		uint64_t PacketsStale = 0;

		// Sends refused because a *configured* budget was spent.
		//
		// The number DATATYPES_LIBRARIES.md §15.1 asks to be visible. Without
		// it, an enforced budget and a congested link look identical from a
		// game's point of view.
		//
		// The byte budget, the packet budget and a payload too large to frame -
		// every reason that is a number somebody chose, and therefore every
		// reason a caller can answer by choosing differently. Congestion is
		// `SendsOverAllowance` and is not counted here.
		uint64_t SendsOverBudget = 0;

		// Sends refused because the *congestion controller* would not carry them.
		//
		// **Kept apart from `SendsOverBudget` on purpose, and the distinction is
		// the whole reading.** That one is a number somebody configured being
		// enforced, and the answer to it is to raise the number or send less.
		// This one is the path saying it cannot take the traffic, and raising
		// anything does nothing at all. A single counter would make "raise the
		// cap" look like a fix for congestion.
		//
		// **A caller that must not lose the send has to read both.** Either off
		// zero means the payload did not go; `replication::Authority::Unsent` is
		// the reader that knows what that costs.
		//
		// @since v0.15
		uint64_t SendsOverAllowance = 0;

		// Reliable packets this end sent that the far side's acknowledgement
		// showed missing.
		//
		// The outbound twin of `PacketsLost`, and the two must not be added
		// together: that one is about the path coming in and this one is about
		// the path going out, and on an asymmetric link they routinely disagree.
		// This is the loss signal the congestion controller acts on, because it
		// is the only one that is about the direction this end is sending in.
		//
		// **Only the reliable channel is visible here**, since it is the only
		// one the far side acknowledges. Unreliable loss on the way out is not
		// reported by anything and is not measured by this.
		//
		// @since v0.15
		uint64_t SendsLost = 0;

		// Payload bytes the congestion controller will carry this tick.
		//
		// The lower of what the path looks able to take and
		// `LinkSettings::BytesPerTick`. Read it beside `SendsOverAllowance`:
		// a number well under the cap with refusals against it is a link doing
		// exactly what it should on a path that cannot take more.
		//
		// @since v0.15
		uint32_t SendAllowanceBytes = 0;

		// Standing queue the path looks to be holding, in milliseconds.
		//
		// The round trip now, less the least this connection has ever seen. It
		// is the number the whole controller is built around: a delay-based
		// algorithm settles this at a couple of packets' worth, where a
		// loss-based one settles it at however deep the bottleneck's buffer is.
		//
		// @since v0.15
		float QueueMilliseconds = 0.0f;

		// How much the round trip moves about, in milliseconds.
		//
		// RFC 6298's `RTTVAR`, and what a player interface should show as
		// jitter. `RoundTripMilliseconds` alone says nothing about whether the
		// next packet will be near it.
		//
		// @since v0.15
		float RoundTripVarianceMilliseconds = 0.0f;

		// Gaps in the far side's sequence numbers - packets that never arrived.
		//
		// An estimate rather than a count, because a packet that is merely late
		// is indistinguishable from one that is lost until it either turns up or
		// does not.
		//
		// Each channel's gaps are found in that channel's own numbering and
		// summed here. A gap measured across two counters that advance at
		// different rates is not a gap.
		uint64_t PacketsLost = 0;

		// Round trip in milliseconds, smoothed.
		//
		// Smoothed rather than instantaneous because a single sample includes
		// whatever the far side happened to be doing, and a number that jumps by
		// 40 ms between two reads is a number a player interface cannot show.
		float RoundTripMilliseconds = 0.0f;

		// Seconds since anything at all arrived from the far side.
		//
		// What the idle timeout is measured against, and what a client shows as
		// "connection lost" before it is certain.
		float SinceLastReceiveSeconds = 0.0f;
	};
}
