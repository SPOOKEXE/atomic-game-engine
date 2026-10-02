#pragma once

// Current-call owned audio may move only after every fallible snapshot
// construction has finished.
#include "EvaluationBudget.hpp"
#include "ValuePayload.hpp"

#include <exception>
#include <limits>
#include <span>
#include <type_traits>

namespace engine::imagegraph::detail {
	struct SnapshotAudioMove {
		AudioBit *Source = nullptr;
		AudioBit *Target = nullptr;
		AllocationReservation *Charge = nullptr;
		uint64_t Bytes = 0;
	};

	// Zero-byte probes establish ledger compatibility before any positive ownership
	// transfer. Validate each result's total, not merely each independently
	// affordable payload.
	inline bool
	PrepareSnapshotAudioMoves(std::span<const SnapshotAudioMove> moves, AllocationReservation &destination) {
		for (size_t i = 0; i < moves.size(); ++i) {
			const auto &move = moves[i];
			if (!move.Source || !move.Target || move.Source == move.Target || !move.Charge ||
				move.Bytes == 0 || move.Bytes != RetainedPayloadBytes(*move.Source) ||
				RetainedPayloadBytes(*move.Target) != 0)
				return false;
			for (size_t j = 0; j < i; ++j)
				if (moves[j].Source == move.Source || moves[j].Target == move.Target) return false;
			for (const auto &candidate : moves)
				if (candidate.Source == move.Target) return false;
			bool first = true;
			for (size_t j = 0; j < i; ++j)
				if (moves[j].Charge == move.Charge) first = false;
			if (!first) continue;
			uint64_t total = 0;
			for (const auto &candidate : moves)
				if (candidate.Charge == move.Charge) {
					if (candidate.Bytes > std::numeric_limits<uint64_t>::max() - total) return false;
					total += candidate.Bytes;
				}
			if (total > move.Charge->Bytes()) return false;
			auto probe = move.Charge->Split(0);
			if (!probe || !destination.Merge(std::move(*probe))) return false;
		}
		return true;
	}

	// Prepare must succeed after all allocations, with source/target/charge
	// unchanged until this commit.
	inline void CommitSnapshotAudioMoves(
		std::span<const SnapshotAudioMove> moves, AllocationReservation &destination
	) noexcept {
		static_assert(std::is_nothrow_move_assignable_v<AudioBit>);
		for (const auto &move : moves) {
			auto transfer = move.Charge->Split(move.Bytes);
			if (!transfer || !destination.Merge(std::move(*transfer))) std::terminate();
			*move.Target = std::move(*move.Source);
		}
	}
} // namespace engine::imagegraph::detail
