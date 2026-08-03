#include "ChunkPool.hpp"

#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	namespace {
		// One freelist's identity. Both halves matter: an aligned `operator
		// delete` is undefined with the wrong alignment, and handing a 12 KiB
		// chunk to a column that asked for 16 KiB is a heap overrun that no test
		// would name.
		struct SizeClass {
			size_t Bytes = 0;
			size_t Alignment = 0;

			bool operator==(const SizeClass &other) const = default;
		};

		struct SizeClassHash {
			size_t operator()(const SizeClass &sizeClass) const {
				// FNV's prime, mixed the same way `ComponentSet` hashes its ids.
				// The two fields are small and correlated, so xor alone would
				// collide every 12-byte-stride class with every 12-aligned one.
				return (sizeClass.Bytes * 1099511628211ull) ^ (sizeClass.Alignment * 2654435761ull);
			}
		};

		// The pool's whole state, behind one lock.
		//
		// One mutex rather than a shard per size class, because of how rarely it
		// is taken: chunks double in size, so a column of a million rows acquires
		// seventeen of them over its whole life and releases them the same way.
		// An uncontended lock amortises to nothing against that, and sharding
		// would be machinery bought with no measurement behind it.
		struct PoolState {
			std::mutex Guard;
			std::unordered_map<SizeClass, std::vector<void *>, SizeClassHash> Free;
			size_t Retained = 0;

			// Outside the lock so that a diagnostic read never contends with the
			// path it is measuring.
			std::atomic<uint64_t> Allocated{0};
			std::atomic<uint64_t> Reused{0};
		};

		// Function-local rather than a namespace static: initialisation order
		// across translation units is unspecified.
		//
		// **And never destroyed, which is the part that is not decoration.** The
		// pool is constructed on the first chunk anybody asks for, which is
		// *later* than the static that owns the store doing the asking — so
		// reverse destruction order tears the pool down first and every column
		// destroyed after it releases into freed memory. That is a
		// use-after-free at exit, it depends on which static was touched first,
		// and it showed up as a benchmark binary that produced its whole report
		// and then failed. A process-lifetime singleton that outlives every
		// other static is the fix; the memory is reclaimed by the process.
		PoolState &Pool() {
			static PoolState *state = new PoolState();
			return *state;
		}
	}

	void *ChunkPool::Acquire(size_t bytes, size_t alignment) {
		PoolState &pool = Pool();

		{
			const std::lock_guard<std::mutex> held(pool.Guard);
			const auto found = pool.Free.find(SizeClass{bytes, alignment});
			if (found != pool.Free.end() && !found->second.empty()) {
				// Last in, first out: the chunk most recently released is the one
				// most likely to still be in cache, and a world oscillating
				// across a boundary gets its own chunk straight back.
				void *chunk = found->second.back();
				found->second.pop_back();
				pool.Retained -= bytes;
				pool.Reused.fetch_add(1, std::memory_order_relaxed);
				return chunk;
			}
		}

		// Outside the lock. An allocation can be slow and can take a lock of its
		// own, and holding one across a call into another subsystem is how a
		// deadlock gets built out of two reasonable pieces of code.
		pool.Allocated.fetch_add(1, std::memory_order_relaxed);
		return ::operator new(bytes, std::align_val_t(alignment));
	}

	void ChunkPool::Release(void *chunk, size_t bytes, size_t alignment) {
		if (chunk == nullptr) {
			return;
		}

		{
			PoolState &pool = Pool();
			const std::lock_guard<std::mutex> held(pool.Guard);
			if (pool.Retained + bytes <= RETAINED_BYTES_CAP) {
				pool.Free[SizeClass{bytes, alignment}].push_back(chunk);
				pool.Retained += bytes;
				return;
			}
		}

		// Over the cap, so this one goes back rather than being kept under a
		// different name. Outside the lock, as in Acquire.
		::operator delete(chunk, std::align_val_t(alignment));
	}

	void ChunkPool::Trim() {
		std::unordered_map<SizeClass, std::vector<void *>, SizeClassHash> taken;

		{
			PoolState &pool = Pool();
			const std::lock_guard<std::mutex> held(pool.Guard);
			taken.swap(pool.Free);
			pool.Retained = 0;
		}

		for (const auto &[sizeClass, chunks] : taken) {
			for (void *chunk : chunks) {
				::operator delete(chunk, std::align_val_t(sizeClass.Alignment));
			}
		}
	}

	size_t ChunkPool::RetainedBytes() {
		PoolState &pool = Pool();
		const std::lock_guard<std::mutex> held(pool.Guard);
		return pool.Retained;
	}

	uint64_t ChunkPool::Allocations() {
		return Pool().Allocated.load(std::memory_order_relaxed);
	}

	uint64_t ChunkPool::Reuses() {
		return Pool().Reused.load(std::memory_order_relaxed);
	}
}
