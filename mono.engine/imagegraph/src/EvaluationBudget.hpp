#pragma once

// Live logical payload reservations. Allocator bookkeeping and profiler overhead are separate counters.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>

namespace engine::imagegraph::detail {
	class AllocationReservation;
	template <class T> class EvaluationAllocator;
	class EvaluationBudget {
	  public:
		explicit EvaluationBudget(uint64_t limit) : MaximumBytes(limit) {}
		~EvaluationBudget() {
			assert(LiveBytes == 0);
		}
		EvaluationBudget(const EvaluationBudget &) = delete;
		EvaluationBudget &operator=(const EvaluationBudget &) = delete;
		uint64_t Available() const {
			return MaximumBytes - LiveBytes;
		}
		uint64_t Used() const {
			return LiveBytes;
		}
		uint64_t Peak() const {
			return PeakBytes;
		}
		[[nodiscard]] std::optional<AllocationReservation> Reserve(uint64_t bytes);

	  private:
		friend class AllocationReservation;
		template <class T> friend class EvaluationAllocator;
		uint64_t MaximumBytes, LiveBytes = 0, PeakBytes = 0;
		bool Acquire(uint64_t bytes) {
			if (bytes > Available()) return false;
			LiveBytes += bytes;
			PeakBytes = std::max(PeakBytes, LiveBytes);
			return true;
		}
		void Release(uint64_t bytes) {
			assert(bytes <= LiveBytes);
			LiveBytes -= bytes;
		}
	};

	// Declare this before its owned container so destruction frees the container before releasing its charge.
	class AllocationReservation {
	  public:
		AllocationReservation() = default;
		AllocationReservation(const AllocationReservation &) = delete;
		AllocationReservation &operator=(const AllocationReservation &) = delete;
		AllocationReservation(AllocationReservation &&other) noexcept
			: Ledger(std::exchange(other.Ledger, nullptr)),
			  ReservedBytes(std::exchange(other.ReservedBytes, 0)) {}
		AllocationReservation &operator=(AllocationReservation &&other) noexcept {
			if (this != &other) {
				Reset();
				Ledger = std::exchange(other.Ledger, nullptr);
				ReservedBytes = std::exchange(other.ReservedBytes, 0);
			}
			return *this;
		}
		~AllocationReservation() {
			Reset();
		}
		uint64_t Bytes() const {
			return ReservedBytes;
		}
		// Grow before reserve/resize/copy; shrink only after freeing the corresponding owned storage.
		// Refusal leaves both the old charge and the container untouched.
		[[nodiscard]] bool Resize(uint64_t bytes) {
			if (!Ledger) return bytes == 0;
			if (bytes > ReservedBytes) {
				if (!Ledger->Acquire(bytes - ReservedBytes)) return false;
			} else
				Ledger->Release(ReservedBytes - bytes);
			ReservedBytes = bytes;
			return true;
		}
		[[nodiscard]] bool Merge(AllocationReservation &&other) {
			if (this == &other) return true;
			if (!other.Ledger) return true;
			if (!Ledger) {
				*this = std::move(other);
				return true;
			}
			if (Ledger != other.Ledger) return false;
			// Both reservations already belong to LiveBytes, whose sum cannot exceed the ledger limit.
			ReservedBytes += other.ReservedBytes;
			other.Ledger = nullptr;
			other.ReservedBytes = 0;
			return true;
		}
		[[nodiscard]] std::optional<AllocationReservation> Split(uint64_t bytes) {
			if (!Ledger || bytes > ReservedBytes) return std::nullopt;
			ReservedBytes -= bytes;
			return AllocationReservation(*Ledger, bytes);
		}
		void Reset() {
			if (Ledger) Ledger->Release(ReservedBytes);
			Ledger = nullptr;
			ReservedBytes = 0;
		}

	  private:
		friend class EvaluationBudget;
		EvaluationBudget *Ledger = nullptr;
		uint64_t ReservedBytes = 0;
		AllocationReservation(EvaluationBudget &budget, uint64_t bytes)
			: Ledger(&budget), ReservedBytes(bytes) {}
	};
	inline std::optional<AllocationReservation> EvaluationBudget::Reserve(uint64_t bytes) {
		if (!Acquire(bytes)) return std::nullopt;
		return AllocationReservation(*this, bytes);
	}
}
