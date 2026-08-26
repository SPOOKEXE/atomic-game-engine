#include <engine/core/FrameGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <studio/Diagnostics.hpp>
#include <vector>

TEST_SUITE_ID("studio.diagnostics")
TEST_DEPENDS("engine.core.framegraph")

using engine::core::FrameGraph;
using engine::core::FrameSpan;
using studio::AccumulateDiagnosticSpans;
using studio::AppendUnaccountedDiagnosticSpans;
using studio::DescribeDiagnosticSpan;
using studio::DiagnosticSpan;
using studio::FinishDiagnosticAverage;
using studio::FitReportedDiagnosticTimeline;
using studio::LayoutDiagnosticRows;

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
