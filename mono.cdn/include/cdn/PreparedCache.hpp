#pragma once

// arch-waiver public-header: forward origin API. `Origin.hpp` owns this cache
// type as part of the complete content-serving contract.

// Groups that have already been compressed, so they are built once and streamed
// many times.
//
// Preparing a group is the origin's only real compute - hashing, chunking and
// compressing a set of assets - and it produces the same bytes every time.
// Doing it per request would make an origin's cost scale with its *popularity*
// rather than with its content, which is exactly backwards: the group everybody
// wants is the one that would be rebuilt most.
//
// **The key is the bundle root *and* the dictionary hash, and both halves are
// load-bearing.** A group compressed against one dictionary is a different
// artefact from the same group compressed against another, and serving the wrong
// one hands a client bytes it cannot decode. Keying on the bundle alone is the
// bug that only appears on the day a second dictionary exists.
//
// **A prepared frame is handed out as a shared pointer, not a view.** Eviction
// happens on whichever thread inserts, and a reader streaming a frame must not
// have it freed underneath. Ownership shared with the reader is the cheap answer;
// the alternative - a lock held for the length of a transfer - makes one slow
// client block every eviction in the origin.
//
// @tier shared

#include <engine/assets/ContentHash.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cdn {

	// What identifies a prepared group.
	struct PreparedKey {
		// The bundle this was built from.
		engine::assets::ContentHash Bundle;

		// The dictionary it was compressed against, or all-zero for none.
		engine::assets::ContentHash Dictionary;

		// Whether two keys name the same artefact.
		bool operator==(const PreparedKey &other) const = default;
	};

	// A prepared group's bytes, shared with everything currently streaming it.
	using PreparedFrame = std::shared_ptr<const std::vector<std::byte>>;

	// A bounded store of prepared groups, evicted least-recently-used.
	//
	// Thread-safe: an origin serves many requests at once and they all read this.
	//
	// @threadsafe
	class PreparedCache {
	  public:
		// What the cache is allowed to hold.
		//
		// Chosen rather than derived - whether this cache belongs in memory,
		// on disk or both is an open question, and this is the memory half
		// with no measurement behind its size yet.
		static constexpr uint64_t DEFAULT_CAPACITY_BYTES = 256ull * 1024 * 1024;

		// @param capacityBytes The most this may hold. Zero falls back to the
		//        default rather than making a cache that evicts everything it is
		//        given, which would look like a cache and behave like a leak of
		//        CPU.
		explicit PreparedCache(uint64_t capacityBytes = DEFAULT_CAPACITY_BYTES);

		// Looks a prepared group up and marks it recently used.
		//
		// @param key The bundle and dictionary.
		// @return The frame, or nullptr on a miss.
		PreparedFrame Find(const PreparedKey &key);

		// Stores a prepared group, evicting to make room.
		//
		// A frame larger than the whole capacity is refused rather than stored
		// by emptying the cache for it - one group that evicts everything else
		// on every insert is worse than not caching that group at all.
		//
		// Inserting a key that is already present replaces nothing and returns
		// the frame already there: two threads that prepared the same group
		// raced, and both results are byte-identical, so the first one wins and
		// the second is discarded.
		//
		// @param key The bundle and dictionary.
		// @param frame The compressed bytes.
		// @return The frame now in the cache, or nullptr when it was refused.
		PreparedFrame Insert(const PreparedKey &key, std::vector<std::byte> frame);

		// Whether a key is present, without marking it used.
		//
		// For a diagnostic or a test. Using this to decide whether to prepare
		// would be a check-then-act race - call Find.
		//
		// @param key The bundle and dictionary.
		// @return Whether it is cached.
		bool Contains(const PreparedKey &key) const;

		// How many bytes are held.
		uint64_t Bytes() const;

		// How many groups are held.
		size_t Count() const;

		// The capacity in use, after the zero fallback.
		uint64_t Capacity() const;

		// Discards everything.
		//
		// What a publish calls: the previous publication's groups were compressed
		// against content and a dictionary that are no longer current, and
		// keeping them wastes the capacity the new publication needs.
		void Clear();

	  private:
		struct Impl;

		// A pimpl, because the mutex and the eviction list are the whole
		// implementation and putting either in this header would make every
		// caller of the origin include a mutex.
		std::shared_ptr<Impl> State;
	};
}
