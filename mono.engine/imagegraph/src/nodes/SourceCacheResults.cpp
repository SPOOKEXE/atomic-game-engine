#include "../CacheResultsSlots.hpp"
#include "ArraySource.hpp"

#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool CacheResults(NodeContext &c) {
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (!owner)
				return c.Fail(
					Status::UnsupportedExecution, "Cache Results needs a caller-owned data replay ledger"
				);
			if (owner->Entries.size() > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Cache Results replay row count exceeds bounds");
			{
				auto validation =
					c.ReserveWorkspace(owner->Entries.size() * sizeof(size_t), "cache_surfaces");
				if (!validation) return false;
				Diagnostic diagnostic;
				if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
					return c.Fail(diagnostic);
			}
			const DataReplayEntry *previous = nullptr;
			for (const auto &entry : owner->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) previous = &entry;
			const ArrayValue *old = nullptr;
			size_t index = 0;
			uint64_t oldBytes = 0;
			if (previous) {
				if (previous->Values.size() != 1 || previous->PreviousValue < 0 ||
					previous->PreviousValue > double(Limits::MaximumArrayElements - 1) ||
					std::floor(previous->PreviousValue) != previous->PreviousValue)
					return c.Fail(Status::InvalidValue, "Cache Results replay index or list is invalid");
				old = std::get_if<ArrayValue>(&previous->Values[0].Data);
				if (!CacheResultsSlots{old}.Valid(c.Request.MaximumImageDimension))
					return c.Fail(
						Status::InvalidValue, "Cache Results replay requires a valid flat owned slot list"
					);
				index = size_t(previous->PreviousValue);
				oldBytes = RetainedPayloadBytes(*old);
			}
			const int64_t amount = c.Integer("amount", 4);
			if (c.FailureCode != Status::Ok) return false;
			if (amount < 0 || uint64_t(amount) > Limits::MaximumArrayElements)
				return c.Fail(
					Status::LimitExceeded, "Cache Results Amount exceeds bounded array storage", "amount"
				);
			const auto firstFrame =
				c.Timeline ? SourceTimelineFirstFrame(*c.Timeline) : std::optional<double>{0.};
			if (!firstFrame) return c.Fail(Status::InvalidValue, "source first-frame bound is invalid");
			const bool first =
				FrameTimeToReal({c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame}) ==
				static_cast<long double>(*firstFrame);
			size_t retained = std::min(CacheResultsSlots{old}.Count(), size_t(amount));
			size_t begin = 0;
			if (first) {
				if (retained == size_t(amount)) {
					if (retained == 0)
						return c.Fail(
							Status::InvalidValue,
							"Zero Amount first frame reads a nonexistent source surface slot",
							"amount"
						);
					begin = 1;
					--retained;
				}
				index = retained;
			}
			const auto *value = c.Find("surface_in");
			if (value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution,
					"Cache Results Atlas-backed source drawing needs an exact atlas observation",
					"surface_in"
				);
			const auto *input = c.Input("surface_in");
			if (!input && value)
				if (const auto *surface = std::get_if<SurfaceValue>(value)) input = &surface->Data;
			if (!input && first)
				return c.Fail(
					Status::UnsupportedExecution,
					"First-frame Cache Results exposes an unwritten 1x1 source surface",
					"surface_in"
				);
			if (input &&
				(!ValidSurfaceLayout(*input, c.Request.MaximumImageDimension, Limits::MaximumOutputBytes) ||
				 !FiniteSurfaceSamples(*input)))
				return c.Fail(Status::InvalidValue, "Cache Results input surface is invalid", "surface_in");
			if (input && index > retained)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cache Results stale index would expose sparse source surface slots",
					"amount"
				);
			const size_t count = input ? std::max(retained, index + 1) : retained;
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Cache Results surface count exceeds bounds");
			uint64_t inputBytes = 0;
			if (input) {
				const auto layout = CheckedSurfaceLayout(
					input->Width, input->Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumArrayBytes
				);
				if (!layout)
					return c.Fail(
						Status::LimitExceeded, "Cache Results converted surface exceeds bounds", "surface_in"
					);
				inputBytes = layout->Bytes;
			}
			// Candidate Elements/Items normalization, the immutable journal and publication coexist.
			const uint64_t slots = std::max(count, CacheResultsSlots{old}.Count());
			const uint64_t fixed = sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
								   std::max(c.Authored.Id.size(), std::string{}.capacity()) +
								   slots * (sizeof(ElementValue) + 2 * sizeof(SourceArrayItem) +
											sizeof(Image) + sizeof(ImageArrayItem));
			if (fixed > Limits::MaximumEvaluationBytes ||
				oldBytes > (Limits::MaximumEvaluationBytes - fixed) / 3 ||
				inputBytes > (Limits::MaximumEvaluationBytes - fixed - oldBytes * 3) / 3)
				return c.Fail(Status::LimitExceeded, "Cache Results full-list clone exceeds bounds");
			if (!c.ReserveOutput(fixed + (oldBytes + inputBytes) * 3, "cache_surfaces")) return false;
			ArrayValue surfaces{ValueType::Image, {}};
			surfaces.Elements.reserve(count);
			for (size_t slot = 0; slot < count; ++slot) {
				if (input && slot == index) {
					Image image;
					image.Width = input->Width;
					image.Height = input->Height;
					image.Pixels.resize(size_t(inputBytes));
					for (uint32_t y = 0; y < image.Height; ++y)
						for (uint32_t x = 0; x < image.Width; ++x) {
							SurfacePixel pixel;
							if (!LoadSurfacePixel(*input, x, y, pixel) ||
								!StoreSurfacePixel(image, x, y, pixel))
								return c.Fail(
									Status::InvalidValue,
									"Cache Results numeric format conversion failed",
									"surface_in"
								);
						}
					image.Hash = SurfaceHash(image);
					surfaces.Elements.push_back(SurfaceValue{std::move(image)});
				} else {
					if (!old || slot + begin >= CacheResultsSlots{old}.Count())
						return c.Fail(Status::InvalidValue, "Cache Results retained surface slot is missing");
					surfaces.Elements.push_back(CacheResultsSlots{old}.Copy(slot + begin));
				}
			}
			const bool freed =
				std::any_of(surfaces.Elements.begin(), surfaces.Elements.end(), [](const auto &slot) {
					return IsFreedCacheResultsSlot(slot);
				});
			if (freed) {
				const bool onlyFreed =
					std::all_of(surfaces.Elements.begin(), surfaces.Elements.end(), [](const auto &slot) {
						return IsFreedCacheResultsSlot(slot);
					});
				if (onlyFreed)
					surfaces.ElementType = ValueType::Struct;
				else {
					surfaces.ElementType = ValueType::Any;
					surfaces.Items.reserve(count);
					for (auto &slot : surfaces.Elements)
						surfaces.Items.push_back({std::move(slot)});
					std::vector<ElementValue>{}.swap(surfaces.Elements);
				}
			}
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Subframe = c.Request.Subframe;
			state.NegativeFrame = c.Request.NegativeFrame;
			state.Initialized = true;
			state.PreviousValue = double(index);
			state.PreviousFrame = double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
			state.Values.reserve(1);
			state.Values.push_back({0, surfaces});
			source_array::Items output;
			output.reserve(count);
			if (!surfaces.Items.empty()) {
				for (auto &slot : surfaces.Items)
					output.push_back(std::move(slot));
			} else {
				for (auto &slot : surfaces.Elements) {
					if (auto *surface = std::get_if<SurfaceValue>(&slot))
						output.push_back({std::move(surface->Data)});
					else
						output.push_back({std::move(slot)});
				}
			}
			if (!source_array::Publish(c, std::move(output), "cache_surfaces", ValueType::Image))
				return false;
			c.DataUpdates.push_back(std::move(state));
			return c.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceCacheResultsExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.cache_results", CacheResults, true}};
		return entries;
	}
}
