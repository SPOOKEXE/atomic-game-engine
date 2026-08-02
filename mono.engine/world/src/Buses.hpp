#pragma once

// The four bus backends, and the router that applies traffic to them.
//
// Private to this module. A bus is reached through `Postbox` from inside a
// world, or not at all — nothing outside `world` has any business holding the
// table a DataStore keeps.
//
// **Everything happens on the driver thread, at the barrier.** That is what
// makes ordering a property of the data rather than of the scheduler: the
// router walks every world's outbox in `(From, Sequence)` order and applies one
// operation at a time. Two runs of the same universe therefore apply the same
// operations in the same order, which is what a replay needs.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/world/Bus.hpp>

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

	// Everything the universe's buses hold.
	//
	// This is what `ROADMAP.md` calls "Universe Data": the backing state of the
	// four buses, and nothing else. There is no general-purpose shared world —
	// player data is a DataStore key, not a row in a world nobody owns.
	struct Buses {
		MessagingBus Messaging;
		MemoryBus Memory;
		DataBus Data;

		// Teleports waiting for their destination world to collect them, keyed
		// by destination world name id.
		std::unordered_map<uint32_t, std::vector<Delivery>> Teleports;
	};
}
