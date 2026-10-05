#include "MeshPayload.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/DataReplay.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace engine::imagegraph {
	uint64_t RetainedDataReplayEntryBytes(const DataReplayEntry &entry) {
		uint64_t bytes = detail::MeshAddBytes(sizeof(entry), entry.NodeId.capacity());
		if (entry.LoadedCacheData.capacity() > std::string{}.capacity())
			bytes = detail::MeshAddBytes(bytes, entry.LoadedCacheData.capacity());
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.Values));
		for (const auto &frame : entry.Values)
			bytes = detail::MeshAddBytes(bytes, detail::RetainedPayloadBytes(frame.Data));
		return bytes;
	}
	uint64_t RetainedDataReplayBytes(const DataReplayState &state) {
		uint64_t bytes = detail::MeshAddBytes(sizeof(state), detail::MeshVectorBytes<true>(state.Entries));
		for (const auto &entry : state.Entries)
			bytes = detail::MeshAddBytes(bytes, RetainedDataReplayEntryBytes(entry) - sizeof(entry));
		bytes = detail::MeshAddBytes(
			bytes, RetainedCacheGroupReplayBytes(state.CacheGroups) - sizeof(state.CacheGroups)
		);
		return bytes;
	}
	uint64_t DataReplayValidationWorkspaceBytes(const DataReplayState &state, size_t additionalRows) {
		if (state.Entries.size() > Limits::MaximumArrayElements ||
			additionalRows > Limits::MaximumArrayElements - state.Entries.size() ||
			state.CacheGroups.Nodes.size() > Limits::MaximumNodes)
			return UINT64_MAX;
		size_t largest = state.Entries.size() + additionalRows;
		for (const auto &node : state.CacheGroups.Nodes) {
			if (node.Outputs.size() > Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts)
				return UINT64_MAX;
			largest = std::max(largest, node.Outputs.size());
		}
		return largest * sizeof(size_t);
	}
	Status ValidateDataReplay(const DataReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic) {
		diagnostic = {};
		const auto refuse = [&](Status code, std::string text, const std::string &node = std::string{}) {
			diagnostic.Code = code;
			diagnostic.Message = std::move(text);
			diagnostic.NodeId = node;
			return code;
		};
		if (state.Entries.size() > Limits::MaximumArrayElements ||
			RetainedDataReplayBytes(state) > maximumBytes ||
			RetainedDataReplayBytes(state) > Limits::MaximumEvaluationBytes)
			return refuse(Status::LimitExceeded, "data replay exceeds row or byte budget");
		const uint64_t retained = RetainedDataReplayBytes(state);
		const auto groupBytes = RetainedCacheGroupReplayBytes(state.CacheGroups);
		const auto otherRetained = retained - groupBytes;
		if (ValidateCacheGroupReplay(state.CacheGroups, maximumBytes - otherRetained, diagnostic) !=
			Status::Ok)
			return diagnostic.Code;
		if (state.Entries.size() > (maximumBytes - retained) / sizeof(size_t))
			return refuse(Status::LimitExceeded, "data replay validation workspace exceeds budget");
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
				return refuse(Status::DuplicateId, "data replay repeats processor identity", second.NodeId);
		}

		for (size_t index = 0; index < state.Entries.size(); ++index) {
			const auto &entry = state.Entries[index];
			if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes ||
				!entry.Initialized || entry.Tick > Limits::MaximumTick || !std::isfinite(entry.Subframe) ||
				entry.Subframe < 0 || entry.Subframe >= 1 ||
				entry.ProcessorRow >= Limits::MaximumArrayElements || !std::isfinite(entry.PreviousValue) ||
				!std::isfinite(entry.PreviousFrame))
				return refuse(
					Status::InvalidValue, "data replay identity or scalar state is invalid", entry.NodeId
				);
			if (entry.LoadedCacheData.size() > Limits::MaximumTextBytes)
				return refuse(
					Status::LimitExceeded, "loaded frame cache identity exceeds text bounds", entry.NodeId
				);
			if (entry.Values.size() > Limits::MaximumArrayElements)
				return refuse(
					Status::LimitExceeded, "data replay value history exceeds frame budget", entry.NodeId
				);
			uint64_t previousFrame = 0;
			bool first = true;
			for (const auto &frame : entry.Values) {
				if (frame.Frame > Limits::MaximumTick || (!first && frame.Frame <= previousFrame) ||
					!detail::ValidValuePayload(frame.Data, true))
					return refuse(Status::InvalidValue, "data replay value history is invalid", entry.NodeId);
				const auto clone = ValueClonePayloadBytes(frame.Data);
				if (!clone || *clone > maximumBytes)
					return refuse(
						Status::LimitExceeded, "data replay value clone exceeds byte budget", entry.NodeId
					);
				first = false;
				previousFrame = frame.Frame;
			}
		}
		return Status::Ok;
	}
}
