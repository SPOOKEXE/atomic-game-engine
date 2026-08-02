#pragma once

// What a connection cost, and what it lost.
//
// This is a userland-visible datatype: `Player:GetConnectionStats()` returns one
// — three layers for one call, which is what keeps `net` free of Luau. So the
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

		// Packets refused because they were not this protocol — a bad magic, an
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
		uint64_t PacketsStale = 0;

		// Sends refused this tick because the byte budget was spent.
		//
		// The number DATATYPES_LIBRARIES.md §15.1 asks to be visible. Without
		// it, an enforced budget and a congested link look identical from a
		// game's point of view.
		uint64_t SendsOverBudget = 0;

		// Gaps in the far side's sequence numbers — packets that never arrived.
		//
		// An estimate rather than a count, because a packet that is merely late
		// is indistinguishable from one that is lost until it either turns up or
		// does not.
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
