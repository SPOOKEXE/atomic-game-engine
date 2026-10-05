#pragma once

#include "EvaluationAllocator.hpp"

#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>

namespace engine::imagegraph::detail {
	inline const Value *SourceFrameCacheStoredFrame(const DataReplayEntry *row, uint64_t tick) {
		if (!row || row->Values.size() < 2 || tick > UINT64_MAX - 2) return nullptr;
		const auto key = tick + 2;
		const auto position = std::lower_bound(
			row->Values.begin() + 2, row->Values.end(), key, [](const auto &frame, uint64_t index) {
				return frame.Frame < index;
			}
		);
		return position != row->Values.end() && position->Frame == key ? &position->Data : nullptr;
	}
	inline const Value *SourceFrameCacheExistingFrame(const DataReplayEntry *row, uint64_t tick) {
		const auto *value = SourceFrameCacheStoredFrame(row, tick);
		return value && (std::holds_alternative<SurfaceValue>(*value) ||
						 std::holds_alternative<ArrayValue>(*value))
				   ? value
				   : nullptr;
	}
	// Borrowed rows are indexed once. No pixel or value copy is needed to cut unread getters.
	struct SourceFrameCacheInputIndex {
		EvaluationVector<const DataReplayEntry *> Rows;
		SourceFrameCacheInputIndex(const DataReplayState *state, EvaluationBudget &budget)
			: Rows(EvaluationAllocator<const DataReplayEntry *>(budget)) {
			if (!state || state->Entries.size() > Limits::MaximumArrayElements) return;
			Rows.reserve(state->Entries.size());
			for (const auto &row : state->Entries)
				Rows.push_back(&row);
			std::sort(Rows.begin(), Rows.end(), [](const auto *left, const auto *right) {
				return left->NodeId < right->NodeId;
			});
		}
		const DataReplayEntry *Find(const Node &node, bool &constructorCleared) const {
			const auto first = std::lower_bound(
				Rows.begin(), Rows.end(), node.Id, [](const auto *row, const std::string &id) {
					return row->NodeId < id;
				}
			);
			const auto saved = SourceFrameCacheSavedText(node);
			const DataReplayEntry *result = nullptr;
			for (auto position = first; position != Rows.end() && (*position)->NodeId == node.Id;
				 ++position) {
				const auto *row = *position;
				if (SourceFrameCacheRowType(*row) != node.Type || row->LoadedCacheData != saved) continue;
				constructorCleared |= row->FrameCacheConstructorCleared;
				if (row->ProcessorRow == 0) result = row;
			}
			return result;
		}
	};
	inline bool SourceFrameCacheKnownHit(
		const Node &node,
		const EvaluationRequest &request,
		const SourceFrameCacheInputIndex &current,
		const SourceFrameCacheInputIndex &loads,
		uint64_t totalFrames
	) {
		if (node.Type != "pc.cache" || request.NegativeFrame || request.Subframe != 0) return false;
		bool cleared = false;
		const auto *row = current.Find(node, cleared);
		if (!row && !cleared && !SourceFrameCacheSavedText(node).empty()) {
			bool ignored = false;
			row = loads.Find(node, ignored);
			if (request.Tick >= totalFrames || (row && (row->NegativeFrame || row->Subframe != 0)))
				return false;
		}
		return SourceFrameCacheExistingFrame(row, request.Tick) != nullptr;
	}
}
