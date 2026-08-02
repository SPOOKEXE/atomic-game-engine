#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <cdn/PreparedCache.hpp>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace cdn {

	namespace {
		using engine::assets::ContentHash;

		struct KeyHash {
			size_t operator()(const PreparedKey &key) const noexcept {
				// The digests are already uniformly distributed, so the first
				// eight bytes of each are as good a hash as anything derived
				// from all sixty-four — and combining the two halves matters
				// because one dictionary and many bundles is the ordinary case.
				size_t bundle = 0;
				size_t dictionary = 0;
				for (size_t index = 0; index < sizeof(size_t); ++index) {
					bundle = (bundle << 8) | key.Bundle.Digest[index];
					dictionary = (dictionary << 8) | key.Dictionary.Digest[index];
				}
				return bundle ^ (dictionary * 0x9E3779B97F4A7C15ull);
			}
		};
	}

	struct PreparedCache::Impl {
		mutable std::mutex Guard;

		// Most recently used at the front. A list rather than a heap because
		// eviction only ever takes from one end and a lookup only ever moves one
		// node, and both are O(1) on a list with the iterator in hand.
		std::list<PreparedKey> Recency;

		struct Entry {
			PreparedFrame Frame;
			std::list<PreparedKey>::iterator Position;
		};

		std::unordered_map<PreparedKey, Entry, KeyHash> Entries;

		uint64_t Held = 0;
		uint64_t Capacity = 0;
	};

	PreparedCache::PreparedCache(uint64_t capacityBytes) : State(std::make_shared<Impl>()) {
		// Zero would make a cache that evicts everything it is given — something
		// that looks like a cache and behaves like a leak of CPU.
		State->Capacity = capacityBytes == 0 ? DEFAULT_CAPACITY_BYTES : capacityBytes;
	}

	PreparedFrame PreparedCache::Find(const PreparedKey &key) {
		ENGINE_PROFILE("PreparedCache::Find");

		const std::lock_guard<std::mutex> held(State->Guard);

		const auto found = State->Entries.find(key);
		if (found == State->Entries.end()) {
			engine::core::Metrics::Count("cdn.prepared.miss", 1.0);
			return nullptr;
		}

		State->Recency.splice(State->Recency.begin(), State->Recency, found->second.Position);
		engine::core::Metrics::Count("cdn.prepared.hit", 1.0);
		return found->second.Frame;
	}

	PreparedFrame PreparedCache::Insert(const PreparedKey &key, std::vector<std::byte> frame) {
		ENGINE_PROFILE("PreparedCache::Insert");

		const uint64_t size = frame.size();

		const std::lock_guard<std::mutex> held(State->Guard);

		// One group that evicts everything else on every insert is worse than
		// not caching that group at all.
		if (size > State->Capacity) {
			engine::core::Metrics::Count("cdn.prepared.refused", 1.0);
			return nullptr;
		}

		const auto existing = State->Entries.find(key);
		if (existing != State->Entries.end()) {
			// Two threads prepared the same group and raced. Both results are
			// byte-identical — preparation is deterministic — so the first one
			// wins and the second is discarded rather than churning the entry
			// and invalidating whatever is streaming it.
			State->Recency.splice(State->Recency.begin(), State->Recency, existing->second.Position);
			return existing->second.Frame;
		}

		while (State->Held + size > State->Capacity && !State->Recency.empty()) {
			const PreparedKey &oldest = State->Recency.back();
			const auto victim = State->Entries.find(oldest);
			if (victim != State->Entries.end()) {
				State->Held -= victim->second.Frame->size();
				// The frame itself survives as long as anything is streaming it:
				// the shared pointer a reader took keeps the bytes alive past
				// this erase.
				State->Entries.erase(victim);
				engine::core::Metrics::Count("cdn.prepared.evicted", 1.0);
			}
			State->Recency.pop_back();
		}

		auto stored = std::make_shared<const std::vector<std::byte>>(std::move(frame));

		State->Recency.push_front(key);
		State->Entries.emplace(key, Impl::Entry{stored, State->Recency.begin()});
		State->Held += size;

		engine::core::Metrics::Count("cdn.prepared.stored", 1.0);
		return stored;
	}

	bool PreparedCache::Contains(const PreparedKey &key) const {
		const std::lock_guard<std::mutex> held(State->Guard);
		return State->Entries.find(key) != State->Entries.end();
	}

	uint64_t PreparedCache::Bytes() const {
		const std::lock_guard<std::mutex> held(State->Guard);
		return State->Held;
	}

	size_t PreparedCache::Count() const {
		const std::lock_guard<std::mutex> held(State->Guard);
		return State->Entries.size();
	}

	uint64_t PreparedCache::Capacity() const {
		const std::lock_guard<std::mutex> held(State->Guard);
		return State->Capacity;
	}

	void PreparedCache::Clear() {
		const std::lock_guard<std::mutex> held(State->Guard);
		State->Entries.clear();
		State->Recency.clear();
		State->Held = 0;
	}
}
