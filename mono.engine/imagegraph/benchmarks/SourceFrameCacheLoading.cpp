#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.frame-cache-loading")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr uint64_t SLOTS = 64, BUDGET = 4 * 1024 * 1024;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	enum class Payload { Pixel, Noone, Nested };
	uint64_t PixelBytes(const std::vector<SourceArrayItem> &items) {
		uint64_t bytes = 0;
		for (const auto &item : items) {
			if (const auto *image = std::get_if<Image>(&item.Data)) bytes += image->Pixels.size();
			if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				bytes += PixelBytes(*children);
		}
		return bytes;
	}
	uint64_t PixelBytes(const Value &value) {
		if (const auto *surface = std::get_if<SurfaceValue>(&value)) return surface->Data.Pixels.size();
		if (const auto *tree = std::get_if<ArrayValue>(&value)) return PixelBytes(tree->Items);
		return 0;
	}
	struct Fixture {
		Node Owner;
		DataReplayEntry Inventory, Current;
		Diagnostic Failure;
		Payload Kind;
		uint64_t ReceiptBytes = 0, RetainedBytes = 0, PeakRowBytes = 0, Checksum = 0, DecodedPixelBytes = 0;
		explicit Fixture(bool array, Payload kind) : Kind(kind) {
			Owner.Id = "cache";
			Owner.Type = array ? "pc.cache_array" : "pc.cache";
			Inventory.NodeId = Owner.Id;
			Inventory.Initialized = true;
			Inventory.PreviousValue = 1;
			Inventory.LoadedCacheData = "[64 source slots with exact original width]";
			Inventory.SourceFrameCacheSerializedSlots = SLOTS;
			Inventory.Values = {{0, Owner.Type}, {1, int64_t{-4}}};
			Image pixel{2, 1, {0, 10, 13, 34, 92, 128, 255, 9}};
			pixel.Hash = SurfaceHash(pixel);
			for (uint64_t slot = 0; slot < SLOTS; ++slot) {
				if (kind == Payload::Noone || slot % 4 == 3) continue;
				Value value = SurfaceValue{pixel};
				if (kind == Payload::Nested) {
					ArrayValue tree{ValueType::Any, {}};
					tree.Items = {
						SourceArrayItem{pixel},
						SourceArrayItem{std::vector<SourceArrayItem>{
							SourceArrayItem{ElementValue{int64_t{-4}}}, SourceArrayItem{pixel}
						}}
					};
					value = std::move(tree);
				}
				Inventory.Values.push_back({slot + 2, std::move(value)});
			}
			ArrayValue chunks;
			Check(
				EncodeSourceFrameCacheReceipt(Inventory, chunks, Failure, BUDGET) == Status::Ok,
				"loading benchmark receipt encode refused"
			);
			for (const auto &chunk : chunks.Elements)
				ReceiptBytes += std::get<std::string>(chunk).size();
			Owner.SourceProperties = {
				{"cache", Inventory.LoadedCacheData},
				{"serialize", true},
				{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), Inventory.LoadedCacheData},
				{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(chunks)}
			};
		}
		void Run() {
			Current = {};
			PeakRowBytes = 0;
			DecodedPixelBytes = 0;
			Check(
				BeginNativeSourceFrameCacheLoading(Owner, Current, Failure, BUDGET) == Status::Ok,
				"loading benchmark constructor refused"
			);
			PeakRowBytes = RetainedDataReplayEntryBytes(Current);
			for (uint64_t slot = 0; slot < SLOTS; ++slot) {
				bool completed = false;
				Check(
					StepSourceFrameCacheLoading(Owner, SLOTS, Current, Current, completed, Failure, BUDGET) ==
						Status::Ok,
					"loading benchmark step refused"
				);
				Check(completed == (slot + 1 == SLOTS), "loading benchmark completion changed");
				PeakRowBytes = std::max(PeakRowBytes, RetainedDataReplayEntryBytes(Current));
				DecodedPixelBytes += PixelBytes(Current.Values.back().Data);
			}
			RetainedBytes = RetainedDataReplayEntryBytes(Current);
			Metrics::Count("imagegraph.loading_bench.decoded_pixel_bytes", double(DecodedPixelBytes));
			Metrics::Count("imagegraph.loading_bench.steps", SLOTS);
			Metrics::Count("imagegraph.loading_bench.packet_bytes", double(ReceiptBytes));
			Metrics::SetGauge("imagegraph.loading_bench.retained_row_bytes", double(RetainedBytes));
		}
		void Verify() {
			Check(
				Current.SourceFrameCacheLoading && !Current.SourceFrameCacheLoading->Loading &&
					Current.SourceFrameCacheLoading->NextSlot == SLOTS && Current.Values.size() == SLOTS + 2,
				"loading benchmark published width changed"
			);
			uint64_t hash = 14695981039346656037ull;
			const auto word = [&](uint64_t value) {
				for (size_t shift = 0; shift < 64; shift += 8)
					hash = (hash ^ ((value >> shift) & 255)) * 1099511628211ull;
			};
			for (uint64_t slot = 0; slot < SLOTS; ++slot) {
				const auto expected =
					std::find_if(Inventory.Values.begin(), Inventory.Values.end(), [&](const auto &frame) {
						return frame.Frame == slot + 2;
					});
				const Value value = expected == Inventory.Values.end() ? Value{int64_t{-4}} : expected->Data;
				Check(
					Current.Values[slot + 2].Frame == slot + 2 && Current.Values[slot + 2].Data == value,
					"loading benchmark decoded slot changed"
				);
				word(slot);
				word(value.index());
				if (const auto *surface = std::get_if<SurfaceValue>(&value)) word(SurfaceHash(surface->Data));
				if (const auto *tree = std::get_if<ArrayValue>(&value)) {
					word(tree->Items.size());
					word(SurfaceHash(std::get<Image>(tree->Items[0].Data)));
					const auto &children = std::get<std::vector<SourceArrayItem>>(tree->Items[1].Data);
					word(children.size());
					word(uint64_t(std::get<int64_t>(std::get<ElementValue>(children[0].Data))));
					word(SurfaceHash(std::get<Image>(children[1].Data)));
				}
			}
			if (Checksum) Check(Checksum == hash, "loading benchmark checksum changed");
			Checksum = hash;
			engine::testing::Consume(hash);
		}
	};
	struct Reading {
		float Milliseconds = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
		uint64_t RetainedBytes = 0, PeakRowBytes = 0;
	};
	struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		Profile(bool array, Payload kind) : Graph(array, kind) {}
		void Measure() {
			Check(Count < Readings.size(), "loading profile supports one to five samples");
			const bool previousEnabled = FrameGraph::IsEnabled();
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.loading_bench.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				FrameGraph::SetEnabled(previousEnabled);
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			reading.RetainedBytes = Graph.RetainedBytes;
			reading.PeakRowBytes = Graph.PeakRowBytes;
			Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"loading profile dropped scopes"
			);
			size_t begins = 0, steps = 0, decodes = 0;
			for (const auto &span : FrameGraph::Spans()) {
				begins += span.Name == "imagegraph.frame_cache.loading_begin";
				steps += span.Name == "imagegraph.frame_cache.loading_step";
				decodes += span.Name == "imagegraph.frame_cache.decode_receipt_slot";
				Check(!span.Reported, "loading CPU profile contains reported worker time");
				reading.Spans.emplace_back(std::string(span.Name), span);
			}
			Check(
				begins == 1 && steps == SLOTS && decodes == SLOTS,
				"loading profile missed constructor or slot steps"
			);
			Metrics::Drain();
			FrameGraph::SetEnabled(previousEnabled);
			Graph.Verify();
			if (Count)
				Check(
					reading.RetainedBytes == Readings[0].RetainedBytes &&
						reading.PeakRowBytes == Readings[0].PeakRowBytes,
					"loading repeated cycle changed retained state bounds"
				);
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_LOADING_PRESET");
			for (size_t index = 0; index < Count; ++index) {
				const auto &r = Readings[index];
				std::printf(
					"# loading-profile preset=%s backend=cpu owner=%s payload=%u slots=64 "
					"call=%zu warmup=%d state_fnv=%llu receipt_bytes=%llu retained_row_bytes=%llu "
					"decoded_pixel_bytes=%llu maximum_observed_row_bytes=%llu budget_bytes=%llu "
					"owner_ms=%.6f unmarked_ms=%.6f "
					"heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu process_live_bytes=%lld "
					"process_peak_bytes=%lld interval_live_bytes_delta=%lld profiler_overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					Graph.Owner.Type.c_str(),
					unsigned(Graph.Kind),
					index + 1,
					index < 8,
					(unsigned long long)Graph.Checksum,
					(unsigned long long)Graph.ReceiptBytes,
					(unsigned long long)r.RetainedBytes,
					(unsigned long long)Graph.DecodedPixelBytes,
					(unsigned long long)r.PeakRowBytes,
					(unsigned long long)BUDGET,
					r.Milliseconds,
					r.Unmarked,
					HeapProfile::IsCompiledIn(),
					(unsigned long long)(r.After.TotalBytes - r.Before.TotalBytes),
					(unsigned long long)(r.After.TotalBlocks - r.Before.TotalBlocks),
					(long long)r.After.LiveBytes,
					(long long)r.After.PeakBytes,
					(long long)(r.After.LiveBytes - r.Before.LiveBytes),
					(long long)r.After.OverheadBytes
				);
				for (size_t spanIndex = 0; spanIndex < r.Spans.size(); ++spanIndex) {
					const auto &[name, span] = r.Spans[spanIndex];
					std::printf(
						"# loading-span call=%zu index=%zu parent=%u depth=%u inclusive_ms=%.6f "
						"self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						index + 1,
						spanIndex,
						span.Parent,
						span.Depth,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						span.Reported,
						name.c_str()
					);
				}
			}
		}
	};
}
BENCH("CPU Cache native loading sixty-four pixel slots", 1) {
	static Profile p(false, Payload::Pixel);
	p.Measure();
}
BENCH("CPU Cache native loading sixty-four noone slots", 1) {
	static Profile p(false, Payload::Noone);
	p.Measure();
}
BENCH("CPU Cache native loading sixty-four nested slots", 1) {
	static Profile p(false, Payload::Nested);
	p.Measure();
}
BENCH("CPU Cache Array native loading sixty-four pixel slots", 1) {
	static Profile p(true, Payload::Pixel);
	p.Measure();
}
BENCH("CPU Cache Array native loading sixty-four noone slots", 1) {
	static Profile p(true, Payload::Noone);
	p.Measure();
}
BENCH("CPU Cache Array native loading sixty-four nested slots", 1) {
	static Profile p(true, Payload::Nested);
	p.Measure();
}
