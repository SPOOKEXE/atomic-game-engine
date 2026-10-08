#pragma once
#include "ValuePayload.hpp"

#include <engine/imagegraph/DataReplay.hpp>

#include <cmath>
namespace engine::imagegraph::detail {
	inline Status
	ValidateReplayRow(const DataReplayEntry &entry, uint64_t maximumBytes, Diagnostic &diagnostic) {
		const auto refuse = [&](Status code, const char *text, const std::string &node) {
			diagnostic = {code, node, {}, text};
			return code;
		};

		if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes || !entry.Initialized ||
			entry.Tick > Limits::MaximumTick || !std::isfinite(entry.Subframe) || entry.Subframe < 0 ||
			entry.Subframe >= 1 || entry.ProcessorRow >= Limits::MaximumArrayElements ||
			!std::isfinite(entry.PreviousValue) || !std::isfinite(entry.PreviousFrame))
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
		if (entry.SourceFrameCacheSerializedSlots &&
			*entry.SourceFrameCacheSerializedSlots > Limits::MaximumArrayElements)
			return refuse(
				Status::LimitExceeded, "frame cache serialized slot count exceeds bounds", entry.NodeId
			);
		if (entry.SourceFrameCacheSerializedSlots &&
			(entry.Values.size() < 2 || entry.Values[0].Frame != 0 || entry.Values[1].Frame != 1 ||
			 !std::holds_alternative<std::string>(entry.Values[0].Data) ||
			 (std::get<std::string>(entry.Values[0].Data) != "pc.cache" &&
			  std::get<std::string>(entry.Values[0].Data) != "pc.cache_array")))
			return refuse(
				Status::InvalidValue, "serialized slot metadata lacks frame cache identity", entry.NodeId
			);
		if (entry.SourceFrameCacheLoading) {
			const auto &progress = *entry.SourceFrameCacheLoading;
			if (!entry.SourceFrameCacheSerializedSlots ||
				progress.NextSlot > *entry.SourceFrameCacheSerializedSlots || entry.Values.size() < 2 ||
				entry.Values[0].Frame != 0 || entry.Values[1].Frame != 1 ||
				!std::holds_alternative<std::string>(entry.Values[0].Data))
				return refuse(Status::InvalidValue, "frame cache loading state is malformed", entry.NodeId);
			const auto &type = std::get<std::string>(entry.Values[0].Data);
			if ((type != "pc.cache" && type != "pc.cache_array") ||
				(progress.NativeReceipt && !progress.PendingSlots.empty()) ||
				(!progress.Loading &&
				 (progress.NextSlot == 0 ||
				  (type == "pc.cache_array" && progress.NextSlot != *entry.SourceFrameCacheSerializedSlots))))
				return refuse(
					Status::InvalidValue, "frame cache loading inventory is malformed", entry.NodeId
				);
			if (progress.PendingSlots.size() > Limits::MaximumArrayElements)
				return refuse(
					Status::LimitExceeded, "frame cache pending inventory exceeds bounds", entry.NodeId
				);
			if (progress.Loading)
				for (const auto &frame : entry.Values)
					if (frame.Frame >= 2 && frame.Frame - 2 >= progress.NextSlot)
						return refuse(
							Status::InvalidValue,
							"frame cache loading exposes an unpublished future slot",
							entry.NodeId
						);
			uint64_t previous = 0;
			bool first = true;
			for (const auto &frame : progress.PendingSlots) {
				if (frame.Frame < 2 || frame.Frame - 2 >= *entry.SourceFrameCacheSerializedSlots ||
					frame.Frame - 2 < progress.NextSlot || (!first && frame.Frame <= previous) ||
					!ValidValuePayload(frame.Data, true))
					return refuse(
						Status::InvalidValue, "frame cache pending slot is malformed", entry.NodeId
					);
				const auto bytes = ValueClonePayloadBytes(frame.Data);
				if (!bytes || *bytes > maximumBytes)
					return refuse(
						Status::LimitExceeded, "frame cache pending clone exceeds bounds", entry.NodeId
					);
				previous = frame.Frame;
				first = false;
			}
		}
		return Status::Ok;
	}
}
