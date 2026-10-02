// Paired stress-scene and floor ticks in both orders. Index diagnostics and exact
// output comparisons run after each workload's owner frames.
#include "../../spatial/tests/fixtures/GridDiagnostics.hpp"
#include "../tests/fixtures/CellSizeParity.hpp"
#include "../tests/fixtures/SteppingScene.hpp"
#include "PipelineInternals.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.cell-size-counterbalance")

namespace {
	using namespace engine;
	constexpr std::array<std::string_view, 4> REQUIRED_PHASES{
		"physics.sync-broadphase", "physics.broadphase", "physics.narrowphase", "physics.solve"
	};

	struct RestoreSettings {
		bool Serial = parallel::ForceSerialCompute();
		bool Profile = core::FrameGraph::IsEnabled();
		~RestoreSettings() {
			core::FrameGraph::SetEnabled(Profile);
			parallel::SetForceSerialCompute(Serial);
		}
	};

	struct SpanReading {
		std::string Name;
		core::FrameSpan Span;
	};

	struct Reading {
		float OwnerMilliseconds = 0.0f;
		size_t Dropped = 0;
		core::HeapTotals HeapBefore;
		core::HeapTotals HeapAfter;
		std::vector<SpanReading> Spans;
		std::array<float, REQUIRED_PHASES.size()> PhaseMilliseconds{};
		size_t Pairs = 0;
		size_t Manifolds = 0;
		size_t Events = 0;
		bool Tree = false;
		physics::PhysicsMemoryStats Memory;
		std::array<spatial::HashGridStats, 3> GridMemory{};
		std::array<spatial::testing::GridReading, 3> Grids{};
	};

	struct Arm {
		std::string_view Scene;
		float Cell;
		size_t Bodies;
		std::unique_ptr<ecs::Store> Store;
		std::array<Reading, 13> Readings{};

		Arm(std::string_view scene, physics::testing::Layout layout, float cell, size_t bodies)
			: Scene(scene), Cell(cell), Bodies(bodies),
			  Store(physics::testing::BuildScene(layout, bodies, cell)) {
			for (auto &reading : Readings)
				reading.Spans.reserve(128);
			for (size_t tick = 0; tick < 10; tick++)
				physics::testing::Tick(*Store);
		}

		void TickOwner(size_t call) {
			auto &reading = Readings[call];
			reading.HeapBefore = core::HeapProfile::Totals();
			core::FrameGraph::BeginFrame();
			physics::testing::Tick(*Store);
			core::FrameGraph::EndFrame();
			reading.HeapAfter = core::HeapProfile::Totals();
			reading.OwnerMilliseconds = core::FrameGraph::FrameMilliseconds();
			reading.Dropped = core::FrameGraph::Dropped();
			const auto spans = core::FrameGraph::Spans();
			if (spans.size() > 128)
				throw std::runtime_error("counterbalance profile exceeded its bounded capture");
			std::array<bool, REQUIRED_PHASES.size()> found{};
			for (size_t index = 0; index < spans.size(); index++) {
				const auto &span = spans[index];
				if (span.Reported ||
					(span.Depth > 0 && (span.Parent >= index || spans[span.Parent].Depth + 1 != span.Depth)))
					throw std::runtime_error("counterbalance owner hierarchy is incomplete");
				reading.Spans.push_back({std::string(span.Name), span});
				for (size_t phase = 0; phase < REQUIRED_PHASES.size(); phase++) {
					if (span.Name == REQUIRED_PHASES[phase]) {
						found[phase] = true;
						reading.PhaseMilliseconds[phase] += span.Milliseconds;
					}
				}
			}
			if (reading.Dropped != 0 || reading.HeapAfter.DroppedScopes != reading.HeapBefore.DroppedScopes ||
				std::find(found.begin(), found.end(), false) != found.end())
				throw std::runtime_error("counterbalance profile dropped scopes or missed a phase");
		}

		void Diagnose(size_t call, std::vector<std::array<int32_t, 3>> &scratch) {
			auto &reading = Readings[call];
			const auto &world = *Store->Resource<physics::PhysicsWorld>();
			reading.Pairs = world.Pairs().size();
			reading.Manifolds = world.Manifolds().size();
			reading.Events = world.Events().size();
			reading.Tree = physics::PipelineInternals::DynamicTreeActive(world);
			reading.Memory = world.MemoryStats();
			const std::array<const spatial::HashGrid *, 3> grids{
				&physics::PipelineInternals::DynamicIndex(world),
				&physics::PipelineInternals::StaticIndex(world),
				&physics::PipelineInternals::ContinuousIndex(*Store->ResourceMutable<physics::PhysicsWorld>())
			};
			for (size_t index = 0; index < grids.size(); index++) {
				reading.GridMemory[index] = grids[index]->Stats();
				reading.Grids[index] = spatial::testing::ReadGrid(*grids[index], scratch);
			}
		}

		void Print(bool reverse, size_t count) const {
			for (size_t call = 0; call < count; call++) {
				const auto &reading = Readings[call];
				const auto total = reading.Memory.Total();
				std::printf(
					"# cell-pair order=%s scene=%s role=%s cell=%.0f bodies=%zu call=%zu scene_tick=%zu "
					"owner_ms=%.6f sync_ms=%.6f broad_ms=%.6f narrow_ms=%.6f solve_ms=%.6f "
					"pairs=%zu manifolds=%zu events=%zu tree=%d spans=%zu dropped=%zu "
					"heap_hooks=%d allocated_bytes=%llu allocated_blocks=%llu heap_live_bytes=%lld "
					"heap_live_blocks=%lld heap_peak_bytes=%lld heap_overhead_bytes=%lld heap_drops=%llu "
					"physics_live_bytes=%zu physics_retained_bytes=%zu\n",
					reverse ? "2-4" : "4-2",
					Scene.data(),
					Bodies == 0 ? "floor-only" : "scene",
					Cell,
					Bodies,
					call + 1,
					call + 11,
					reading.OwnerMilliseconds,
					reading.PhaseMilliseconds[0],
					reading.PhaseMilliseconds[1],
					reading.PhaseMilliseconds[2],
					reading.PhaseMilliseconds[3],
					reading.Pairs,
					reading.Manifolds,
					reading.Events,
					reading.Tree,
					reading.Spans.size(),
					reading.Dropped,
					core::HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(
						reading.HeapAfter.TotalBytes - reading.HeapBefore.TotalBytes
					),
					static_cast<unsigned long long>(
						reading.HeapAfter.TotalBlocks - reading.HeapBefore.TotalBlocks
					),
					static_cast<long long>(reading.HeapAfter.LiveBytes),
					static_cast<long long>(reading.HeapAfter.LiveBlocks),
					static_cast<long long>(reading.HeapAfter.PeakBytes),
					static_cast<long long>(reading.HeapAfter.OverheadBytes),
					static_cast<unsigned long long>(
						reading.HeapAfter.DroppedScopes - reading.HeapBefore.DroppedScopes
					),
					total.LiveBytes,
					total.RetainedBytes
				);
				const std::array memoryGroups{
					reading.Memory.BroadphaseBuffers,
					reading.Memory.DynamicGrid,
					reading.Memory.StaticGrid,
					reading.Memory.DynamicTree,
					reading.Memory.Solver,
					reading.Memory.Persistent
				};
				constexpr std::array<std::string_view, 6> memoryNames{
					"broadphase_buffers",
					"dynamic_and_continuous_grids",
					"static_grid",
					"dynamic_tree",
					"solver",
					"persistent"
				};
				for (size_t group = 0; group < memoryGroups.size(); group++) {
					std::printf(
						"# cell-memory order=%s scene=%s cell=%.0f bodies=%zu call=%zu group=%s "
						"live_bytes=%zu retained_bytes=%zu\n",
						reverse ? "2-4" : "4-2",
						Scene.data(),
						Cell,
						Bodies,
						call + 1,
						memoryNames[group].data(),
						memoryGroups[group].LiveBytes,
						memoryGroups[group].RetainedBytes
					);
				}
				for (size_t grid = 0; grid < reading.Grids.size(); grid++) {
					const auto &occupancy = reading.Grids[grid];
					std::printf(
						"# cell-grid order=%s scene=%s cell=%.0f bodies=%zu call=%zu grid=%zu proxies=%zu "
						"residual=%zu live_bytes=%zu retained_bytes=%zu\n",
						reverse ? "2-4" : "4-2",
						Scene.data(),
						Cell,
						Bodies,
						call + 1,
						grid,
						occupancy.Proxies,
						occupancy.ResidualProxies,
						reading.GridMemory[grid].LiveBytes,
						reading.GridMemory[grid].RetainedBytes
					);
					for (size_t level = 0; level < occupancy.Levels.size(); level++) {
						const auto &value = occupancy.Levels[level];
						std::printf(
							"# cell-level order=%s scene=%s cell=%.0f bodies=%zu call=%zu grid=%zu level=%zu "
							"proxies=%zu entries=%zu cells=%zu max_memberships=%zu buckets=%zu "
							"occupied_buckets=%zu max_bucket_entries=%zu\n",
							reverse ? "2-4" : "4-2",
							Scene.data(),
							Cell,
							Bodies,
							call + 1,
							grid,
							level,
							value.Proxies,
							value.Entries,
							value.OccupiedCells,
							value.MaximumCellMemberships,
							value.Buckets,
							value.OccupiedBuckets,
							value.MaximumBucketEntries
						);
					}
				}
				for (size_t index = 0; index < reading.Spans.size(); index++) {
					const auto &value = reading.Spans[index];
					std::printf(
						"# cell-span order=%s scene=%s cell=%.0f bodies=%zu call=%zu index=%zu "
						"parent=%u depth=%u start_ms=%.6f inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f "
						"name=%s\n",
						reverse ? "2-4" : "4-2",
						Scene.data(),
						Cell,
						Bodies,
						call + 1,
						index,
						value.Span.Parent,
						value.Span.Depth,
						value.Span.StartMilliseconds,
						value.Span.Milliseconds,
						value.Span.SelfMilliseconds,
						value.Span.IdleMilliseconds,
						value.Name.c_str()
					);
				}
			}
		}
	};

	struct Workload {
		std::array<std::unique_ptr<Arm>, 4> Arms;
	};

	struct PairFixture {
		bool Reverse;
		std::array<Workload, 4> Workloads{};
		std::vector<std::array<int32_t, 3>> Scratch;
		size_t Count = 0;

		explicit PairFixture(bool reverse) : Reverse(reverse) {
			const RestoreSettings restore;
			parallel::SetForceSerialCompute(true);
			core::FrameGraph::SetEnabled(true);
			constexpr std::array<std::pair<std::string_view, physics::testing::Layout>, 4> scenes{{
				{"pile", physics::testing::Layout::Pile},
				{"stacked", physics::testing::Layout::Stacks},
				{"scattered", physics::testing::Layout::Scattered},
				{"mixed-scale", physics::testing::Layout::MixedScale},
			}};
			for (size_t index = 0; index < scenes.size(); index++) {
				const auto [name, layout] = scenes[index];
				Workload &workload = Workloads[index];
				workload.Arms = {
					std::make_unique<Arm>(name, layout, 4.0f, 0),
					std::make_unique<Arm>(name, layout, 4.0f, 4000),
					std::make_unique<Arm>(name, layout, 2.0f, 0),
					std::make_unique<Arm>(name, layout, 2.0f, 4000),
				};
			}
		}
		~PairFixture() {
			for (const Workload &workload : Workloads)
				for (const auto &arm : workload.Arms)
					arm->Print(Reverse, Count);
		}
		void Measure() {
			const RestoreSettings restore;
			parallel::SetForceSerialCompute(true);
			core::FrameGraph::SetEnabled(true);
			if (Count == 13)
				throw std::runtime_error("counterbalance supports five samples after eight warmups");
			const std::array<size_t, 4> order =
				Reverse ? std::array<size_t, 4>{2, 3, 0, 1} : std::array<size_t, 4>{0, 1, 2, 3};
			for (Workload &workload : Workloads) {
				for (const size_t arm : order)
					workload.Arms[arm]->TickOwner(Count);
				// Diagnose only after all owner frames so occupancy scans stay out of timing.
				for (auto &arm : workload.Arms)
					arm->Diagnose(Count, Scratch);
				const auto floor4 = physics::testing::Capture(*workload.Arms[0]->Store);
				const auto scene4 = physics::testing::Capture(*workload.Arms[1]->Store);
				const auto floor2 = physics::testing::Capture(*workload.Arms[2]->Store);
				const auto scene2 = physics::testing::Capture(*workload.Arms[3]->Store);
				if (!physics::testing::SameSnapshot(floor4, floor2) ||
					!physics::testing::SameSnapshot(scene4, scene2))
					throw std::runtime_error("counterbalanced tick outputs differ across cell sizes");
				// Export only the final sample, after all four owner frames.
				if (Count == 12) {
					const size_t orderBase = Reverse ? 4 : 0;
					const size_t sceneBase = static_cast<size_t>(&workload - Workloads.data()) * 8;
					physics::testing::DumpSnapshot("counterbalance", sceneBase + orderBase, 23, floor4);
					physics::testing::DumpSnapshot("counterbalance", sceneBase + orderBase + 1, 23, scene4);
					physics::testing::DumpSnapshot("counterbalance", sceneBase + orderBase + 2, 23, floor2);
					physics::testing::DumpSnapshot("counterbalance", sceneBase + orderBase + 3, 23, scene2);
				}
			}
			Count++;
		}
	};
}

BENCH("Serial paired physics scenes: 4m then 2m", 1) {
	static PairFixture fixture(false);
	fixture.Measure();
}

BENCH("Serial paired physics scenes: 2m then 4m", 1) {
	static PairFixture fixture(true);
	fixture.Measure();
}
