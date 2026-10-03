#include "../SourcePathShiftMemo.hpp"
#include "ArraySource.hpp"
#include "Families.hpp"

#include <engine/imagegraph/DataReplay.hpp>

#include <algorithm>

namespace engine::imagegraph::detail {
	namespace {
		uint64_t ItemStorage(const Value &value) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return 0;
			if (!array->Items.empty()) return 0;
			uint64_t nodes = array->Nested.empty() ? array->Elements.size() : array->Nested.size();
			for (const auto &row : array->Nested)
				nodes += row.size();
			return nodes * sizeof(SourceArrayItem);
		}
		SourceArrayItem HistoryItem(const Value &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value)) return {surface->Data};
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				if (!array->Items.empty()) return {array->Items};
				source_array::Items members;
				if (array->Nested.empty()) {
					members.reserve(array->Elements.size());
					for (const auto &leaf : array->Elements)
						members.push_back({leaf});
				} else {
					members.reserve(array->Nested.size());
					for (const auto &row : array->Nested) {
						source_array::Items children;
						children.reserve(row.size());
						for (const auto &leaf : row)
							children.push_back({leaf});
						members.push_back({std::move(children)});
					}
				}
				return {std::move(members)};
			}
			return {std::move(*ArrayElement(value))};
		}
		// Source array_clone copies arrays and aliases non-array values. Native history owns
		// immutable snapshots; mutable struct/resource identity needs a source host observation.
		bool CacheValueArray(NodeContext &c) {
			const bool linked = c.IsLinked("value") || c.Input("value") ||
								std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &item) {
									return item.first == "value";
								});
			if (!linked) {
				c.SetValue("cache_array", ArrayValue{ValueType::Any, {}});
				return c.FailureCode == Status::Ok;
			}
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (!owner)
				return c.Fail(
					Status::UnsupportedExecution, "value cache needs an explicit data replay owner"
				);
			if (c.Request.NegativeFrame || c.Request.Subframe != 0)
				return c.Fail(
					Status::UnsupportedExecution, "value cache requires integer nonnegative source frames"
				);
			Diagnostic diagnostic;
			if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
				return c.Fail(diagnostic.Code, diagnostic.Message);
			const DataReplayEntry *previous = nullptr;
			for (const auto &entry : owner->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) previous = &entry;
			const double total = c.Timeline ? double(c.Timeline->Frames) : 1;
			const int64_t begin = c.Integer("start_frame", -1), end = c.Integer("stop_frame", -1);
			const double first = begin < 0 ? 0 : double(begin) - 1,
						 last = end < 0 ? total - 1 : double(end) - 1;
			const bool capture = double(c.Request.Tick) >= first && double(c.Request.Tick) <= last;
			const Image *surface = c.Input("value");
			const ImageArray *surfaces = nullptr;
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "value") surfaces = array;
			uint64_t resourceBytes = surface ? surface->Pixels.size() + sizeof(Image) : 0;
			if (surfaces) {
				source_array::TreeCost cost;
				if (!source_array::ImageCost(*surfaces, surfaces->Items, cost, 1))
					return c.Fail(Status::LimitExceeded, "value cache surface array is invalid", "value");
				resourceBytes = cost.Bytes + sizeof(ArrayValue);
			}
			const Value empty = int64_t{-4};
			const Value *input = c.Find("value");
			if (!input) input = &empty;
			if (!surface && !surfaces && !ValidValuePayload(*input, true))
				return c.Fail(Status::InvalidValue, "value cache input is invalid", "value");
			const bool replacing =
				capture && previous &&
				std::any_of(previous->Values.begin(), previous->Values.end(), [&](const auto &frame) {
					return frame.Frame == c.Request.Tick;
				});
			const size_t records = (previous ? previous->Values.size() : 0) + (capture && !replacing);
			uint64_t length = previous && !previous->Values.empty() ? previous->Values.back().Frame + 1 : 0;
			if (capture) length = std::max(length, c.Request.Tick + 1);
			if (length > Limits::MaximumArrayElements || records > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "value cache exceeds its frame budget");
			uint64_t bytes = sizeof(DataReplayEntry) +
							 std::max(c.Authored.Id.size(), std::string{}.capacity()) +
							 records * sizeof(DataReplayValueFrame) + length * sizeof(SourceArrayItem);
			const auto add = [&](const Value &value) {
				const auto clone = ValueClonePayloadBytes(value);
				const uint64_t remaining =
					Limits::MaximumEvaluationBytes - std::min(bytes, Limits::MaximumEvaluationBytes);
				const uint64_t storage = ItemStorage(value);
				if (!clone || storage > remaining || *clone > (remaining - storage) / 2) return false;
				const uint64_t extra = *clone * 2 + storage;
				bytes += extra;
				return true;
			};
			if (capture && resourceBytes) {
				if (resourceBytes >
					(Limits::MaximumEvaluationBytes - std::min(bytes, Limits::MaximumEvaluationBytes)) / 2)
					return c.Fail(Status::LimitExceeded, "value cache surface clone exceeds its budget");
				bytes += resourceBytes * 2;
			}
			if (capture && !resourceBytes && !add(*input))
				return c.Fail(Status::LimitExceeded, "value cache input clone exceeds its budget");
			if (previous)
				for (const auto &frame : previous->Values)
					if (!(capture && frame.Frame == c.Request.Tick) && !add(frame.Data))
						return c.Fail(Status::LimitExceeded, "value cache history clone exceeds its budget");
			if (!c.ReserveOutput(bytes, "cache_array")) return false;
			Value resource;
			if (capture && surface) {
				resource = SurfaceValue{*surface};
				input = &resource;
			} else if (capture && surfaces) {
				ArrayValue array{ValueType::Any, {}};
				array.Items = source_array::FromImages(*surfaces, surfaces->Items);
				resource = std::move(array);
				input = &resource;
			}
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Initialized = true;
			state.PreviousFrame = double(c.Request.Tick);
			state.Values.reserve(records);
			if (previous)
				for (const auto &frame : previous->Values)
					if (!(capture && frame.Frame == c.Request.Tick)) {
						state.Values.push_back(frame);
						// Previous-frame tags belong to a discarded evaluation namespace.
						StripSourcePathShiftIdentities(state.Values.back().Data);
					}
			if (capture) {
				if (resourceBytes)
					state.Values.push_back({c.Request.Tick, std::move(resource)});
				else
					state.Values.push_back({c.Request.Tick, *input});
			}
			std::sort(state.Values.begin(), state.Values.end(), [](const auto &a, const auto &b) {
				return a.Frame < b.Frame;
			});
			source_array::Items output(size_t(length), SourceArrayItem{ElementValue{double{0}}});
			for (const auto &frame : state.Values)
				output[size_t(frame.Frame)] = HistoryItem(frame.Data);
			if (!source_array::Publish(c, std::move(output), "cache_array", ValueType::Any)) return false;
			if (capture) c.DataUpdates.push_back(std::move(state));
			return c.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceCacheValueExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.cache_value_array", CacheValueArray}};
		return entries;
	}
} // namespace engine::imagegraph::detail
