#include "MeshPayload.hpp"

#include <engine/imagegraph/RandomReplay.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace engine::imagegraph {
	uint64_t RetainedRandomEntryBytes(const RandomReplayEntry &entry) {
		return detail::MeshAddBytes(
			detail::MeshAddBytes(sizeof(entry), entry.NodeId.capacity()),
			detail::MeshVectorBytes<true>(entry.Kernel)
		);
	}
	uint64_t RetainedRandomReplayBytes(const RandomReplayState &state) {
		uint64_t bytes = detail::MeshAddBytes(sizeof(state), detail::MeshVectorBytes<true>(state.Entries));
		for (const auto &entry : state.Entries)
			bytes = detail::MeshAddBytes(bytes, RetainedRandomEntryBytes(entry) - sizeof(entry));
		return bytes;
	}
	Status
	ValidateRandomReplay(const RandomReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic) {
		diagnostic = {};
		const auto refuse = [&](Status code, std::string text, const std::string &node = std::string{}) {
			diagnostic.Code = code;
			diagnostic.Message = std::move(text);
			diagnostic.NodeId = node;
			return code;
		};
		if (state.Entries.size() > Limits::MaximumArrayElements ||
			RetainedRandomReplayBytes(state) > maximumBytes ||
			RetainedRandomReplayBytes(state) > Limits::MaximumEvaluationBytes)
			return refuse(Status::LimitExceeded, "random replay exceeds row or byte budget");
		const uint64_t retained = RetainedRandomReplayBytes(state);
		if (state.Entries.size() > (maximumBytes - retained) / sizeof(size_t))
			return refuse(Status::LimitExceeded, "random replay validation workspace exceeds budget");
		std::vector<size_t> order(state.Entries.size());
		std::iota(order.begin(), order.end(), size_t{0});
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			const auto &first = state.Entries[a];
			const auto &second = state.Entries[b];
			return first.NodeId == second.NodeId ? first.ProcessorRow < second.ProcessorRow
												 : first.NodeId < second.NodeId;
		});
		for (size_t index = 1; index < order.size(); ++index) {
			const auto &first = state.Entries[order[index - 1]], &second = state.Entries[order[index]];
			if (first.NodeId == second.NodeId && first.ProcessorRow == second.ProcessorRow)
				return refuse(Status::DuplicateId, "random replay repeats processor identity", second.NodeId);
		}

		for (size_t index = 0; index < state.Entries.size(); ++index) {
			const auto &entry = state.Entries[index];
			if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes ||
				!entry.Initialized || entry.Tick > Limits::MaximumTick || !std::isfinite(entry.Subframe) ||
				entry.Subframe < 0 || entry.Subframe >= 1 ||
				entry.ProcessorRow >= Limits::MaximumArrayElements || !std::isfinite(entry.Accumulation) ||
				!std::isfinite(entry.MovingAverage) || !std::isfinite(entry.PreviousOutput))
				return refuse(
					Status::InvalidValue, "random replay identity or scalar state is invalid", entry.NodeId
				);
			if (entry.Kernel.size() > Limits::MaximumArrayElements)
				return refuse(
					Status::LimitExceeded, "random convolution history exceeds frame budget", entry.NodeId
				);
			for (double value : entry.Kernel)
				if (!std::isfinite(value))
					return refuse(
						Status::InvalidValue, "random convolution history is nonfinite", entry.NodeId
					);
		}
		return Status::Ok;
	}
}
