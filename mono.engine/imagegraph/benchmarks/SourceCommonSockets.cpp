#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCommonSockets.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.common-sockets")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t OWNERS = 64, STEPS = 16;
	constexpr uint64_t BUDGET = 1024 * 1024;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	struct Fixture {
		std::array<std::string, OWNERS> Ids, Names;
		std::array<SourceCommonOwner, OWNERS> Owners;
		SourceCommonSocketSession Session;
		std::vector<SourceCommonStepReceipt> Receipts;
		Diagnostic Failure;
		uint64_t RetainedBytes = 0, StateFnv = 0;
		size_t Resets = 0;
		Fixture() {
			for (size_t index = 0; index < OWNERS; ++index) {
				Ids[index] = "owner" + std::to_string(index);
				Names[index] = "name" + std::to_string(index);
				Owners[index] = {
					Ids[index],
					index % 2 ? "Collection" : "pc.number_simple",
					index % 4 != 0,
					index % 2 == 0,
					index % 3 != 0
				};
			}
			Run();
			const auto bytes = RetainedBytes, checksum = StateFnv;
			Run();
			Check(bytes == RetainedBytes && checksum == StateFnv, "common repeated cycle changed");
		}
		void Run() {
			Session = {};
			Receipts = {};
			Resets = 0;
			Check(
				InitializeSourceCommonSockets(Owners, Session, Failure, BUDGET) == Status::Ok,
				"common constructor refused"
			);
			Check(Session.Owners.size() == OWNERS, "common owner count changed");
			for (const auto &state : Session.Owners)
				Check(
					!state.Updated && state.Name.empty() && state.Position == Vector2{},
					"common constructor socket changed"
				);
			auto expected = Session;
			std::array<SourceCommonFullUpdateCapture, OWNERS> completed;
			for (size_t index = 0; index < OWNERS; ++index) {
				const bool safe = index % 8 == 1 || index % 8 == 2;
				completed[index] = {
					Owners[index].OwnerId,
					Owners[index].OwnerType,
					safe,
					safe ? std::optional<bool>{} : std::optional<bool>{true}
				};
			}
			std::vector<SourceCommonStepCapture> captures;
			for (size_t step = 0; step < STEPS; ++step) {
				captures.clear();
				std::vector<SourceCommonStepReceipt> resets;
				for (size_t index = 0; index < OWNERS; ++index) {
					const auto &owner = Owners[index];
					if (!owner.Active) continue;
					const bool requested = (step + index) % 3 == 0;
					captures.push_back(
						{owner.OwnerId,
						 owner.OwnerType,
						 requested,
						 true,
						 SourceCommonMetadataCapture{Names[index], {double(step), double(index)}}}
					);
					if (owner.ShowUpdateTrigger) {
						expected.Owners[index].Updated = false;
						if (requested) resets.push_back({Ids[index], true});
					}
					if (owner.OutMeta) {
						expected.Owners[index].Name = Names[index];
						expected.Owners[index].Position = {double(step), double(index)};
					}
				}
				Check(
					BeginSourceCommonStep(Owners, captures, Session, Receipts, Failure, BUDGET) == Status::Ok,
					"common ordered step refused"
				);
				Check(Session == expected && Receipts == resets, "common step state or resets changed");
				Resets += resets.size();
				Check(
					CompleteSourceCommonFullUpdates(completed, Session, Failure, BUDGET) == Status::Ok,
					"common full completion refused"
				);
				for (size_t index = 0; index < OWNERS; ++index)
					if (!completed[index].SafeMode) expected.Owners[index].Updated = true;
				Check(Session == expected, "common completion held state changed");
			}
			const auto oldReceipts = Receipts;
			RetainedBytes =
				RetainedSourceCommonSocketBytes(Session) + RetainedSourceCommonReceiptBytes(Receipts);
			Check(
				BeginSourceCommonStep(Owners, captures, Session, Receipts, Failure, 1) ==
					Status::LimitExceeded,
				"common budget refusal changed"
			);
			Check(Session == expected && Receipts == oldReceipts, "common budget refusal published state");
			completed.back().Completed.reset();
			Check(
				CompleteSourceCommonFullUpdates(completed, Session, Failure, BUDGET) ==
					Status::UnsupportedExecution,
				"common unknown completion accepted"
			);
			Check(
				Session == expected && Receipts == oldReceipts, "common unknown completion published state"
			);
			Check(
				RetainedBytes ==
					RetainedSourceCommonSocketBytes(Session) + RetainedSourceCommonReceiptBytes(Receipts),
				"common refusal byte accounting changed"
			);
			StateFnv = 14695981039346656037ull;
			const auto byte = [&](unsigned char value) { StateFnv = (StateFnv ^ value) * 1099511628211ull; };
			for (const auto &state : Session.Owners) {
				for (const auto &text : {state.OwnerId, state.OwnerType, state.Name}) {
					for (unsigned char value : text)
						byte(value);
					byte(0);
				}
				byte(state.Updated);
				for (double coordinate : {state.Position.X, state.Position.Y}) {
					const auto bits = std::bit_cast<uint64_t>(coordinate);
					for (size_t shift = 0; shift < 64; shift += 8)
						byte((bits >> shift) & 255);
				}
			}
			Metrics::Count("imagegraph.common_bench.steps", STEPS);
			Metrics::Count("imagegraph.common_bench.resets", double(Resets));
			Metrics::SetGauge("imagegraph.common_bench.retained_bytes", double(RetainedBytes));
			engine::testing::Consume(StateFnv);
		}
	};
	struct Reading {
		float Milliseconds = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
	};
	struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		void Measure() {
			Check(Count < Readings.size(), "common profile supports one to five samples");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count++];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.common_bench.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"common profile dropped scopes"
			);
			for (const auto &span : FrameGraph::Spans())
				reading.Spans.emplace_back(std::string(span.Name), span);
			Metrics::Drain();
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_COMMON_PRESET");
			for (size_t call = 0; call < Count; ++call) {
				const auto &reading = Readings[call];
				std::printf(
					"# common-profile preset=%s backend=cpu owners=64 steps=16 "
					"completions=16 "
					"refusals=2 resets=%zu state_fnv=%llu budget_bytes=%llu call=%zu "
					"warmup=%d "
					"owner_ms=%.6f unmarked_ms=%.6f retained_bytes=%llu heap_compiled=%d "
					"allocated_bytes=%llu allocated_blocks=%llu live_bytes=%lld "
					"peak_bytes=%lld "
					"profiler_overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					Graph.Resets,
					static_cast<unsigned long long>(Graph.StateFnv),
					static_cast<unsigned long long>(BUDGET),
					call + 1,
					call < 8,
					reading.Milliseconds,
					reading.Unmarked,
					static_cast<unsigned long long>(Graph.RetainedBytes),
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(reading.After.TotalBytes - reading.Before.TotalBytes),
					static_cast<unsigned long long>(reading.After.TotalBlocks - reading.Before.TotalBlocks),
					static_cast<long long>(reading.After.LiveBytes),
					static_cast<long long>(reading.After.PeakBytes),
					static_cast<long long>(reading.After.OverheadBytes)
				);
				for (size_t index = 0; index < reading.Spans.size(); ++index) {
					const auto &[name, span] = reading.Spans[index];
					std::printf(
						"# common-span call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						call + 1,
						index,
						span.Parent,
						span.Depth,
						span.StartMilliseconds,
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
} // namespace
BENCH(
	"CPU sixty-four common socket owners ordered steps held metadata and "
	"atomic refusal",
	1
) {
	static Profile profile;
	profile.Measure();
}
