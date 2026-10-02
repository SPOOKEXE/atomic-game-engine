#pragma once

#include "EvaluationBudget.hpp"

#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::imagegraph::detail {
	// Private scratch containers admit their actual allocation requests, including rehash overlap.
	// Their allocator carries one explicit ledger; borrowed payloads and allocator overhead are excluded.
	template <class T> class EvaluationAllocator {
	  public:
		using value_type = T;
		using propagate_on_container_move_assignment = std::true_type;
		using propagate_on_container_swap = std::true_type;
		using is_always_equal = std::false_type;
		EvaluationAllocator() = delete;
		explicit EvaluationAllocator(EvaluationBudget &budget) noexcept : Ledger(&budget) {}
		template <class U>
		EvaluationAllocator(const EvaluationAllocator<U> &other) noexcept : Ledger(other.Ledger) {}
		[[nodiscard]] T *allocate(size_t count) {
			if (count > std::numeric_limits<uint64_t>::max() / sizeof(T)) throw std::bad_array_new_length{};
			const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(T);
			if (!Ledger->Acquire(bytes)) throw std::bad_alloc{};
			try {
				return std::allocator<T>{}.allocate(count);
			} catch (...) {
				Ledger->Release(bytes);
				throw;
			}
		}
		void deallocate(T *storage, size_t count) noexcept {
			std::allocator<T>{}.deallocate(storage, count);
			Ledger->Release(static_cast<uint64_t>(count) * sizeof(T));
		}
		template <class U> bool operator==(const EvaluationAllocator<U> &other) const noexcept {
			return Ledger == other.Ledger;
		}

	  private:
		template <class U> friend class EvaluationAllocator;
		EvaluationBudget *Ledger;
	};
	template <class T> using EvaluationVector = std::vector<T, EvaluationAllocator<T>>;
	template <class T> using EvaluationSet = std::set<T, std::less<T>, EvaluationAllocator<T>>;
	template <class K, class V>
	using EvaluationMap = std::map<K, V, std::less<K>, EvaluationAllocator<std::pair<const K, V>>>;
	template <class T>
	using EvaluationHashSet = std::unordered_set<T, std::hash<T>, std::equal_to<T>, EvaluationAllocator<T>>;
	template <class K, class V>
	using EvaluationHashMap =
		std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, EvaluationAllocator<std::pair<const K, V>>>;
	template <class T> auto MakeEvaluationSet(EvaluationBudget &budget) {
		return EvaluationSet<T>(std::less<T>{}, EvaluationAllocator<T>(budget));
	}
	template <class K, class V> auto MakeEvaluationMap(EvaluationBudget &budget) {
		return EvaluationMap<K, V>(std::less<K>{}, EvaluationAllocator<std::pair<const K, V>>(budget));
	}
	template <class T> auto MakeEvaluationHashSet(EvaluationBudget &budget) {
		return EvaluationHashSet<T>(0, std::hash<T>{}, std::equal_to<T>{}, EvaluationAllocator<T>(budget));
	}
	template <class K, class V> auto MakeEvaluationHashMap(EvaluationBudget &budget) {
		return EvaluationHashMap<K, V>(
			0, std::hash<K>{}, std::equal_to<K>{}, EvaluationAllocator<std::pair<const K, V>>(budget)
		);
	}

}
