#pragma once

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::imagegraphfont::testing {
	// Fixed snapshots reuse the normal benchmark collector; verification and reporting are outside frames.
	template <class Workload> struct FontBoundaryProfile {
		using FrameGraph = core::FrameGraph;
		using HeapProfile = core::HeapProfile;
		struct Span {
			std::string Name;
			core::FrameSpan Data;
		};
		struct Reading {
			float OwnerMilliseconds = 0;
			uint64_t OutputHash = 0;
			core::HeapTotals Before{}, After{};
			std::vector<core::HeapNodeView> NodesBefore, NodesAfter;
			std::vector<Span> Spans;
			std::vector<core::Counter> Counters;
		};
		Workload Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		std::string Label;
		std::vector<std::string_view> Required;
		template <class Kind>
		FontBoundaryProfile(std::string label, Kind kind, std::initializer_list<std::string_view> required)
			: Graph(kind), Label(std::move(label)), Required(required) {}
		void Measure() {
			if (Count == Readings.size())
				throw std::runtime_error("font boundary maximum five samples after eight warmups");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			core::Metrics::Drain();
			FrameGraph::SetEnabled(true);
			auto &reading = Readings[Count];
			reading.NodesBefore.reserve(HeapProfile::MAXIMUM_NODES);
			reading.NodesAfter.reserve(HeapProfile::MAXIMUM_NODES);
			reading.Spans.reserve(128);
			for (uint32_t index = 0; index < HeapProfile::NodeCount(); ++index)
				reading.NodesBefore.push_back(HeapProfile::Node(index));
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraphfont.boundary");
				if (!Graph.Run()) throw std::runtime_error("font boundary operation refused");
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.OwnerMilliseconds = FrameGraph::FrameMilliseconds();
			for (uint32_t index = 0; index < HeapProfile::NodeCount(); ++index)
				reading.NodesAfter.push_back(HeapProfile::Node(index));
			uint64_t taggedBytes = 0, taggedBlocks = 0;
			for (size_t index = 0; index < reading.NodesAfter.size(); ++index) {
				const auto before =
					index < reading.NodesBefore.size() ? reading.NodesBefore[index] : core::HeapNodeView{};
				taggedBytes += reading.NodesAfter[index].TotalBytes - before.TotalBytes;
				taggedBlocks += reading.NodesAfter[index].TotalBlocks - before.TotalBlocks;
			}
			if (taggedBytes != reading.After.TotalBytes - reading.Before.TotalBytes ||
				taggedBlocks != reading.After.TotalBlocks - reading.Before.TotalBlocks ||
				reading.After.DroppedScopes != reading.Before.DroppedScopes || FrameGraph::Dropped())
				throw std::runtime_error("font boundary incomplete heap/frame capture");
			const auto spans = FrameGraph::Spans();
			for (const auto required : Required)
				if (std::none_of(spans.begin(), spans.end(), [&](const auto &span) {
						return span.Name == required;
					}))
					throw std::runtime_error("font boundary missing phase " + std::string(required));
			for (size_t index = 0; index < spans.size(); ++index) {
				const auto &span = spans[index];
				if (span.Reported || (span.Parent != FrameGraph::NO_PARENT &&
									  (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1)))
					throw std::runtime_error("font boundary invalid owner hierarchy");
				reading.Spans.push_back({std::string(span.Name), span});
			}
			reading.Counters = core::Metrics::Drain();
			if constexpr (requires { Graph.VerifyCounters(reading.Counters); })
				Graph.VerifyCounters(reading.Counters);
			reading.OutputHash = Graph.Verify();
			if (Count && reading.OutputHash != Readings[0].OutputHash)
				throw std::runtime_error("font boundary changed semantic output");
			engine::testing::Consume(reading.OutputHash);
			++Count;
		}
		~FontBoundaryProfile() {
			for (size_t call = 0; call < Count; ++call) {
				const auto &r = Readings[call];
				std::printf(
					"# font-boundary operation=%s call=%zu input_fnv=%llu output_fnv=%llu owner_ms=%.6f "
					"heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu live_bytes=%lld "
					"peak_bytes=%lld overhead_bytes=%lld\n",
					Label.c_str(),
					call + 1,
					static_cast<unsigned long long>(Graph.InputHash),
					static_cast<unsigned long long>(r.OutputHash),
					r.OwnerMilliseconds,
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(r.After.TotalBytes - r.Before.TotalBytes),
					static_cast<unsigned long long>(r.After.TotalBlocks - r.Before.TotalBlocks),
					static_cast<long long>(r.After.LiveBytes),
					static_cast<long long>(r.After.PeakBytes),
					static_cast<long long>(r.After.OverheadBytes)
				);
				for (size_t index = 0; index < r.Spans.size(); ++index) {
					const auto &span = r.Spans[index];
					std::printf(
						"# font-span operation=%s call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						Label.c_str(),
						call + 1,
						index,
						span.Data.Parent,
						span.Data.Depth,
						span.Data.StartMilliseconds,
						span.Data.Milliseconds,
						span.Data.SelfMilliseconds,
						span.Data.IdleMilliseconds,
						span.Data.Reported,
						span.Name.c_str()
					);
				}
				for (const auto &counter : r.Counters)
					std::printf(
						"# font-counter operation=%s call=%zu name=%.*s value=%.0f samples=%u\n",
						Label.c_str(),
						call + 1,
						int(counter.Name.Text().size()),
						counter.Name.Text().data(),
						counter.Value,
						counter.Samples
					);
				for (size_t index = 0; index < r.NodesAfter.size(); ++index) {
					const auto &after = r.NodesAfter[index];
					const auto before =
						index < r.NodesBefore.size() ? r.NodesBefore[index] : core::HeapNodeView{};
					if (after.TotalBytes == before.TotalBytes && after.TotalBlocks == before.TotalBlocks &&
						after.LiveBytes == before.LiveBytes)
						continue;
					std::printf(
						"# font-heap operation=%s call=%zu index=%zu parent=%u depth=%u allocated_bytes=%llu "
						"allocated_blocks=%llu live_bytes=%lld live_blocks=%lld peak_bytes=%lld name=%.*s\n",
						Label.c_str(),
						call + 1,
						index,
						after.Parent,
						after.Depth,
						static_cast<unsigned long long>(after.TotalBytes - before.TotalBytes),
						static_cast<unsigned long long>(after.TotalBlocks - before.TotalBlocks),
						static_cast<long long>(after.LiveBytes),
						static_cast<long long>(after.LiveBlocks),
						static_cast<long long>(after.PeakBytes),
						int(after.Name.size()),
						after.Name.data()
					);
				}
			}
		}
	};
}
