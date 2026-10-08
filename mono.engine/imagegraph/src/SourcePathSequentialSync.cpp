#include "SourcePathShiftVisit.hpp"

namespace engine::imagegraph::detail {
	bool SyncSourcePathSequentialValue(
		SourcePathShiftMemo &memo,
		Value &value,
		EvaluationBudget &budget,
		AllocationReservation &charge,
		Diagnostic &diagnostic
	) {
		SourcePathShiftRoute route;
		auto synchronize = [&](SourcePathData2D &operation, const SourcePathShiftRoute &) {
			if (operation.Wave && operation.EvaluationMemoId) {
				if (operation.EvaluationMemoId > memo.Owners.size()) {
					diagnostic = {
						Status::InvalidValue, {}, "path", "Source Wave publication lacks current owner"
					};
					return false;
				}
				const auto &owner = memo.Owners[operation.EvaluationMemoId - 1];
				if (!owner.WaveInitialized) return true;
				auto &state = *operation.Wave;
				size_t count = 0;
				for (const auto &entry : memo.Entries)
					count += entry.OwnerId == operation.EvaluationMemoId;
				bool same = state.Cache.size() == count;
				size_t index = 0;
				for (const auto &entry : memo.Entries) {
					if (entry.OwnerId != operation.EvaluationMemoId) continue;
					const SourcePathSequentialCachePoint point{
						entry.Coordinate,
						uint32_t(entry.Line),
						{{entry.Point.X, entry.Point.Y}, entry.Point.Weight}
					};
					if (same && state.Cache[index] != point) same = false;
					++index;
				}
				if (!same) {
					auto admission = budget.Reserve(count * sizeof(SourcePathSequentialCachePoint));
					if (!admission) {
						diagnostic = {
							Status::LimitExceeded, {}, "path", "Source Wave cache publication exceeds budget"
						};
						return false;
					}
					std::vector<SourcePathSequentialCachePoint> replacement;
					replacement.reserve(count);
					if (replacement.capacity() != count) {
						diagnostic = {
							Status::LimitExceeded, {}, "path", "Source Wave cache capacity exceeds admission"
						};
						return false;
					}
					for (const auto &entry : memo.Entries)
						if (entry.OwnerId == operation.EvaluationMemoId)
							replacement.push_back(
								{entry.Coordinate,
								 uint32_t(entry.Line),
								 {{entry.Point.X, entry.Point.Y}, entry.Point.Weight}}
							);
					const uint64_t oldBytes = state.Cache.capacity() * sizeof(SourcePathSequentialCachePoint);
					state.Cache.swap(replacement);
					std::vector<SourcePathSequentialCachePoint>{}.swap(replacement);
					if (!charge.Merge(std::move(*admission))) std::terminate();
					if (oldBytes) {
						auto release = charge.Split(oldBytes);
						if (!release) std::terminate();
					}
				}
				state.Buffers = owner.WaveBuffers;
				return true;
			}
			if (operation.Spiral && operation.EvaluationMemoId) {
				if (operation.EvaluationMemoId > memo.Owners.size()) {
					diagnostic = {
						Status::InvalidValue, {}, "path", "Source Spiral publication lacks current owner"
					};
					return false;
				}
				const auto &owner = memo.Owners[operation.EvaluationMemoId - 1];
				if (!owner.SpiralInitialized) return true;
				auto &state = *operation.Spiral;
				size_t count = 0;
				for (const auto &entry : memo.Entries)
					count += entry.OwnerId == operation.EvaluationMemoId;
				auto admission = budget.Reserve(count * sizeof(SourcePathSequentialCachePoint));
				if (!admission) {
					diagnostic = {
						Status::LimitExceeded, {}, "path", "Source Spiral cache publication exceeds budget"
					};
					return false;
				}
				std::vector<SourcePathSequentialCachePoint> replacement;
				replacement.reserve(count);
				if (replacement.capacity() != count) {
					diagnostic = {
						Status::LimitExceeded, {}, "path", "Source Spiral cache capacity exceeds admission"
					};
					return false;
				}
				for (const auto &entry : memo.Entries)
					if (entry.OwnerId == operation.EvaluationMemoId)
						replacement.push_back(
							{entry.Coordinate,
							 uint32_t(entry.Line),
							 {{entry.Point.X, entry.Point.Y}, entry.Point.Weight}}
						);
				const uint64_t oldBytes = state.Cache.capacity() * sizeof(SourcePathSequentialCachePoint);
				state.Cache.swap(replacement);
				std::vector<SourcePathSequentialCachePoint>{}.swap(replacement);
				if (!charge.Merge(std::move(*admission))) std::terminate();
				if (oldBytes) {
					auto release = charge.Split(oldBytes);
					if (!release) std::terminate();
				}
				state.Buffers = owner.SpiralBuffers;
				return true;
			}
			if (!operation.Sequential || !operation.EvaluationMemoId) return true;
			if (!operation.EvaluationMemoId || operation.EvaluationMemoId > memo.Owners.size()) {
				diagnostic = {
					Status::InvalidValue,
					{},
					"path",
					"Source sequential path publication lacks its current owner"
				};
				return false;
			}
			const auto &owner = memo.Owners[operation.EvaluationMemoId - 1];
			if (!owner.SequentialInitialized) return true;
			auto &state = *operation.Sequential;
			size_t count = 0;
			for (const auto &entry : memo.Entries)
				count += entry.OwnerId == operation.EvaluationMemoId;
			bool same = state.Cache.size() == count;
			size_t index = 0;
			for (const auto &entry : memo.Entries) {
				if (entry.OwnerId != operation.EvaluationMemoId) continue;
				const SourcePathSequentialCachePoint point{
					entry.Coordinate,
					uint32_t(entry.Line),
					{{entry.Point.X, entry.Point.Y}, entry.Point.Weight}
				};
				if (same && state.Cache[index] != point) same = false;
				++index;
			}
			if (!same) {
				auto admission = budget.Reserve(count * sizeof(SourcePathSequentialCachePoint));
				if (!admission) {
					diagnostic = {
						Status::LimitExceeded,
						{},
						"path",
						"Source sequential path cache publication exceeds byte budget"
					};
					return false;
				}
				std::vector<SourcePathSequentialCachePoint> replacement;
				replacement.reserve(count);
				if (replacement.capacity() != count) {
					diagnostic = {
						Status::LimitExceeded,
						{},
						"path",
						"Source sequential path cache capacity exceeds admission"
					};
					return false;
				}
				for (const auto &entry : memo.Entries)
					if (entry.OwnerId == operation.EvaluationMemoId)
						replacement.push_back(
							{entry.Coordinate,
							 uint32_t(entry.Line),
							 {{entry.Point.X, entry.Point.Y}, entry.Point.Weight}}
						);
				const uint64_t oldBytes = state.Cache.capacity() * sizeof(SourcePathSequentialCachePoint);
				state.Cache.swap(replacement);
				std::vector<SourcePathSequentialCachePoint>{}.swap(replacement);
				if (!charge.Merge(std::move(*admission))) std::terminate();
				if (oldBytes) {
					auto release = charge.Split(oldBytes);
					if (!release) std::terminate();
				}
			}
			if (operation.Kind == SourcePathOperationKind::Smoothen) {
				state.SmoothPoint = owner.SequentialBuffers[0];
				state.SmoothProbe = owner.SequentialBuffers[1];
			}
			return true;
		};
		if (!std::visit([&](auto &leaf) { return VisitSourcePathShift(leaf, route, synchronize); }, value))
			return false;
		if (!ValidRuntimeValue(value)) {
			diagnostic = {
				Status::LimitExceeded,
				{},
				"path",
				"Source sequential path publication exceeds aggregate value bounds"
			};
			return false;
		}
		return true;
	}
}
