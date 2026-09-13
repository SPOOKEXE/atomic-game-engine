#include "PhysicsProfiler.hpp"
#include "ProfilerFlame.hpp"
#include "TimelineBar.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <studio/Diagnostics.hpp>
#include <vector>

TEST_SUITE_ID("studio.diagnostics")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.examples.scene")
TEST_DEPENDS("engine.physics.pipeline")

using engine::core::FrameGraph;
using engine::core::FrameSpan;
using studio::AccumulateDiagnosticSpans;
using studio::AppendUnaccountedDiagnosticSpans;
using studio::DescribeDiagnosticSpan;
using studio::DiagnosticAggregation;
using studio::DiagnosticSpan;
using studio::FilterDiagnosticSpans;
using studio::FinishDiagnosticAverage;
using studio::FitReportedDiagnosticTimeline;
using studio::FocusDiagnosticSpans;
using studio::LayoutDiagnosticRows;
using studio::SelectHeapHistorySnapshot;
using studio::ShouldReplaceDiagnosticSnapshot;

TEST_CASE("timeline bars fit at the right edge and in narrow panels", "[studio][diagnostics]") {
	for (const float width : {0.0f, 0.5f, 1.0f, 100.0f}) {
		for (const float start : {-1.0f, 0.0f, width - 0.25f, width, width + 1.0f}) {
			for (const float duration : {0.0f, 0.001f, 1.0f, 200.0f}) {
				const auto bar = studio::FitTimelineBar(start, duration, width);
				CAPTURE(width, start, duration);
				CHECK(bar.Left >= 0.0f);
				CHECK(bar.Right <= width);
				CHECK(bar.Right - bar.Left >= std::min(width, 1.0f));
			}
		}
	}
	const auto bar = studio::FitTimelineBar(25.0f, 30.0f, 100.0f);
	CHECK(bar.Left == 25.0f);
	CHECK(bar.Right == 55.0f);
}

TEST_CASE("profiler flame filtering reparents around reported workers", "[studio][diagnostics]") {
	const std::array spans{
		DiagnosticSpan{.Name = "root"},
		DiagnosticSpan{.Name = "worker", .Depth = 1, .Parent = 0, .Reported = true},
		DiagnosticSpan{.Name = "measured child", .Depth = 2, .Parent = 1},
	};
	std::vector<DiagnosticSpan> measured;
	studio::MeasuredProfilerSpans(spans, measured);
	REQUIRE(measured.size() == 2);
	CHECK(measured[0].Parent == FrameGraph::NO_PARENT);
	CHECK(measured[1].Parent == 0);
	CHECK(measured[1].Depth == 1);
	std::vector<uint32_t> rows;
	CHECK(LayoutDiagnosticRows(measured, rows) == 2);
	CHECK(rows[1] == 1);
}

TEST_CASE("physics profiler retains reported work and its scheduler ancestors", "[studio][diagnostics]") {
	using engine::core::ProfileCategory;
	const std::array spans{
		FrameSpan{.Name = "world", .Parent = FrameGraph::NO_PARENT, .Category = ProfileCategory::ECS},
		FrameSpan{.Name = "phase", .Depth = 1, .Parent = 0, .Category = ProfileCategory::ECS},
		FrameSpan{
			.Name = "physics.worker",
			.Depth = 2,
			.Parent = 1,
			.Category = ProfileCategory::ECS,
			.Reported = true
		},
		FrameSpan{.Name = "unrelated ecs", .Parent = FrameGraph::NO_PARENT, .Category = ProfileCategory::ECS},
	};
	std::vector<bool> selected;
	studio::PhysicsProfilerSpanMask(spans, selected);
	CHECK(selected == std::vector<bool>{true, true, true, false});
}

TEST_CASE("physics profiler retains the last Slide reading between physics ticks", "[studio][diagnostics]") {
	struct RestoreAssets {
		std::filesystem::path Previous;
		~RestoreAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};
	const RestoreAssets restoreAssets{engine::core::Paths::Assets()};
	engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

	engine::ecs::Store store("studio.physics-profiler.slide");
	engine::ecs::Scheduler systems;
	std::string error;
	REQUIRE(engine::examples::LoadScene(store, systems, engine::examples::ExamplePath("Slide.luau"), error));
	engine::physics::PreparePhysicsWorld(store);
	engine::physics::RegisterPhysicsSystems(systems);
	engine::scene::PrepareGravity(store);
	engine::scene::RegisterGravitySystem(systems);

	FrameGraph::SetEnabled(true);
	struct DisableFrameGraph {
		~DisableFrameGraph() {
			FrameGraph::SetEnabled(false);
		}
	};
	const DisableFrameGraph disableFrameGraph;
	FrameGraph::BeginFrame();
	systems.Tick(store, 1.0f / 60.0f);
	FrameGraph::EndFrame();

	std::vector<DiagnosticSpan> captured;
	REQUIRE(studio::CapturePhysicsProfilerSpans(FrameGraph::Spans(), captured));
	REQUIRE_FALSE(captured.empty());
	CHECK(std::any_of(captured.begin(), captured.end(), [](const DiagnosticSpan &span) {
		return span.Name == "physics.integrate" && span.Milliseconds > 0.0f;
	}));

	const std::vector<DiagnosticSpan> prior = captured;
	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("studio render-only fixture"); }
	FrameGraph::EndFrame();
	CHECK_FALSE(studio::CapturePhysicsProfilerSpans(FrameGraph::Spans(), captured));
	CHECK(captured.size() == prior.size());
	CHECK(captured.front().Name == prior.front().Name);
	CHECK(captured.front().Milliseconds == prior.front().Milliseconds);
}

TEST_CASE("studio profiling macros submit studio ownership", "[studio][diagnostics]") {
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("studio binding"); }
	FrameGraph::EndFrame();

	const bool owned =
		FrameGraph::Spans().size() == 1 && FrameGraph::Spans()[0].Owner == engine::core::ProfileOwner::Studio;
	FrameGraph::SetEnabled(false);
	CHECK(owned);
}

TEST_CASE("frame extrema retain one complete hierarchy", "[studio][diagnostics]") {
	const std::array frames{
		std::vector<DiagnosticSpan>{
			DiagnosticSpan{.Name = "frame", .Milliseconds = 8.0f},
			DiagnosticSpan{.Name = "simulation", .Depth = 1, .Parent = 0, .Milliseconds = 7.0f},
		},
		std::vector<DiagnosticSpan>{
			DiagnosticSpan{.Name = "frame", .Milliseconds = 20.0f},
			DiagnosticSpan{.Name = "render", .Depth = 1, .Parent = 0, .Milliseconds = 19.0f},
		},
	};
	const std::array milliseconds{8.0f, 20.0f};

	size_t selected = 0;
	for (size_t index = 1; index < frames.size(); index++) {
		if (ShouldReplaceDiagnosticSnapshot(
				DiagnosticAggregation::Maximum, true, milliseconds[selected], milliseconds[index]
			)) {
			selected = index;
		}
	}
	CHECK(frames[selected][1].Name == "render");
	CHECK(frames[selected][1].Milliseconds == 19.0f);

	selected = 0;
	for (size_t index = 1; index < frames.size(); index++) {
		if (ShouldReplaceDiagnosticSnapshot(
				DiagnosticAggregation::Minimum, true, milliseconds[selected], milliseconds[index]
			)) {
			selected = index;
		}
	}
	CHECK(frames[selected][1].Name == "simulation");
	CHECK(frames[selected][1].Milliseconds == 7.0f);
}

TEST_CASE("heap window aggregation treats absent tags as zero", "[studio][diagnostics]") {
	using engine::core::HeapHistorySnapshot;
	using engine::core::HeapSample;
	const std::array snapshots{
		HeapHistorySnapshot{
			.Sample = HeapSample{.Seconds = 1.0, .LiveBytes = 100, .LiveBlocks = 10},
			.InclusiveBytes = {100, 60, 30}
		},
		HeapHistorySnapshot{
			.Sample = HeapSample{.Seconds = 2.0, .LiveBytes = 200, .LiveBlocks = 20},
			.InclusiveBytes = {200, 50, 0, 90}
		},
	};

	HeapHistorySnapshot selected;
	REQUIRE(SelectHeapHistorySnapshot(snapshots, DiagnosticAggregation::Latest, selected));
	CHECK(selected.Sample.Seconds == 2.0);
	CHECK(selected.InclusiveBytes == std::vector<int64_t>{200, 50, 0, 90});

	REQUIRE(SelectHeapHistorySnapshot(snapshots, DiagnosticAggregation::Average, selected));
	CHECK(selected.Sample.Seconds == 2.0);
	CHECK(selected.Sample.LiveBytes == 150);
	CHECK(selected.Sample.LiveBlocks == 15);
	CHECK(selected.InclusiveBytes == std::vector<int64_t>{150, 55, 15, 45});

	REQUIRE(SelectHeapHistorySnapshot(snapshots, DiagnosticAggregation::Maximum, selected));
	CHECK(selected.Sample.Seconds == 2.0);
	CHECK(selected.InclusiveBytes == std::vector<int64_t>{200, 50, 0, 90});

	REQUIRE(SelectHeapHistorySnapshot(snapshots, DiagnosticAggregation::Minimum, selected));
	CHECK(selected.Sample.Seconds == 1.0);
	CHECK(selected.InclusiveBytes == std::vector<int64_t>{100, 60, 30});
}

TEST_CASE("empty heap window leaves its prior selection intact", "[studio][diagnostics]") {
	engine::core::HeapHistorySnapshot selected{
		.Sample = engine::core::HeapSample{.Seconds = 3.0, .LiveBytes = 42, .LiveBlocks = 1},
		.InclusiveBytes = {42},
	};
	CHECK_FALSE(SelectHeapHistorySnapshot({}, DiagnosticAggregation::Latest, selected));
	CHECK(selected.Sample.LiveBytes == 42);
	CHECK(selected.InclusiveBytes == std::vector<int64_t>{42});
}

TEST_CASE("owner filters retain valid product trees", "[studio][diagnostics]") {
	using engine::core::ProfileOwner;

	const std::array spans{
		DiagnosticSpan{.Name = "Application", .Owner = ProfileOwner::Studio},
		DiagnosticSpan{.Name = "presentation", .Depth = 1, .Parent = 0, .Owner = ProfileOwner::Studio},
		DiagnosticSpan{.Name = "client runtime", .Depth = 2, .Parent = 1, .Owner = ProfileOwner::Client},
		DiagnosticSpan{.Name = "Universe::Tick", .Depth = 3, .Parent = 2, .Owner = ProfileOwner::Engine},
		DiagnosticSpan{.Name = "interface", .Depth = 2, .Parent = 1, .Owner = ProfileOwner::Studio},
	};

	std::vector<DiagnosticSpan> filtered;
	FilterDiagnosticSpans(spans, ProfileOwner::Studio, filtered);
	REQUIRE(filtered.size() == 3);
	CHECK(filtered[0].Name == "Application");
	CHECK(filtered[1].Parent == 0);
	CHECK(filtered[2].Parent == 1);
	CHECK(filtered[2].Depth == 2);

	FilterDiagnosticSpans(spans, ProfileOwner::Engine, filtered);
	REQUIRE(filtered.size() == 1);
	CHECK(filtered[0].Name == "Universe::Tick");
	CHECK(filtered[0].Parent == FrameGraph::NO_PARENT);
	CHECK(filtered[0].Depth == 0);

	FilterDiagnosticSpans(spans, ProfileOwner::All, filtered);
	REQUIRE(filtered.size() == spans.size());
	CHECK(filtered[3].Parent == 2);
}

TEST_CASE("flame graph focus retains one selected subtree", "[studio][diagnostics]") {
	const std::array spans{
		DiagnosticSpan{.Name = "frame"},
		DiagnosticSpan{.Name = "renderer", .Depth = 1, .Parent = 0},
		DiagnosticSpan{.Name = "cull", .Depth = 2, .Parent = 1},
		DiagnosticSpan{.Name = "present", .Depth = 1, .Parent = 0},
	};

	std::vector<DiagnosticSpan> focused;
	std::vector<uint32_t> sourceIndices;
	FocusDiagnosticSpans(spans, 1, focused, sourceIndices);

	REQUIRE(focused.size() == 2);
	CHECK(sourceIndices == std::vector<uint32_t>{1, 2});
	CHECK(focused[0].Name == "renderer");
	CHECK(focused[0].Parent == FrameGraph::NO_PARENT);
	CHECK(focused[0].Depth == 0);
	CHECK(focused[1].Name == "cull");
	CHECK(focused[1].Parent == 0);
	CHECK(focused[1].Depth == 1);

	FocusDiagnosticSpans(spans, 99, focused, sourceIndices);
	CHECK(focused.empty());
	CHECK(sourceIndices.empty());
}

TEST_CASE("every diagnostic span has an explanatory tooltip", "[studio][diagnostics]") {
	CHECK(
		DescribeDiagnosticSpan("content.demand", engine::core::ProfileCategory::Assets, false)
			.find("references changed") != std::string_view::npos
	);
	CHECK_FALSE(DescribeDiagnosticSpan("runtime.system", engine::core::ProfileCategory::ECS, false).empty());
	CHECK(
		DescribeDiagnosticSpan("worker", engine::core::ProfileCategory::ECS, true).find("another worker") !=
		std::string_view::npos
	);
	CHECK(
		DescribeDiagnosticSpan("unaccounted", engine::core::ProfileCategory::Engine, false)
			.find("not covered") != std::string_view::npos
	);
}

TEST_CASE("averaging retains repeated scheduler trees", "[studio][diagnostics]") {
	const std::array frame{
		FrameSpan{.Name = "world", .Depth = 0, .Parent = FrameGraph::NO_PARENT},
		FrameSpan{.Name = "ecs.systems", .Depth = 1, .Parent = 0, .Milliseconds = 2.0f},
		FrameSpan{.Name = "post-simulation", .Depth = 2, .Parent = 1, .Milliseconds = 0.5f},
		FrameSpan{.Name = "world", .Depth = 0, .Parent = FrameGraph::NO_PARENT},
		FrameSpan{.Name = "ecs.systems", .Depth = 1, .Parent = 3, .Milliseconds = 3.0f},
		FrameSpan{.Name = "post-simulation", .Depth = 2, .Parent = 4, .Milliseconds = 0.75f},
	};

	std::vector<DiagnosticSpan> totals;
	AccumulateDiagnosticSpans(frame, totals);
	FinishDiagnosticAverage(totals, 1);

	REQUIRE(totals.size() == frame.size());
	CHECK(totals[1].Name == "ecs.systems");
	CHECK(totals[4].Name == "ecs.systems");
	CHECK(totals[1].Parent == 0);
	CHECK(totals[4].Parent == 3);
	CHECK(totals[2].Name == "post-simulation");
	CHECK(totals[5].Name == "post-simulation");
	CHECK(totals[2].Milliseconds == 0.5f);
	CHECK(totals[5].Milliseconds == 0.75f);
}

TEST_CASE("same-name siblings keep their ordinal across frames", "[studio][diagnostics]") {
	const std::array frame{
		FrameSpan{.Name = "root", .Depth = 0, .Parent = FrameGraph::NO_PARENT},
		FrameSpan{.Name = "tick", .Depth = 1, .Parent = 0, .Milliseconds = 1.0f},
		FrameSpan{.Name = "tick", .Depth = 1, .Parent = 0, .Milliseconds = 2.0f},
	};

	std::vector<DiagnosticSpan> totals;
	AccumulateDiagnosticSpans(frame, totals);
	AccumulateDiagnosticSpans(frame, totals);
	FinishDiagnosticAverage(totals, 2);

	REQUIRE(totals.size() == 3);
	CHECK(totals[1].Milliseconds == 1.0f);
	CHECK(totals[2].Milliseconds == 2.0f);
	CHECK(totals[1].Occurrences == 2);
	CHECK(totals[2].Occurrences == 2);
}

TEST_CASE("reported parallel work fits inside measured wall time", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "Universe::Tick", .Depth = 0, .Milliseconds = 10.0f},
		DiagnosticSpan{
			.Name = "barrier",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 0.0f,
			.Milliseconds = 1.0f,
		},
		DiagnosticSpan{
			.Name = "workers",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 9.0f,
			.Milliseconds = 20.0f,
			.Reported = true,
		},
		DiagnosticSpan{
			.Name = "left",
			.Depth = 2,
			.Parent = 2,
			.Milliseconds = 4.0f,
			.Reported = true,
		},
		DiagnosticSpan{
			.Name = "right",
			.Depth = 2,
			.Parent = 2,
			.Milliseconds = 6.0f,
			.Reported = true,
		},
		DiagnosticSpan{
			.Name = "diagnostics",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 9.0f,
			.Milliseconds = 1.0f,
		},
	};

	FitReportedDiagnosticTimeline(spans, 10.0f);

	CHECK(spans[2].StartMilliseconds == 1.0f);
	CHECK(spans[2].Milliseconds == 8.0f);
	CHECK(spans[3].StartMilliseconds == 1.0f);
	CHECK(spans[3].Milliseconds == Catch::Approx(3.2f));
	CHECK(spans[4].StartMilliseconds == Catch::Approx(4.2f));
	CHECK(spans[4].Milliseconds == Catch::Approx(4.8f));

	std::vector<uint32_t> rows;
	CHECK(LayoutDiagnosticRows(spans, rows) == 3);
}

TEST_CASE("reported trees cannot overlap the next measured sibling", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "Application", .Depth = 0, .Milliseconds = 10.0f},
		DiagnosticSpan{.Name = "simulation", .Depth = 1, .Parent = 0, .Milliseconds = 6.0f},
		DiagnosticSpan{
			.Name = "workers",
			.Depth = 2,
			.Parent = 1,
			.StartMilliseconds = 5.0f,
			.Milliseconds = 12.0f,
			.Reported = true,
		},
		DiagnosticSpan{
			.Name = "late measured child",
			.Depth = 3,
			.Parent = 2,
			.StartMilliseconds = 7.0f,
			.Milliseconds = 2.0f,
		},
		DiagnosticSpan{
			.Name = "presentation",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 6.0f,
			.Milliseconds = 4.0f,
		},
	};

	FitReportedDiagnosticTimeline(spans, 10.0f);

	const float simulationEnd = spans[1].StartMilliseconds + spans[1].Milliseconds;
	CHECK(spans[2].StartMilliseconds + spans[2].Milliseconds <= simulationEnd);
	CHECK(spans[3].StartMilliseconds + spans[3].Milliseconds <= simulationEnd);
	CHECK(spans[4].StartMilliseconds >= simulationEnd);
}

TEST_CASE("timing overlap never changes hierarchy rows", "[studio][diagnostics]") {
	const std::array spans{
		DiagnosticSpan{.Name = "first", .Depth = 0, .StartMilliseconds = 0.0f, .Milliseconds = 5.0f},
		DiagnosticSpan{.Name = "overlap", .Depth = 0, .StartMilliseconds = 2.0f, .Milliseconds = 2.0f},
		DiagnosticSpan{
			.Name = "child", .Depth = 1, .Parent = 0, .StartMilliseconds = 1.0f, .Milliseconds = 1.0f
		},
		DiagnosticSpan{.Name = "grandchild", .Depth = 2, .Parent = 2, .Milliseconds = 1.0f},
	};

	std::vector<uint32_t> rows;
	const uint32_t count = LayoutDiagnosticRows(spans, rows);

	REQUIRE(rows.size() == spans.size());
	CHECK(rows[0] == rows[1]);
	CHECK(rows[2] == rows[0] + 1);
	CHECK(rows[3] == rows[2] + 1);
	CHECK(count == 3);
}

TEST_CASE("parallel summaries keep every branch on the same depth grid", "[studio][diagnostics]") {
	const std::array spans{
		DiagnosticSpan{
			.Name = "application", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .Milliseconds = 5.0f
		},
		DiagnosticSpan{.Name = "simulation", .Depth = 1, .Parent = 0, .Milliseconds = 5.0f},
		DiagnosticSpan{.Name = "workers", .Depth = 2, .Parent = 1, .Milliseconds = 8.0f, .Reported = true},
		DiagnosticSpan{.Name = "measured wait", .Depth = 2, .Parent = 1, .Milliseconds = 5.0f},
		DiagnosticSpan{.Name = "left world", .Depth = 3, .Parent = 2, .Milliseconds = 4.0f, .Reported = true},
		DiagnosticSpan{
			.Name = "right world", .Depth = 3, .Parent = 2, .Milliseconds = 4.0f, .Reported = true
		},
		DiagnosticSpan{.Name = "system", .Depth = 4, .Parent = 4, .Milliseconds = 1.0f, .Reported = true},
	};

	std::vector<uint32_t> rows;
	const uint32_t count = LayoutDiagnosticRows(spans, rows);

	REQUIRE(rows.size() == spans.size());
	CHECK(rows == std::vector<uint32_t>{0, 1, 2, 2, 3, 3, 4});
	CHECK(count == 5);
}

TEST_CASE("unaccounted spans fill gaps between direct children", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{
			.Name = "Application",
			.Depth = 0,
			.StartMilliseconds = 0.0f,
			.Milliseconds = 10.0f,
			.SelfMilliseconds = 6.0f,
		},
		DiagnosticSpan{
			.Name = "first",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 1.0f,
			.Milliseconds = 2.0f,
		},
		DiagnosticSpan{
			.Name = "second",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 5.0f,
			.Milliseconds = 2.0f,
		},
	};

	AppendUnaccountedDiagnosticSpans(spans);

	REQUIRE(spans.size() == 6);
	CHECK(spans[3].Name == "unaccounted");
	CHECK(spans[3].StartMilliseconds == 0.0f);
	CHECK(spans[3].Milliseconds == 1.0f);
	CHECK(spans[4].StartMilliseconds == 3.0f);
	CHECK(spans[4].Milliseconds == 2.0f);
	CHECK(spans[5].StartMilliseconds == 7.0f);
	CHECK(spans[5].Milliseconds == 3.0f);
	CHECK(spans[3].Parent == 0);
	CHECK(spans[3].Depth == 1);
}

TEST_CASE("unaccounted spans merge overlapping child coverage", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "Application", .Depth = 0, .StartMilliseconds = 2.0f, .Milliseconds = 8.0f},
		DiagnosticSpan{
			.Name = "first",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 1.0f,
			.Milliseconds = 6.0f,
		},
		DiagnosticSpan{
			.Name = "overlap",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 5.0f,
			.Milliseconds = 7.0f,
		},
	};

	AppendUnaccountedDiagnosticSpans(spans);

	CHECK(spans.size() == 3);
}

TEST_CASE("reported worker time does not conceal measured parent gaps", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "Universe::Tick", .Depth = 0, .Milliseconds = 2.0f},
		DiagnosticSpan{
			.Name = "barrier",
			.Depth = 1,
			.Parent = 0,
			.Milliseconds = 0.5f,
		},
		DiagnosticSpan{
			.Name = "worlds (pinned workers)",
			.Depth = 1,
			.Parent = 0,
			.Milliseconds = 8.0f,
			.Reported = true,
		},
	};
	spans[0].SelfMilliseconds = 1.5f;

	AppendUnaccountedDiagnosticSpans(spans);

	REQUIRE(spans.size() == 4);
	CHECK(spans[3].Name == "unaccounted");
	CHECK(spans[3].Parent == 0);
	CHECK(spans[3].StartMilliseconds == 0.5f);
	CHECK(spans[3].Milliseconds == 1.5f);
}

TEST_CASE("averaged sparse gaps are capped by measured self time", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{
			.Name = "ecs.systems",
			.Depth = 0,
			.Milliseconds = 0.30f,
			.SelfMilliseconds = 0.012f,
		},
		DiagnosticSpan{
			.Name = "pre-simulation",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 0.01f,
			.Milliseconds = 0.04f,
		},
		DiagnosticSpan{
			.Name = "simulation",
			.Depth = 1,
			.Parent = 0,
			.StartMilliseconds = 0.20f,
			.Milliseconds = 0.08f,
		},
	};

	AppendUnaccountedDiagnosticSpans(spans);

	float unaccounted = 0.0f;
	for (const DiagnosticSpan &span : spans) {
		if (span.Name == "unaccounted" && span.Parent == 0) {
			unaccounted += span.Milliseconds;
		}
	}
	CHECK(unaccounted == Catch::Approx(0.012f));
}

TEST_CASE("reported summaries do not invent unaccounted worker time", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "workers", .Depth = 0, .Milliseconds = 8.0f, .Reported = true},
		DiagnosticSpan{
			.Name = "world",
			.Depth = 1,
			.Parent = 0,
			.Milliseconds = 8.0f,
			.Reported = true,
		},
	};

	AppendUnaccountedDiagnosticSpans(spans);

	CHECK(spans.size() == 2);
}
