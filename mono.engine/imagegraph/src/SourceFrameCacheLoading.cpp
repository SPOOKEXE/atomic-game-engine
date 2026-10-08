#include "DataReplayValidation.hpp"
#include "MeshPayload.hpp"
#include "NativeFrameCacheReceipt.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	namespace {
		Status FailLoading(const Node &node, Diagnostic &error, Status code, const char *text) {
			error = {code, node.Id, "cache", text};
			return code;
		}
		Value ConstructorOutput(const Node &node) {
			if (node.Type == "pc.cache") return int64_t{-4};
			ArrayValue value;
			value.ElementType = ValueType::Any;
			return value;
		}
		Status
		CheckLoadingRow(const Node &node, const DataReplayEntry &row, Diagnostic &error, uint64_t cap) {
			if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") || node.Id.empty() ||
				node.Id.size() > Limits::MaximumTextBytes ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
				return FailLoading(node, error, Status::InvalidValue, "frame cache loading node is invalid");
			if (row.Values.size() > Limits::MaximumArrayElements ||
				(row.SourceFrameCacheLoading &&
				 row.SourceFrameCacheLoading->PendingSlots.size() > Limits::MaximumArrayElements))
				return FailLoading(
					node, error, Status::LimitExceeded, "frame cache loading inventory exceeds bounds"
				);
			if (row.NodeId != node.Id || SourceFrameCacheRowType(row) != node.Type ||
				row.LoadedCacheData != SourceFrameCacheIdentity(node))
				return FailLoading(
					node, error, Status::InvalidValue, "frame cache loading identity differs from node"
				);
			if (!row.SourceFrameCacheSerializedSlots)
				return FailLoading(
					node,
					error,
					Status::UnsupportedExecution,
					"frame cache loading requires original serialized slot count"
				);
			bool malformed = false;
			uint64_t work = sizeof(row) + row.NodeId.size() + row.LoadedCacheData.size();
			const auto chargeFrames = [&](const std::vector<DataReplayValueFrame> &frames) {
				for (const auto &frame : frames) {
					const auto bytes = ValueClonePayloadBytes(frame.Data);
					if (!bytes) {
						malformed = true;
						return false;
					}
					if (work > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES ||
						*bytes > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES - work)
						return false;
					work += *bytes;
				}
				return true;
			};
			if (!chargeFrames(row.Values) ||
				(row.SourceFrameCacheLoading && !chargeFrames(row.SourceFrameCacheLoading->PendingSlots)))
				return FailLoading(
					node,
					error,
					malformed ? Status::InvalidValue : Status::LimitExceeded,
					"frame cache loading clone work exceeds bounds"
				);
			return detail::ValidateReplayRow(row, cap, error);
		}
		bool AdmitLoading(
			const DataReplayEntry *source,
			const DataReplayEntry &output,
			uint64_t candidate,
			uint64_t extra,
			uint64_t cap
		) {
			if (!cap || cap > Limits::MaximumEvaluationBytes ||
				output.Values.size() > Limits::MaximumArrayElements ||
				(output.SourceFrameCacheLoading &&
				 output.SourceFrameCacheLoading->PendingSlots.size() > Limits::MaximumArrayElements))
				return false;
			uint64_t bytes = extra;
			if (source) bytes = detail::MeshAddBytes(bytes, RetainedDataReplayEntryBytes(*source));
			if (source != &output) bytes = detail::MeshAddBytes(bytes, RetainedDataReplayEntryBytes(output));
			return detail::MeshAddBytes(bytes, candidate) <= cap;
		}
	}
	Status BeginSourceFrameCacheLoading(
		const Node &node,
		const DataReplayEntry &decoded,
		DataReplayEntry &output,
		Diagnostic &error,
		uint64_t cap
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.loading_begin");
		error = {};
		if (auto code = CheckLoadingRow(node, decoded, error, cap); code != Status::Ok) return code;
		if (decoded.SourceFrameCacheLoading)
			return FailLoading(node, error, Status::InvalidValue, "frame cache loading has already begun");
		const auto count = *decoded.SourceFrameCacheSerializedSlots;
		if (node.Type == "pc.cache_array" && count > Limits::MaximumArrayElements - 2)
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache array published slots exceed bounds"
			);
		for (const auto &frame : decoded.Values)
			if (frame.Frame >= 2 && frame.Frame - 2 >= count)
				return FailLoading(
					node, error, Status::InvalidValue, "decoded frame lies outside serialized inventory"
				);
		const auto quote = detail::MeshAddBytes(
			RetainedDataReplayEntryBytes(decoded), 2 * sizeof(DataReplayValueFrame) + 64
		);
		if (!AdmitLoading(&decoded, output, quote, 0, cap))
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache loading begin exceeds byte budget"
			);
		DataReplayEntry candidate = decoded;
		auto &progress = candidate.SourceFrameCacheLoading.emplace();
		progress.PendingSlots = std::move(candidate.Values);
		candidate.Values.reserve(2);
		candidate.Values.push_back(std::move(progress.PendingSlots[0]));
		candidate.Values.push_back({1, ConstructorOutput(node)});
		progress.PendingSlots.erase(progress.PendingSlots.begin(), progress.PendingSlots.begin() + 2);
		output = std::move(candidate);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return FailLoading(node, error, Status::LimitExceeded, "frame cache loading allocation failed");
	}
	Status BeginNativeSourceFrameCacheLoading(
		const Node &node, DataReplayEntry &output, Diagnostic &error, uint64_t cap
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.loading_begin");
		error = {};
		if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") || node.Id.empty() ||
			node.Id.size() > Limits::MaximumTextBytes ||
			node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
			return FailLoading(node, error, Status::InvalidValue, "frame cache loading node is invalid");
		detail::NativeFrameCacheReceiptInspection inspection;
		const auto code = detail::InspectNativeFrameCacheReceipt(node, 0, inspection);
		if (code != Status::Ok)
			return FailLoading(node, error, code, "native frame cache receipt is invalid");
		if (!inspection.SerializedSlots)
			return FailLoading(
				node,
				error,
				Status::UnsupportedExecution,
				"native frame cache loading requires original serialized slot count"
			);
		if (node.Type == "pc.cache_array" && *inspection.SerializedSlots > Limits::MaximumArrayElements - 2)
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache array published slots exceed bounds"
			);
		const auto identity = SourceFrameCacheIdentity(node);
		const uint64_t quote = sizeof(DataReplayEntry) + node.Id.size() + identity.size() +
							   2 * sizeof(DataReplayValueFrame) + 128;
		if (!AdmitLoading(nullptr, output, quote, 0, cap))
			return FailLoading(
				node, error, Status::LimitExceeded, "native frame cache loading begin exceeds byte budget"
			);
		DataReplayEntry candidate;
		candidate.NodeId = node.Id;
		candidate.Initialized = true;
		candidate.PreviousValue = 1;
		candidate.LoadedCacheData = identity;
		candidate.SourceFrameCacheSerializedSlots = inspection.SerializedSlots;
		candidate.SourceFrameCacheLoading.emplace().NativeReceipt = true;
		candidate.Values = {{0, std::string(node.Type)}, {1, ConstructorOutput(node)}};
		output = std::move(candidate);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return FailLoading(
			node, error, Status::LimitExceeded, "native frame cache loading allocation failed"
		);
	}
	Status StepSourceFrameCacheLoading(
		const Node &node,
		uint64_t totalFrames,
		const DataReplayEntry &source,
		DataReplayEntry &output,
		bool &completedNow,
		Diagnostic &error,
		uint64_t cap
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.loading_step");
		error = {};
		if (auto code = CheckLoadingRow(node, source, error, cap); code != Status::Ok) return code;
		if (!source.SourceFrameCacheLoading)
			return FailLoading(node, error, Status::InvalidValue, "frame cache loading has not begun");
		const auto &progress = *source.SourceFrameCacheLoading;
		const uint64_t base = RetainedDataReplayEntryBytes(source);
		const uint64_t extra = (source.Values.size() + 1) * sizeof(DataReplayValueFrame) + 64;
		if (!AdmitLoading(&source, output, base, extra, cap))
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache loading step exceeds byte budget"
			);
		if (!progress.Loading) {
			DataReplayEntry candidate = source;
			output = std::move(candidate);
			completedNow = false;
			return Status::Ok;
		}
		if (progress.NextSlot >= *source.SourceFrameCacheSerializedSlots)
			return FailLoading(
				node,
				error,
				Status::UnsupportedExecution,
				"source frame cache loading selected an undefined inventory slot"
			);
		if (totalFrames > Limits::MaximumTick)
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache project frame count exceeds bounds"
			);
		const uint64_t frame = progress.NextSlot + 2;
		const auto found = std::lower_bound(
			source.Values.begin(), source.Values.end(), frame, [](const auto &item, uint64_t key) {
				return item.Frame < key;
			}
		);
		if ((found == source.Values.end() || found->Frame != frame) &&
			source.Values.size() == Limits::MaximumArrayElements)
			return FailLoading(
				node, error, Status::LimitExceeded, "frame cache published slot count exceeds bounds"
			);
		Value decoded = int64_t{-4};
		if (progress.NativeReceipt) {
			const uint64_t used = detail::MeshAddBytes(detail::MeshAddBytes(base, extra), base);
			const uint64_t previous = &source == &output ? 0 : RetainedDataReplayEntryBytes(output);
			if (used > cap || previous > cap - used)
				return FailLoading(
					node, error, Status::LimitExceeded, "frame cache slot decoder exceeds byte budget"
				);
			const auto code = detail::DecodeSourceFrameCacheReceiptSlot(
				node, progress.NextSlot, decoded, error, cap - used - previous
			);
			if (code != Status::Ok) return code;
		}
		DataReplayEntry candidate = source;
		auto &next = *candidate.SourceFrameCacheLoading;
		if (!next.NativeReceipt) {
			auto pending = std::lower_bound(
				next.PendingSlots.begin(),
				next.PendingSlots.end(),
				frame,
				[](const auto &item, uint64_t key) { return item.Frame < key; }
			);
			if (pending != next.PendingSlots.end() && pending->Frame == frame) {
				decoded = std::move(pending->Data);
				next.PendingSlots.erase(pending);
			}
		}
		candidate.Values.reserve(candidate.Values.size() + 1);
		auto destination = std::lower_bound(
			candidate.Values.begin(), candidate.Values.end(), frame, [](const auto &item, uint64_t key) {
				return item.Frame < key;
			}
		);
		if (destination != candidate.Values.end() && destination->Frame == frame)
			destination->Data = std::move(decoded);
		else
			candidate.Values.insert(destination, {frame, std::move(decoded)});
		++next.NextSlot;
		const bool complete =
			next.NextSlot ==
			(node.Type == "pc.cache" ? totalFrames : *candidate.SourceFrameCacheSerializedSlots);
		if (complete) next.Loading = false;
		output = std::move(candidate);
		completedNow = complete;
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return FailLoading(node, error, Status::LimitExceeded, "frame cache loading allocation failed");
	}
}
