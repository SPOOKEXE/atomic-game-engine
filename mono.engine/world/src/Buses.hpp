#pragma once

// The bus backends: the tables a MessagingService, a MemoryStore, a DataStore
// and the named channels are actually kept in.
//
// Private to this module. A bus is reached through `Postbox` from inside a
// world, or not at all — nothing outside `world` has any business holding the
// table a DataStore keeps.
//
// Storage and nothing else. What applies traffic to these, and in what order,
// is `BusRouter.hpp`.
//
// @tier L4 · shared

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::world {

	// Publish and subscribe over named topics.
	struct MessagingBus {
		// Topic name id to the worlds listening on it.
		std::unordered_map<uint32_t, std::unordered_set<uint32_t>> Subscribers;
	};

	// Ephemeral shared state: values and queues, gone when the run ends.
	struct MemoryBus {
		std::unordered_map<uint32_t, std::vector<std::byte>> Values;

		// A deque per queue, so a matchmaker popping from the front is O(1) and
		// several worlds popping the same queue in one barrier each get a
		// different entry.
		std::unordered_map<uint32_t, std::deque<std::vector<std::byte>>> Queues;
	};

	// Durable key/value with versions.
	//
	// **The backing is in memory and that is a v0.2 limitation, not a design.**
	// The interface, the versioning and the read-modify-write conflict are all
	// real; what is missing is the disk underneath, which lands with the
	// filesystem work. Said plainly here so nobody ships a save on top of a
	// hash map.
	struct DataBus {
		struct Record {
			std::vector<std::byte> Value;

			// Bumped on every write. An Update carrying a stale version is
			// refused rather than silently overwriting whatever landed between
			// the caller's read and its write.
			uint64_t Version = 0;
		};

		std::unordered_map<uint32_t, Record> Records;
	};

	// Which worlds are listening on which named channel.
	//
	// **Membership and nothing else, which is why an unordered container is safe
	// here and is not on `Messaging`.** A publish walks its subscriber set and
	// the walk order decides who is delivered to first; a channel send names one
	// world and asks whether it opened the channel, so the only thing ever read
	// out of this is a `contains`. Nothing about a run depends on how it iterates.
	//
	// @since v0.17
	struct ChannelBus {
		// Channel name id to the worlds that opened it.
		std::unordered_map<uint32_t, std::unordered_set<uint32_t>> Open;
	};

	// Everything the universe's buses hold.
	//
	// This is what `ROADMAP.md` calls "Universe Data": the backing state of the
	// buses, and nothing else. There is no general-purpose shared world — player
	// data is a DataStore key, not a row in a world nobody owns.
	//
	// **A teleport has no table here and does not want one.** It is routed to a
	// destination in the same barrier it was posted in, so there is nothing to
	// hold between two of them; a map of pending teleports sat in this struct from
	// v0.2 with no reader anywhere in the engine, which reads as a queue somebody
	// forgot to drain rather than as a routing step that never needed one.
	struct Buses {
		MessagingBus Messaging;
		MemoryBus Memory;
		DataBus Data;
		ChannelBus Channels;
	};
}
