#include "PanelShares.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.panelshares")
TEST_DEPENDS("engine.core.framegraph")

using Catch::Approx;
using engine::core::FrameGraph;
using engine::core::FrameSpan;
using engine::core::ProfileCategory;
using engine::render::BusyMillisecondsOf;
using engine::render::BusyShares;
using engine::render::CATEGORY_BAR_COUNT;
using engine::render::CategoryShares;
using engine::render::DebugPanelData;
using engine::render::UNMARKED_BAR;

namespace {
	FrameSpan Span(ProfileCategory category, float selfMilliseconds) {
		FrameSpan span;
		span.Name = "span";
		span.Parent = FrameGraph::NO_PARENT;
		span.Milliseconds = selfMilliseconds;
		span.SelfMilliseconds = selfMilliseconds;
		span.Category = category;
		return span;
	}

	constexpr size_t Slot(ProfileCategory category) {
		return static_cast<size_t>(category);
	}

	// The panel state as the client fills it: a frame, how much of it was a
	// wait, what no span covered, and the spans themselves.
	DebugPanelData Panel(
		std::span<const FrameSpan> spans,
		float frameMilliseconds,
		float idleMilliseconds,
		float unmarkedMilliseconds = 0.0f
	) {
		DebugPanelData data;
		data.Spans = spans;
		data.FrameMilliseconds = frameMilliseconds;
		data.IdleMilliseconds = idleMilliseconds;
		data.UnmarkedMilliseconds = unmarkedMilliseconds;
		return data;
	}
}

TEST_CASE("a bar is a share of the busy frame and not of the whole one", "[panelshares]") {
	// The vsynced frame this tab exists to survive: 16.7 ms of wall clock, 15
	// of it a sleep, and 1.7 ms of work spread over two categories.
	//
	// **This is the case the tab was getting wrong.** Divided by the frame,
	// render's bar was 0.9 ms in 16.7 and drew at five per cent of its track —
	// so with vertical sync on, which is the default, every bar on the panel
	// was a stub and nothing on it could be compared with anything else.
	const std::vector<FrameSpan> spans{
		Span(ProfileCategory::Idle, 15.0f),
		Span(ProfileCategory::Render, 0.9f),
		Span(ProfileCategory::Physics, 0.8f),
	};

	const auto shares = CategoryShares(Panel(spans, 16.7f, 15.0f));

	REQUIRE(shares[Slot(ProfileCategory::Render)] == Approx(0.9f / 1.7f).margin(0.001));
	REQUIRE(shares[Slot(ProfileCategory::Physics)] == Approx(0.8f / 1.7f).margin(0.001));

	// Against the whole frame these would be 0.054 and 0.048. The margin is
	// far wider than the difference between the two readings, so this fails
	// loudly rather than by a rounding.
	REQUIRE(shares[Slot(ProfileCategory::Render)] > 0.4f);
	REQUIRE(shares[Slot(ProfileCategory::Physics)] > 0.4f);
}

TEST_CASE("the bars partition the busy frame", "[panelshares]") {
	// The claim the tab makes by drawing them on one axis: every bar plus the
	// unmarked remainder is the busy frame, exactly. It holds because category
	// totals are self time, idle is a category, and unmarked time is what no
	// span covered.
	const std::vector<FrameSpan> spans{
		Span(ProfileCategory::Idle, 10.0f),
		Span(ProfileCategory::Render, 2.0f),
		Span(ProfileCategory::ECS, 1.0f),
		Span(ProfileCategory::Network, 0.5f),
	};

	// Frame 16.0, idle 10.0, so busy is 6.0 — of which 3.5 is in spans and the
	// remaining 2.5 was never inside one.
	const auto shares = CategoryShares(Panel(spans, 16.0f, 10.0f, 2.5f));

	float total = 0.0f;
	for (float share : shares) {
		total += share;
	}
	REQUIRE(total == Approx(1.0f).margin(0.001));
}

TEST_CASE("idle gets no bar of its own", "[panelshares]") {
	const std::vector<FrameSpan> spans{Span(ProfileCategory::Idle, 15.0f)};

	// Idle is not in the denominator, so a share of it is not a quantity. Zero
	// rather than 15/1.7, which would be a bar nine times the panel width and
	// would have to be clamped to a full track — reading as "the frame was
	// entirely idle work", which is a contradiction.
	const auto shares = CategoryShares(Panel(spans, 16.7f, 15.0f));
	REQUIRE(shares[Slot(ProfileCategory::Idle)] == Approx(0.0f));
}

TEST_CASE("a share never exceeds a full track", "[panelshares]") {
	// A `Reported` span carries time measured on another thread and is not
	// subtracted from its parent's self time, so eight workers can honestly
	// contribute more than the frame has wall clock. `OverlayImage::Blend`
	// clips to the image and not to the panel, so an unclamped share draws a
	// bar across the game being profiled.
	const std::vector<FrameSpan> spans{
		Span(ProfileCategory::ECS, 40.0f),
		Span(ProfileCategory::ECS, 40.0f),
	};

	const auto shares = CategoryShares(Panel(spans, 8.0f, 0.0f));
	REQUIRE(shares[Slot(ProfileCategory::ECS)] == Approx(1.0f));
}

TEST_CASE("a zero-length frame does not divide by it", "[panelshares]") {
	const std::vector<FrameSpan> spans{Span(ProfileCategory::Engine, 0.0f)};

	const auto shares = CategoryShares(Panel(spans, 0.0f, 0.0f));
	for (float share : shares) {
		REQUIRE(share >= 0.0f);
		REQUIRE(share <= 1.0f);
	}
}

TEST_CASE("unmarked time rides in the slot after the last category", "[panelshares]") {
	const auto shares = CategoryShares(Panel({}, 4.0f, 0.0f, 1.0f));

	REQUIRE(UNMARKED_BAR == static_cast<size_t>(ProfileCategory::Count));
	REQUIRE(CATEGORY_BAR_COUNT == UNMARKED_BAR + 1);
	REQUIRE(shares[UNMARKED_BAR] == Approx(0.25f));
}

TEST_CASE("a category outside the enum is dropped rather than written past", "[panelshares]") {
	// Not reachable through the profiling macros, which take a `ProfileCategory`.
	// Reachable by anyone filling a `FrameSpan` by hand — which the panel suite
	// does — and the panels are the thing that says whether a frame can be
	// trusted, so they do not get to be the thing that corrupts it.
	std::vector<FrameSpan> spans{Span(ProfileCategory::Engine, 1.0f)};
	spans[0].Category = static_cast<ProfileCategory>(200);

	const auto shares = CategoryShares(Panel(spans, 2.0f, 0.0f));
	for (float share : shares) {
		REQUIRE(share == Approx(0.0f));
	}
}

// --- the SHARE column -------------------------------------------------------
//
// The bug this guards against was reported off the screen, not off a test: the
// column read **2634%**. Nothing was corrupt — a span's `Milliseconds` is
// inclusive, the denominator was the frame less its idle time, and the render
// span encloses the swapchain wait. The arithmetic divided two real numbers
// that were never the same measurement.

namespace {
	// The frame the bug was found on, to the numbers the snapshot recorded.
	//
	// frame 16.737 ms, of which `acquire swapchain` waited 16.115 ms, so the
	// busy part is 0.622 ms and `Renderer::Render` covers 16.385 ms of it.
	std::vector<FrameSpan> VsyncedFrame() {
		std::vector<FrameSpan> spans(3);

		spans[0].Name = "Renderer::Render";
		spans[0].Depth = 0;
		spans[0].Parent = 0;
		spans[0].Milliseconds = 16.385f;
		spans[0].SelfMilliseconds = 0.270f;
		spans[0].Category = ProfileCategory::Render;

		spans[1].Name = "acquire swapchain";
		spans[1].Depth = 1;
		spans[1].Parent = 0;
		spans[1].Milliseconds = 16.115f;
		spans[1].SelfMilliseconds = 16.115f;
		spans[1].Category = ProfileCategory::Idle;

		spans[2].Name = "simulation";
		spans[2].Depth = 0;
		spans[2].Parent = 2;
		spans[2].Milliseconds = 0.231f;
		spans[2].SelfMilliseconds = 0.231f;
		spans[2].Category = ProfileCategory::Simulation;

		return spans;
	}
}

TEST_CASE("a span enclosing the vsync wait does not exceed 100%", "[panelshares]") {
	std::vector<FrameSpan> spans = VsyncedFrame();
	engine::core::AccumulateIdleMilliseconds(spans);
	const std::vector<float> shares = BusyShares(Panel(spans, 16.737f, 16.115f));

	REQUIRE(shares.size() == spans.size());

	// 16.385 / 0.622 was 2634%. With the wait taken out of the numerator it is
	// 0.270 / 0.622, which is what the render actually cost.
	CHECK(shares[0] == Approx(0.270f / 0.622f).margin(0.01));
	CHECK(shares[0] < 1.0f);

	// A scope whose entire job is to block accounts for none of the busy time.
	// Zero is the honest answer rather than a suppressed one.
	CHECK(shares[1] == Approx(0.0f).margin(0.001));

	CHECK(shares[2] == Approx(0.231f / 0.622f).margin(0.01));
}

TEST_CASE("no span's share exceeds the whole busy frame", "[panelshares]") {
	std::vector<FrameSpan> spans = VsyncedFrame();
	engine::core::AccumulateIdleMilliseconds(spans);
	const std::vector<float> shares = BusyShares(Panel(spans, 16.737f, 16.115f));

	for (const float share : shares) {
		CHECK(share >= 0.0f);
		CHECK(share <= 1.0f);
	}
}

TEST_CASE("nested waits are not counted twice", "[panelshares]") {
	std::vector<FrameSpan> spans(3);

	spans[0].Name = "outer";
	spans[0].Depth = 0;
	spans[0].Parent = 0;
	spans[0].Milliseconds = 10.0f;
	spans[0].SelfMilliseconds = 2.0f;
	spans[0].Category = ProfileCategory::Render;

	// A wait containing another wait. Subtracting inclusive time at both levels
	// would remove the inner one twice and drive the outer share negative.
	spans[1].Depth = 1;
	spans[1].Parent = 0;
	spans[1].Milliseconds = 8.0f;
	spans[1].SelfMilliseconds = 3.0f;
	spans[1].Category = ProfileCategory::Idle;

	spans[2].Depth = 2;
	spans[2].Parent = 1;
	spans[2].Milliseconds = 5.0f;
	spans[2].SelfMilliseconds = 5.0f;
	spans[2].Category = ProfileCategory::Idle;

	engine::core::AccumulateIdleMilliseconds(spans);
	const std::vector<float> shares = BusyShares(Panel(spans, 10.0f, 8.0f));

	// 10 inclusive less 8 of waiting is 2, which is the whole busy frame.
	CHECK(shares[0] == Approx(1.0f).margin(0.001));
	CHECK(shares[1] == Approx(0.0f).margin(0.001));
	CHECK(shares[2] == Approx(0.0f).margin(0.001));
}

TEST_CASE("busy and idle split an inclusive duration in two", "[panelshares]") {
	// **The pair the BUSY and IDLE columns print.** The share arithmetic above
	// checks a ratio, which stays right even if both halves are wrong together.
	// These are the two numbers a reader acts on, so they are checked as
	// numbers: what the renderer did, and what it waited for.
	std::vector<FrameSpan> spans = VsyncedFrame();
	engine::core::AccumulateIdleMilliseconds(spans);

	// The enclosing render span: 16.385 inclusive, 16.115 of it the wait.
	CHECK(spans[0].IdleMilliseconds == Approx(16.115f).margin(0.001));
	CHECK(BusyMillisecondsOf(spans[0]) == Approx(0.270f).margin(0.001));

	// The wait itself is all idle and no work, which is what makes it legible
	// as the answer rather than as another expensive-looking row.
	CHECK(spans[1].IdleMilliseconds == Approx(16.115f).margin(0.001));
	CHECK(BusyMillisecondsOf(spans[1]) == Approx(0.0f).margin(0.001));

	// A sibling that never waited reports no idle at all — the column has to
	// distinguish "did not wait" from "was not measured".
	CHECK(spans[2].IdleMilliseconds == Approx(0.0f).margin(0.001));
	CHECK(BusyMillisecondsOf(spans[2]) == Approx(0.231f).margin(0.001));

	// Busy plus idle is the inclusive time, for every span. The split loses
	// nothing, which is why the wall clock is still recoverable from the panel.
	for (const FrameSpan &span : spans) {
		CHECK(BusyMillisecondsOf(span) + span.IdleMilliseconds == Approx(span.Milliseconds).margin(0.001));
	}
}

TEST_CASE("busy is never negative when a child reports more than its parent", "[panelshares]") {
	// A `Reported` child carries time measured on another thread, so a parent
	// can legitimately contain more idle than it has wall clock. The columns
	// must not print a negative cost when that happens.
	std::vector<FrameSpan> spans(2);
	spans[0].Depth = 0;
	spans[0].Parent = 0;
	spans[0].Milliseconds = 1.0f;
	spans[0].SelfMilliseconds = 1.0f;
	spans[0].Category = ProfileCategory::Render;

	spans[1].Depth = 1;
	spans[1].Parent = 0;
	spans[1].Milliseconds = 5.0f;
	spans[1].SelfMilliseconds = 5.0f;
	spans[1].Category = ProfileCategory::Idle;
	spans[1].Reported = true;

	engine::core::AccumulateIdleMilliseconds(spans);

	CHECK(BusyMillisecondsOf(spans[0]) >= 0.0f);
	CHECK(BusyShares(Panel(spans, 1.0f, 0.0f))[0] >= 0.0f);
}

TEST_CASE("a frame with no idle is unchanged by the correction", "[panelshares]") {
	std::vector<FrameSpan> spans(1);
	spans[0].Depth = 0;
	spans[0].Parent = 0;
	spans[0].Milliseconds = 4.0f;
	spans[0].SelfMilliseconds = 4.0f;
	spans[0].Category = ProfileCategory::Simulation;

	// The uncapped case, where the fix must do nothing at all.
	CHECK(BusyShares(Panel(spans, 8.0f, 0.0f))[0] == Approx(0.5f).margin(0.001));
}

TEST_CASE("a parent chain that points at itself terminates", "[panelshares]") {
	std::vector<FrameSpan> spans(2);
	spans[0].Depth = 0;
	spans[0].Parent = 1;
	spans[0].Milliseconds = 1.0f;
	spans[0].Category = ProfileCategory::Idle;
	spans[0].SelfMilliseconds = 1.0f;

	spans[1].Depth = 0;
	spans[1].Parent = 0;
	spans[1].Milliseconds = 1.0f;
	spans[1].Category = ProfileCategory::Render;

	// A cycle is not something the frame graph should produce, and the overlay
	// must not hang if it ever does — a profiler that freezes the frame it is
	// profiling is worse than a wrong number.
	engine::core::AccumulateIdleMilliseconds(spans);
	const std::vector<float> shares = BusyShares(Panel(spans, 1.0f, 0.0f));
	CHECK(shares.size() == 2);
}
