#include "CacheResultsSlots.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/CacheResultsReplay.hpp>

#include <algorithm>
#include <cmath>
#include <new>

namespace engine::imagegraph {
	namespace {
		constexpr std::string_view FREED = "native.cache-results.freed-slot";
		constexpr uint64_t MARKER_BYTES = sizeof(StructData) + sizeof(std::pair<std::string, Value>) +
										  2 * FREED.size() + 64 + sizeof(ElementValue);
		bool SourceList(const DataReplayEntry &entry) {
			if (entry.Values.size() != 1 || entry.Values[0].Frame != 0 ||
				!std::isfinite(entry.PreviousValue) || entry.PreviousValue < 0 ||
				entry.PreviousValue >= double(Limits::MaximumArrayElements) ||
				std::floor(entry.PreviousValue) != entry.PreviousValue)
				return false;
			const auto *array = std::get_if<ArrayValue>(&entry.Values[0].Data);
			return detail::CacheResultsSlots{array}.Valid();
		}
		ElementValue FreedSlot() {
			StructValue slot;
			slot.Data.emplace();
			slot.Data->Fields.reserve(1);
			slot.Data->Fields.emplace_back(std::string(FREED), true);
			return slot;
		}
		bool FreedRecord(const StructValue *record) {
			if (!record || !record->Data || record->Data->Fields.size() != 1) return false;
			const auto &field = record->Data->Fields.front();
			const auto *freed = std::get_if<bool>(&field.second);
			return field.first == FREED && freed && *freed;
		}
	}
	bool IsFreedCacheResultsSlot(const ElementValue &slot) {
		return FreedRecord(std::get_if<StructValue>(&slot));
	}
	bool IsFreedCacheResultsSlot(const Value &slot) {
		return FreedRecord(std::get_if<StructValue>(&slot));
	}
	uint64_t ClearedCacheResultsReplayBytes(const DataReplayState &source, std::string_view nodeId) {
		if (source.Entries.size() > Limits::MaximumArrayElements) return UINT64_MAX;
		uint64_t bytes = RetainedDataReplayBytes(source);
		if (bytes > Limits::MaximumEvaluationBytes) return UINT64_MAX;
		for (const auto &entry : source.Entries) {
			if (entry.NodeId != nodeId) continue;
			if (!SourceList(entry)) return UINT64_MAX;
			const auto &array = std::get<ArrayValue>(entry.Values[0].Data);
			const size_t count = detail::CacheResultsSlots{&array}.Count();
			if (count > (UINT64_MAX - bytes) / MARKER_BYTES) return UINT64_MAX;
			bytes += count * MARKER_BYTES;
		}
		return bytes;
	}
	Status ClearCacheResultsReplay(
		const DataReplayState &source,
		std::string_view nodeId,
		DataReplayState &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.cache_results.clear");
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, std::string(nodeId), {}, message};
			return status;
		};
		if (nodeId.empty()) return fail(Status::UnknownNode, "Cache Results clear requires a node ID");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "Cache Results clear byte cap is outside bounds");
		const uint64_t retained = RetainedDataReplayBytes(source);
		if (source.Entries.size() > Limits::MaximumArrayElements || retained > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "Cache Results clear source exceeds replay bounds");
		const uint64_t oldOutput =
			&source == &output ? 0 : RetainedDataReplayBytes(output) - sizeof(DataReplayState);
		const uint64_t replacement = ClearedCacheResultsReplayBytes(source, nodeId);
		if (replacement == UINT64_MAX)
			return fail(Status::InvalidValue, "Cache Results clear requires an owned source slot list");
		const uint64_t workspace = source.Entries.size() * sizeof(size_t);
		if (retained > maximumBytes || oldOutput > maximumBytes - retained ||
			replacement > maximumBytes - retained - oldOutput ||
			workspace > maximumBytes - retained - oldOutput - replacement)
			return fail(Status::LimitExceeded, "Cache Results clear replacement exceeds live byte bounds");
		if (ValidateDataReplay(source, maximumBytes, diagnostic) != Status::Ok) return diagnostic.Code;
		DataReplayState candidate = source;
		for (auto &entry : candidate.Entries) {
			if (entry.NodeId != nodeId) continue;
			auto &array = std::get<ArrayValue>(entry.Values[0].Data);
			const size_t count = detail::CacheResultsSlots{&array}.Count();
			std::vector<ElementValue> freed;
			freed.reserve(count);
			for (size_t index = 0; index < count; ++index)
				freed.push_back(FreedSlot());
			array.Elements = std::move(freed);
			std::vector<SourceArrayItem>{}.swap(array.Items);
			array.ElementType = ValueType::Struct;
			entry.PreviousValue = 0;
		}
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			Status::LimitExceeded, std::string(nodeId), {}, "Cache Results clear allocation refused"
		};
		return Status::LimitExceeded;
	}
}
