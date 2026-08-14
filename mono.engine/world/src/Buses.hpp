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

		// How many channels each world holds, by world index. Absent is zero, so
		// a world that holds none costs no entry.
		//
		// **Counted rather than derived, and the four methods below are the only
		// things that touch either half.** Deriving it means walking every
		// channel in the universe per open, which is the cost the cap exists to
		// bound turned into the cost of enforcing it; keeping it beside the table
		// is a second copy of one fact, which is rule 2 — so the copy is private
		// to this struct and nothing outside can move one half without the other.
		std::unordered_map<uint32_t, uint32_t> Held;

		// How many channels a world holds.
		//
		// @param world The world's registry index.
		uint32_t HeldBy(uint32_t world) const {
			const auto found = Held.find(world);
			return found == Held.end() ? 0 : found->second;
		}

		// Opens `channel` for `world`, unless that would pass `limit`.
		//
		// Reopening a channel a world already holds is not refused at the cap: it
		// is idempotent and changes nothing, and refusing it would make a world's
		// own repeated open fail once its list was full.
		//
		// @param channel The channel name's id.
		// @param world   The world's registry index.
		// @param limit   `UniverseSettings::ChannelsPerWorld`.
		// @return `false` when the world is at the limit and this is a new
		//         channel for it. Nothing is inserted in that case — an empty
		//         entry left behind by a refused open would grow the table the
		//         limit exists to bound.
		bool OpenFor(uint32_t channel, uint32_t world, uint32_t limit) {
			const auto listening = Open.find(channel);
			if (listening != Open.end() && listening->second.contains(world)) {
				return true;
			}
			if (HeldBy(world) >= limit) {
				return false;
			}

			Open[channel].insert(world);
			Held[world]++;
			return true;
		}

		// Closes `channel` for `world`. Closing one it does not hold does
		// nothing.
		//
		// @param channel The channel name's id.
		// @param world   The world's registry index.
		void CloseFor(uint32_t channel, uint32_t world) {
			const auto listening = Open.find(channel);
			if (listening == Open.end() || listening->second.erase(world) == 0) {
				return;
			}

			const auto held = Held.find(world);
			if (held != Held.end() && --held->second == 0) {
				// Erased rather than left at zero, so a universe that opens and
				// closes channels all day does not grow one entry per world that
				// ever held one.
				Held.erase(held);
			}
		}

		// Rebuilds `Held` from `Open`.
		//
		// For the snapshot reader, which fills the table directly because the
		// channels and the topics are the same shape and share one codec. Without
		// it a restored universe would let every world open the cap again on top
		// of what it already holds.
		void Recount() {
			Held.clear();
			for (const auto &entry : Open) {
				for (const uint32_t world : entry.second) {
					Held[world]++;
				}
			}
		}
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
