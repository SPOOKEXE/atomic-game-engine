#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace engine::imagegraphio::detail {
	// One import reconciliation owns its candidate shadow and all temporary allocations.
	// Payload holds are conservative until the operation ends; allocator blocks release on destruction.
	class ImportBudget {
	  public:
		explicit ImportBudget(uint64_t limit) : Limit(limit) {}
		bool Hold(uint64_t bytes) {
			if (bytes > Limit - Live) {
				Rejected = true;
				return false;
			}
			Live += bytes;
			return true;
		}
		void Release(uint64_t bytes) {
			Live -= bytes;
		}
		void Reject() {
			Rejected = true;
		}
		bool Exceeded() const {
			return Rejected;
		}
		uint64_t Available() const {
			return Limit - Live;
		}

	  private:
		uint64_t Limit, Live = 0;
		bool Rejected = false;
	};
	template <class T> struct ImportAllocator {
		using value_type = T;
		ImportBudget *Budget;
		explicit ImportAllocator(ImportBudget &budget) : Budget(&budget) {}
		template <class U> ImportAllocator(const ImportAllocator<U> &other) : Budget(other.Budget) {}
		T *allocate(size_t count) {
			if (count > UINT64_MAX / sizeof(T) || !Budget->Hold(count * sizeof(T))) throw std::bad_alloc{};
			try {
				return std::allocator<T>{}.allocate(count);
			} catch (...) {
				Budget->Release(count * sizeof(T));
				throw;
			}
		}
		void deallocate(T *pointer, size_t count) {
			std::allocator<T>{}.deallocate(pointer, count);
			Budget->Release(count * sizeof(T));
		}
		template <class U> bool operator==(const ImportAllocator<U> &other) const {
			return Budget == other.Budget;
		}
	};
	template <class T> using ImportVector = std::vector<T, ImportAllocator<T>>;
}
