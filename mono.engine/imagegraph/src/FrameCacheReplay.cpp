#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	std::string_view SourceFrameCacheRowType(const DataReplayEntry &entry) {
		if (entry.Values.size() < 2 || entry.Values[0].Frame != 0 || entry.Values[1].Frame != 1) return {};
		const auto *type = std::get_if<std::string>(&entry.Values[0].Data);
		return type && (*type == "pc.cache" || *type == "pc.cache_array") ? std::string_view(*type)
																		  : std::string_view{};
	}
	const Value *SourceFrameCacheLastOutput(const DataReplayEntry &entry) {
		return SourceFrameCacheRowType(entry).empty() ? nullptr : &entry.Values[1].Data;
	}
	Status OverlaySourceFrameCacheRows(
		const Document &document,
		const DataReplayState &retained,
		DataReplayState &target,
		FrameCacheOutputPolicy policy,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.overlay");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (policy != FrameCacheOutputPolicy::RetainedObservation &&
			policy != FrameCacheOutputPolicy::PreObservation && policy != FrameCacheOutputPolicy::Constructor)
			return fail(Status::InvalidValue, "invalid frame-cache output observation policy");
		if (maximumBytes > Limits::MaximumEvaluationBytes || document.Nodes.size() > Limits::MaximumNodes ||
			retained.Entries.size() > Limits::MaximumArrayElements ||
			target.Entries.size() > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "frame-cache overlay exceeds count or byte bounds");
		// Charge worst-case membership/name work before scanning IDs or allocating copies.
		uint64_t names = 0;
		for (const auto &node : document.Nodes) {
			if (names >= 64ull * 1024 * 1024 || node.Id.size() >= 64ull * 1024 * 1024 - names)
				return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
			names += node.Id.size() + 1;
		}
		const uint64_t scans = 3 * (retained.Entries.size() + target.Entries.size());
		if (names > 64ull * 1024 * 1024 - 64)
			return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
		names += 64;
		if (scans && names > (64ull * 1024 * 1024) / scans)
			return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
		const uint64_t pairs = 3 * uint64_t(retained.Entries.size()) * target.Entries.size();
		uint64_t longest = 1;
		for (const auto &entry : retained.Entries)
			longest = std::max(longest, uint64_t(entry.NodeId.size() + 1));
		for (const auto &entry : target.Entries)
			longest = std::max(longest, uint64_t(entry.NodeId.size() + 1));
		if (pairs && longest > (64ull * 1024 * 1024 - scans * names) / pairs)
			return fail(Status::LimitExceeded, "frame-cache row merge exceeds work bounds");
		const auto alive = [&](const DataReplayEntry &entry) {
			const auto type = SourceFrameCacheRowType(entry);
			return !type.empty() &&
				   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					   return node.Id == entry.NodeId && node.Type == type;
				   });
		};
		uint64_t copies = 0;
		size_t count = 0;
		for (const auto &entry : target.Entries)
			if (SourceFrameCacheRowType(entry).empty() || alive(entry)) ++count;
		for (const auto &entry : target.Entries) {
			const uint64_t bytes = RetainedDataReplayEntryBytes(entry);
			if (bytes > (maximumBytes - std::min(copies, maximumBytes)) / 2)
				return fail(Status::LimitExceeded, "frame-cache target copy exceeds bounds");
			copies += bytes * 2;
		}
		for (const auto &entry : retained.Entries)
			if (alive(entry)) {
				const uint64_t bytes = RetainedDataReplayEntryBytes(entry);
				if (bytes > (maximumBytes - std::min(copies, maximumBytes)) / 2)
					return fail(Status::LimitExceeded, "frame-cache retained copy exceeds bounds");
				copies += bytes * 2;
				if (std::none_of(target.Entries.begin(), target.Entries.end(), [&](const auto &old) {
						return old.NodeId == entry.NodeId && old.ProcessorRow == entry.ProcessorRow &&
							   (SourceFrameCacheRowType(old).empty() ||
								SourceFrameCacheRowType(old) == SourceFrameCacheRowType(entry));
					}))
					++count;
			}
		if (count > Limits::MaximumArrayElements ||
			count * sizeof(DataReplayEntry) > maximumBytes - std::min(copies, maximumBytes))
			return fail(Status::LimitExceeded, "frame-cache overlay table exceeds bounds");
		copies += count * sizeof(DataReplayEntry);
		const uint64_t validation = std::max(retained.Entries.size(), target.Entries.size()) * sizeof(size_t);
		if (validation > maximumBytes - std::min(copies, maximumBytes))
			return fail(Status::LimitExceeded, "frame-cache validation workspace exceeds bounds");
		if (ValidateDataReplay(retained, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok ||
			ValidateDataReplay(target, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		DataReplayState candidate;
		candidate.Entries.reserve(count);
		for (const auto &entry : target.Entries)
			if (SourceFrameCacheRowType(entry).empty() || alive(entry)) candidate.Entries.push_back(entry);
		for (const auto &entry : retained.Entries)
			if (alive(entry)) {
				const auto position =
					std::find_if(candidate.Entries.begin(), candidate.Entries.end(), [&](const auto &row) {
						return row.NodeId == entry.NodeId && row.ProcessorRow == entry.ProcessorRow;
					});
				DataReplayEntry replacement = entry;
				auto &output = replacement.Values[1].Data;
				if (policy == FrameCacheOutputPolicy::Constructor ||
					policy == FrameCacheOutputPolicy::PreObservation) {
					const Value *earlier =
						position == candidate.Entries.end() ? nullptr : SourceFrameCacheLastOutput(*position);
					if (policy == FrameCacheOutputPolicy::PreObservation && earlier)
						output = *earlier;
					else if (SourceFrameCacheRowType(entry) == "pc.cache")
						output = int64_t{-4};
					else
						output = ArrayValue{ValueType::Any, {}};
				}
				if (position == candidate.Entries.end())
					candidate.Entries.push_back(std::move(replacement));
				else
					*position = std::move(replacement);
			}
		target = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "frame-cache overlay allocation failed"};
		return diagnostic.Code;
	}
}
