#include <engine/core/FrameGraph.hpp>
#include <engine/testing/Suite.hpp>

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
using studio::DiagnosticSpan;
using studio::FinishDiagnosticAverage;
using studio::LayoutDiagnosticRows;

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

TEST_CASE("overlapping bars at one depth receive separate rows", "[studio][diagnostics]") {
	const std::array spans{
		DiagnosticSpan{.Name = "first", .Depth = 0, .StartMilliseconds = 0.0f, .Milliseconds = 5.0f},
		DiagnosticSpan{.Name = "overlap", .Depth = 0, .StartMilliseconds = 2.0f, .Milliseconds = 2.0f},
		DiagnosticSpan{.Name = "after", .Depth = 0, .StartMilliseconds = 5.0f, .Milliseconds = 2.0f},
		DiagnosticSpan{.Name = "child", .Depth = 1, .StartMilliseconds = 1.0f, .Milliseconds = 1.0f},
	};

	std::vector<uint32_t> rows;
	const uint32_t count = LayoutDiagnosticRows(spans, 10.0f, 0.01f, rows);

	REQUIRE(rows.size() == spans.size());
	CHECK(rows[0] != rows[1]);
	CHECK(rows[0] == rows[2]);
	CHECK(rows[3] > rows[1]);
	CHECK(count == 3);
}

TEST_CASE("unaccounted spans fill gaps between direct children", "[studio][diagnostics]") {
	std::vector spans{
		DiagnosticSpan{.Name = "Application", .Depth = 0, .StartMilliseconds = 0.0f, .Milliseconds = 10.0f},
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
