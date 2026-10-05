#include "ArraySource.hpp"

#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>

namespace engine::imagegraph::detail {
	namespace {
		bool Metadata(NodeContext &c, const std::string *&saved) {
			if (!c.Request.SourceCachePlayback || !c.Request.SourceCachePlayback->SynchronousProducer)
				return c.Fail(
					Status::UnsupportedExecution,
					"source frame cache requires explicit synchronous playback observations"
				);
			const auto &observation = *c.Request.SourceCachePlayback;
			if (observation.Sampling != SourceCacheSampling::ObservedFrame &&
				observation.Sampling != SourceCacheSampling::NativePlayedPrefix)
				return c.Fail(Status::InvalidValue, "invalid source frame-cache sampling profile");
			if (observation.Sampling == SourceCacheSampling::NativePlayedPrefix && !observation.Playing)
				return c.Fail(Status::InvalidValue, "native played-prefix sampling requires Playing=true");
			bool serialize = true;
			const ArrayValue *group = nullptr;
			saved = nullptr;
			std::array<bool, 5> seen{};
			for (const auto &property : c.Authored.SourceProperties) {
				size_t index;
				if (property.Port == "serialize")
					index = 0;
				else if (property.Port == "cache_group")
					index = 1;
				else if (property.Port == "cache")
					index = 2;
				else if (property.Port == SOURCE_FRAME_CACHE_NATIVE_TEXT)
					index = 3;
				else if (property.Port == SOURCE_FRAME_CACHE_NATIVE_DATA)
					index = 4;
				else
					return c.Fail(Status::InvalidValue, "unknown source frame-cache metadata", property.Port);
				if (seen[index])
					return c.Fail(
						Status::InvalidValue, "duplicate source frame-cache metadata", property.Port
					);
				seen[index] = true;
				if (index == 0) {
					const auto *value = std::get_if<bool>(&property.Data);
					if (!value)
						return c.Fail(
							Status::InvalidValue, "Serialize must be a source boolean", property.Port
						);
					serialize = *value;
				} else if (index == 1) {
					group = std::get_if<ArrayValue>(&property.Data);
					if (!group || group->ElementType != ValueType::Text || !group->Items.empty() ||
						!group->Nested.empty() || group->Elements.size() > Limits::MaximumNodes ||
						!ValidValuePayload(property.Data, true))
						return c.Fail(
							Status::InvalidValue, "Cache Group must be a bounded string array", property.Port
						);
				} else if (index == 2 || index == 3) {
					const auto *text = std::get_if<std::string>(&property.Data);
					if (!text)
						return c.Fail(
							Status::InvalidValue, "serialized source cache must be text", property.Port
						);
					if (index == 2) saved = text;
				} else if (!std::holds_alternative<ArrayValue>(property.Data))
					return c.Fail(
						Status::InvalidValue, "native frame cache packet must be an array", property.Port
					);
			}
			if (serialize && group && !group->Elements.empty())
				return c.Fail(
					Status::UnsupportedExecution, "source cache-group scheduling requires host capture"
				);
			if (!serialize) saved = nullptr;
			if (saved && saved->empty())
				return c.Fail(Status::InvalidValue, "enabled serialized source cache text is empty", "cache");
			if (c.Request.NegativeFrame || c.Request.Subframe != 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"native frame-cache storage requires integer nonnegative source frames"
				);
			return true;
		}
		struct InputView {
			const Value *Data = nullptr;
			const Image *Surface = nullptr;
			const ImageArray *Images = nullptr;
			uint64_t Bytes = 0;
		};
		uint64_t NormalizationBytes(const Value &value) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return source_array::PackedCount(value) * sizeof(SourceArrayItem);
			if (!array->Items.empty()) {
				source_array::TreeCost cost;
				return source_array::MeasureSource(array->Items, cost) ? cost.Bytes : UINT64_MAX;
			}
			uint64_t nodes = array->Nested.empty() ? array->Elements.size() : array->Nested.size();
			const auto measure = [&](const auto &elements) {
				for (const auto &leaf : elements)
					nodes += source_array::PackedCount(leaf);
				nodes += elements.size();
			};
			if (array->Nested.empty()) {
				nodes = 0;
				measure(array->Elements);
			} else
				for (const auto &row : array->Nested)
					measure(row);
			return nodes <= Limits::MaximumArrayElements ? nodes * sizeof(SourceArrayItem) : UINT64_MAX;
		}
		bool ReadInput(NodeContext &c, InputView &input) {
			input.Data = c.Find("surface_in");
			input.Surface = c.Input("surface_in");
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "surface_in") input.Images = array;
			if (input.Images) {
				source_array::TreeCost cost;
				if (!source_array::ImageCost(*input.Images, input.Images->Items, cost, 1))
					return c.Fail(
						Status::LimitExceeded, "frame-cache surface tree exceeds bounds", "surface_in"
					);
				for (const auto &image : input.Images->Images)
					if (!ValidSurfaceLayout(
							image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes
						) ||
						!FiniteSurfaceSamples(image))
						return c.Fail(
							Status::InvalidValue,
							"frame-cache surface tree has nonfinite pixels",
							"surface_in"
						);
				input.Bytes = cost.Bytes + sizeof(ArrayValue);
			} else if (input.Surface) {
				if (!ValidSurfaceLayout(
						*input.Surface, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes
					) ||
					!FiniteSurfaceSamples(*input.Surface))
					return c.Fail(
						Status::InvalidValue, "frame-cache surface storage is invalid", "surface_in"
					);
				input.Bytes = sizeof(Image) + input.Surface->Pixels.size();
			} else if (input.Data) {
				const auto bytes = ValueClonePayloadBytes(*input.Data);
				if (!bytes || !ValidValuePayload(*input.Data, true))
					return c.Fail(Status::InvalidValue, "frame-cache input payload is invalid", "surface_in");
				input.Bytes = *bytes;
			} else
				return c.Fail(
					Status::InvalidValue, "frame-cache linked source input is missing", "surface_in"
				);
			return true;
		}
		Value CopyInput(const InputView &input) {
			if (input.Images) {
				ArrayValue value{ValueType::Any, {}};
				value.Items = source_array::FromImages(*input.Images, input.Images->Items);
				return value;
			}
			if (input.Surface) return SurfaceValue{*input.Surface};
			return *input.Data;
		}
		bool Publish(NodeContext &c, const Value &value, std::string_view port) {
			if (const auto *image = std::get_if<SurfaceValue>(&value)) {
				auto *out = c.NewImage(port, image->Data.Width, image->Data.Height, image->Data.Format);
				if (!out) return false;
				out->Pixels = image->Data.Pixels;
				out->Hash = SurfaceHash(*out);
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value))
				return source_array::Publish(c, source_array::FromValues(*array), port, ValueType::Image);
			c.SetValue(port, value);
			return c.FailureCode == Status::Ok;
		}
		const Value *FindStoredFrame(const DataReplayEntry *state, uint64_t tick) {
			if (!state) return nullptr;
			const auto key = tick + 2;
			const auto position = std::lower_bound(
				state->Values.begin() + 2, state->Values.end(), key, [](const auto &frame, uint64_t index) {
					return frame.Frame < index;
				}
			);
			return position != state->Values.end() && position->Frame == key ? &position->Data : nullptr;
		}
		const Value *FindFrame(const DataReplayEntry *state, uint64_t tick) {
			const auto *value = FindStoredFrame(state, tick);
			// Source cacheExist accepts an array or an existing surface, never its noone sentinel.
			return value && (std::holds_alternative<SurfaceValue>(*value) ||
							 std::holds_alternative<ArrayValue>(*value))
					   ? value
					   : nullptr;
		}
		bool Execute(NodeContext &c, bool array) {
			ENGINE_PROFILE("imagegraph.source.frame_cache");
			const std::string *saved = nullptr;
			if (!Metadata(c, saved)) return false;
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (!owner || owner->Entries.size() > Limits::MaximumArrayElements)
				return c.Fail(
					Status::UnsupportedExecution,
					"source frame cache needs a bounded caller-owned data replay ledger"
				);
			{
				auto workspace = c.ReserveWorkspace(DataReplayValidationWorkspaceBytes(*owner));
				if (!workspace) return false;
				Diagnostic diagnostic;
				if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
					return c.Fail(diagnostic);
			}
			const DataReplayEntry *previous = nullptr;
			bool loadedNow = false;
			bool constructorCleared = false;
			for (const auto &entry : owner->Entries) {
				if (entry.NodeId == c.Authored.Id && entry.FrameCacheConstructorCleared &&
					SourceFrameCacheRowType(entry) == c.Authored.Type &&
					entry.LoadedCacheData == (saved ? *saved : std::string_view{}))
					constructorCleared = true;
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) {
					if (SourceFrameCacheRowType(entry) == c.Authored.Type)
						previous = &entry;
					else if (SourceFrameCacheRowType(entry).empty())
						return c.Fail(Status::InvalidValue, "source frame-cache replay tag is invalid");
				}
			}
			if (previous && previous->LoadedCacheData != (saved ? *saved : std::string_view{}))
				previous = nullptr;
			if (saved && !previous && !constructorCleared && c.Request.SourceFrameCacheLoads) {
				for (const auto &entry : c.Request.SourceFrameCacheLoads->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) {
						if (SourceFrameCacheRowType(entry) != c.Authored.Type ||
							entry.LoadedCacheData != *saved || entry.NegativeFrame || entry.Subframe != 0)
							return c.Fail(
								Status::InvalidValue,
								"saved frame cache receipt does not match authored source"
							);
						previous = &entry;
						loadedNow = true;
					}
			}
			auto nativeCharge = c.ReserveWorkspace(0, "cache");
			if (!nativeCharge) return false;
			DataReplayEntry nativeRow;
			if (saved && !previous && !constructorCleared) {
				uint64_t bytes = 0;
				Diagnostic diagnostic;
				const auto measured = MeasureSourceFrameCacheReceipt(c.Authored, bytes, diagnostic);
				if (measured != Status::Ok && measured != Status::UnsupportedExecution)
					return c.Fail(diagnostic);
				if (measured == Status::Ok) {
					bytes += RetainedDataReplayEntryBytes(nativeRow);
					if (bytes > c.AvailableBytes() || !nativeCharge->Resize(bytes))
						return c.Fail(
							Status::LimitExceeded, "native frame cache load exceeds live byte budget", "cache"
						);
					if (DecodeSourceFrameCacheReceipt(c.Authored, nativeRow, diagnostic, bytes) != Status::Ok)
						return c.Fail(diagnostic);
					nativeRow.ProcessorRow = c.ProcessorRow;
					previous = &nativeRow;
					loadedNow = true;
				}
			}
			if (saved && !previous && !constructorCleared)
				return c.Fail(
					Status::UnsupportedExecution,
					"serialized source cache requires an exact-data decoded load receipt"
				);
			const uint64_t total = c.Timeline ? c.Timeline->Frames : 1;
			if (total > Limits::MaximumArrayElements - 2)
				return c.Fail(
					Status::LimitExceeded, "source frame-cache frame storage exceeds native bounds"
				);
			const Value empty = array ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}};
			const Value *output = previous ? SourceFrameCacheLastOutput(*previous) : &empty;
			bool capture = false;
			uint64_t count = 0, first = 0, last = 0, step = 1;
			// Cache loads exactly TOTAL_FRAMES slots. Cache Array restores every serialized slot.
			const uint64_t loadLimit = loadedNow && !array ? total + 2 : UINT64_MAX;
			const auto *hit = c.Request.Tick + 2 < loadLimit ? FindFrame(previous, c.Request.Tick) : nullptr;
			double animated = previous ? previous->PreviousValue : 1;
			if (!array && !hit) {
				animated = c.Boolean("animated") ? 1 : 0;
				if (c.FailureCode != Status::Ok) return false;
			}
			if (!array) {
				if (hit)
					output = hit;
				else
					capture = true;
			} else if (c.Request.SourceCachePlayback->Playing &&
					   c.FrameCacheInputReads == SourceFrameCacheInputReads::All) {
				const int64_t start = c.Integer("start_frame", -1), stop = c.Integer("stop_frame", -1),
							  stride = c.Integer("step", 1);
				if (c.FailureCode != Status::Ok) return false;
				const int64_t begin = start < 0 ? 0 : start - 1, end = stop < 0 ? int64_t(total) : stop;
				// Source checks invalid range before the clock, and leaves the output unchanged on every
				// return.
				if (end >= begin && stride > 0 && int64_t(c.Request.Tick) >= begin &&
					int64_t(c.Request.Tick) < end) {

					first = uint64_t(begin);
					last = uint64_t(end);
					step = uint64_t(stride);
					count = (last - first - 1) / step + 1;
					if (count > Limits::MaximumArrayElements)
						return c.Fail(Status::LimitExceeded, "Cache Array output count exceeds bounds");
					capture = true;
				}
			}
			const bool useInput = capture && c.Request.SourceCachePlayback->Playing &&
								  c.FrameCacheInputReads == SourceFrameCacheInputReads::All;
			InputView input;
			if (useInput && !ReadInput(c, input)) return false;
			const bool writeFrame = capture && c.Request.Tick <= total;
			// Auto-cache hits skip cacheCurrentFrame. Misses resize first, including paused misses
			// and captures beyond the current duration; manual Cache Array resizes only on capture.
			const uint64_t frameLimit = capture ? std::min(loadLimit, total + 3) : loadLimit;
			size_t records = 2 + (writeFrame ? 1 : 0);
			if (previous)
				for (size_t i = 2; i < previous->Values.size(); ++i)
					if (previous->Values[i].Frame < frameLimit &&
						(!writeFrame || previous->Values[i].Frame != c.Request.Tick + 2))
						++records;
			if (records > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "frame-cache history exceeds native frame count");
			const uint64_t oldBytes = previous ? RetainedDataReplayEntryBytes(*previous) : 0;
			const auto lastBytes = ValueClonePayloadBytes(*output);
			if (!lastBytes)
				return c.Fail(Status::LimitExceeded, "frame-cache latest output exceeds clone bounds");
			const uint64_t fixed =
				sizeof(DataReplayEntry) + std::max(c.Authored.Id.size(), std::string{}.capacity()) +
				records * sizeof(DataReplayValueFrame) +
				(saved ? std::max(saved->size(), std::string{}.capacity()) : std::string{}.capacity()) +
				2 * (c.Authored.Type.size() + std::string{}.capacity()) +
				(count + total) * 2 * sizeof(SourceArrayItem);
			uint64_t normalization = NormalizationBytes(*output);
			if (previous)
				for (const auto &frame : previous->Values) {
					const auto bytes = NormalizationBytes(frame.Data);
					if (bytes > Limits::MaximumEvaluationBytes -
									std::min(normalization, Limits::MaximumEvaluationBytes))
						return c.Fail(Status::LimitExceeded, "frame-cache normalization exceeds bounds");
					normalization += bytes;
				}
			if (useInput && input.Data) {
				const auto bytes = NormalizationBytes(*input.Data);
				if (bytes >
					Limits::MaximumEvaluationBytes - std::min(normalization, Limits::MaximumEvaluationBytes))
					return c.Fail(Status::LimitExceeded, "frame-cache input normalization exceeds bounds");
				normalization += bytes;
			}
			const uint64_t each = oldBytes + input.Bytes + *lastBytes + normalization;
			if (fixed > Limits::MaximumEvaluationBytes || each > (Limits::MaximumEvaluationBytes - fixed) / 6)
				return c.Fail(Status::LimitExceeded, "frame-cache generations exceed clone bounds");
			// Six full copies cover history replacement, normalization, latest output and published
			// generation.
			if (!c.ReserveOutput(fixed + each * 6, array ? "cache_array" : "cache_surface")) return false;
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			if (saved) state.LoadedCacheData = *saved;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Initialized = true;
			state.FrameCacheConstructorCleared = constructorCleared;
			state.PreviousValue = animated;
			state.PreviousFrame = double(c.Request.Tick);
			state.Values.reserve(records);
			state.Values.push_back({0, c.Authored.Type});
			state.Values.push_back({1, *output});
			if (previous)
				for (size_t i = 2; i < previous->Values.size(); ++i) {
					if (previous->Values[i].Frame < 2)
						return c.Fail(Status::InvalidValue, "frame-cache retained position is invalid");
					if (previous->Values[i].Frame >= frameLimit) continue;
					if (!writeFrame || previous->Values[i].Frame != c.Request.Tick + 2)
						state.Values.push_back(previous->Values[i]);
				}
			if (writeFrame)
				state.Values.push_back({c.Request.Tick + 2, useInput ? CopyInput(input) : *output});
			std::sort(state.Values.begin() + 2, state.Values.end(), [](const auto &a, const auto &b) {
				return a.Frame < b.Frame;
			});
			if (!array) {
				if (writeFrame)
					state.Values[1].Data = *FindStoredFrame(&state, c.Request.Tick);
				else if (useInput)
					state.Values[1].Data = CopyInput(input);
			} else if (capture) {
				source_array::Items items;
				items.reserve(size_t(count));
				for (uint64_t frame = first; frame < last; frame += step) {
					const Value *value = FindFrame(&state, frame);
					if (!value)
						items.push_back({ElementValue{int64_t{-1}}});
					else if (const auto *surface = std::get_if<SurfaceValue>(value))
						items.push_back({surface->Data});
					else if (const auto *members = std::get_if<ArrayValue>(value))
						items.push_back({source_array::FromValues(*members)});
					else
						items.push_back(source_array::SourceLeaf(*value));
				}
				ArrayValue value{ValueType::Any, {}};
				value.Items = std::move(items);
				state.Values[1].Data = std::move(value);
			}
			if (!Publish(c, state.Values[1].Data, array ? "cache_array" : "cache_surface")) return false;
			c.DataUpdates.push_back(std::move(state));
			return c.FailureCode == Status::Ok;
		}
		bool Cache(NodeContext &c) {
			return Execute(c, false);
		}
		bool CacheArray(NodeContext &c) {
			return Execute(c, true);
		}
	}
	std::span<const ExecutorEntry> SourceFrameCacheExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.cache", Cache, true}, {"pc.cache_array", CacheArray, true}
		};
		return entries;
	}
}
