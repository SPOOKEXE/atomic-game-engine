#pragma once
#include "NodeExecutors.hpp"
namespace engine::imagegraph::detail {
	inline const DataReplayEntry *
	FindStrandReplayEntry(const NodeContext &context, std::string_view id, size_t row) {
		for (auto i = context.DataUpdates.rbegin(); i != context.DataUpdates.rend(); ++i)
			if (i->NodeId == id && i->ProcessorRow == row) return &*i;
		const auto *state = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
		if (state)
			for (const auto &entry : state->Entries)
				if (entry.NodeId == id && entry.ProcessorRow == row) return &entry;
		return nullptr;
	}
	inline bool ValidStrandReplayEntry(const DataReplayEntry &entry) {
		if (!entry.Initialized || entry.Values.size() != 1 || entry.Values[0].Frame != entry.Tick)
			return false;
		const auto *value = std::get_if<StrandValue>(&entry.Values[0].Data);
		return value && value->Data && ValidStrandPayload(*value) &&
			   value->Data->OriginNodeId == entry.NodeId &&
			   value->Data->OriginProcessorRow == entry.ProcessorRow;
	}
	inline const StrandValue *FindStrandReplayValue(const NodeContext &context, const StrandValue &value) {
		if (!value.Data || value.Data->OriginNodeId.empty()) return nullptr;
		const auto *entry =
			FindStrandReplayEntry(context, value.Data->OriginNodeId, value.Data->OriginProcessorRow);
		if (!entry || !ValidStrandReplayEntry(*entry)) return nullptr;
		const auto *stored = std::get_if<StrandValue>(&entry->Values[0].Data);
		return stored && stored->Data && stored->Data->AuthoringRevision == value.Data->AuthoringRevision
				   ? stored
				   : nullptr;
	}
	inline bool PublishStrandReplay(NodeContext &context, const StrandValue &value) {
		if (!value.Data || value.Data->OriginNodeId.empty()) return true;
		const auto &data = *value.Data;
		if (!ValidStrandPayload(value))
			return context.Fail(Status::InvalidValue, "Strand replay payload is invalid", "strands");
		const uint64_t bytes = MeshAddBytes(
			sizeof(DataReplayEntry) + std::max(data.OriginNodeId.size(), std::string{}.capacity()) +
				sizeof(DataReplayValueFrame),
			StrandStorageBytes<true>(value)
		);
		if (!context.ReserveOutput(bytes, "strands")) return false;
		if (context.DataUpdates.size() >= Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "Strand replay update count exceeds budget");
		if (context.DataUpdates.size() == context.DataUpdates.capacity()) {
			const size_t capacity = context.DataUpdates.size() + 1;
			if (!context.ReserveOutput(
					(capacity - context.DataUpdates.capacity()) * sizeof(DataReplayEntry), "strands"
				))
				return false;
			context.DataUpdates.reserve(capacity);
		}
		DataReplayEntry entry;
		entry.NodeId = data.OriginNodeId;
		entry.ProcessorRow = data.OriginProcessorRow;
		entry.Tick = context.Request.Tick;
		entry.Subframe = context.Request.Subframe;
		entry.NegativeFrame = context.Request.NegativeFrame;
		entry.Initialized = true;
		entry.Values.push_back({context.Request.Tick, Value{value}});
		context.DataUpdates.push_back(std::move(entry));
		return true;
	}
	inline bool StrandReplayTimeMatches(const DataReplayEntry &entry, const EvaluationRequest &request) {
		return entry.Tick == request.Tick && entry.Subframe == request.Subframe &&
			   entry.NegativeFrame == request.NegativeFrame;
	}
} // namespace engine::imagegraph::detail
