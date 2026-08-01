#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

TEST_SUITE_ID("engine.core.framegraph")

using engine::core::FrameGraph;
using engine::core::ProfileCategory;

namespace {
	// The graph is process-wide state, so each case has to leave it off.
	struct Collecting {
		Collecting() {
			FrameGraph::SetEnabled(true);
		}
		~Collecting() {
			FrameGraph::SetEnabled(false);
		}
	};
}

TEST_CASE("nothing is recorded while collection is off", "[framegraph]") {
	FrameGraph::SetEnabled(false);

	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("off"); }
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().empty());
}

TEST_CASE("a scope becomes a span", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("outer"); }
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 1);
	REQUIRE(spans[0].Name == "outer");
	REQUIRE(spans[0].Depth == 0);
}

TEST_CASE("nesting becomes depth", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("outer");
		{
			ENGINE_PROFILE_CAT("inner", ProfileCategory::Render);
			{ ENGINE_PROFILE("innermost"); }
		}
		{ ENGINE_PROFILE("sibling"); }
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 4);

	// Open order, which is what a flamegraph is drawn from left to right.
	REQUIRE(spans[0].Name == "outer");
	REQUIRE(spans[0].Depth == 0);
	REQUIRE(spans[1].Name == "inner");
	REQUIRE(spans[1].Depth == 1);
	REQUIRE(spans[1].Category == ProfileCategory::Render);
	REQUIRE(spans[2].Name == "innermost");
	REQUIRE(spans[2].Depth == 2);
	REQUIRE(spans[3].Name == "sibling");
	REQUIRE(spans[3].Depth == 1);
}

TEST_CASE("self time excludes direct children", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("outer");
		{
			ENGINE_PROFILE("inner");
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);

	// The child holds nearly all of the parent's duration, so the parent's own
	// time is the small remainder. Loose bounds: this is a real clock.
	REQUIRE(spans[1].Milliseconds >= 3.0f);
	REQUIRE(spans[0].SelfMilliseconds < spans[1].Milliseconds);
	REQUIRE(spans[0].SelfMilliseconds >= 0.0f);
}

TEST_CASE("category totals are self time, not double-counted", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE_CAT("draw", ProfileCategory::Render);
		{
			ENGINE_PROFILE_CAT("submit", ProfileCategory::Render);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	FrameGraph::EndFrame();

	// Two nested Render spans, so a naive sum would report about twice the
	// frame. Self time keeps the total honest.
	const float render = FrameGraph::CategoryMilliseconds(ProfileCategory::Render);
	REQUIRE(render <= FrameGraph::FrameMilliseconds() + 0.5f);
	REQUIRE(render >= 1.0f);
}

TEST_CASE("work inside no span is reported rather than lost", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("marked"); }
	// Nobody put a scope around this, which is exactly the case. A panel that
	// lists the span above and nothing else is not reporting a fast frame; it
	// is failing to report a slow one.
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().size() == 1);
	REQUIRE(FrameGraph::UnmarkedMilliseconds() >= 4.0f);
	REQUIRE(FrameGraph::UnmarkedMilliseconds() <= FrameGraph::FrameMilliseconds());
}

TEST_CASE("a fully instrumented frame has nothing unmarked", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("everything");
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	FrameGraph::EndFrame();

	// The one root span covers the frame, so the gap is whatever BeginFrame and
	// EndFrame cost between them. Loose bound: this is a real clock, and the
	// claim is "nearly nothing", not "zero".
	REQUIRE(FrameGraph::UnmarkedMilliseconds() < 1.0f);
}

TEST_CASE("unmarked time counts root spans once, not every span", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("outer");
		{
			ENGINE_PROFILE("inner");
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}
	FrameGraph::EndFrame();

	// Root durations are inclusive. Summing every span instead would count the
	// nested 4 ms twice, drive the total past the frame, and clamp the gap to
	// zero — which is the same wrong answer the panel had before, arrived at
	// from the other direction.
	REQUIRE(FrameGraph::Spans().size() == 2);
	REQUIRE(FrameGraph::UnmarkedMilliseconds() < 1.0f);
}

TEST_CASE("the marked and unmarked halves add up to the frame", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("first");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(3));
	{
		ENGINE_PROFILE("second");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	FrameGraph::EndFrame();

	// This is the property the overlay depends on: the rows a person reads and
	// the heading above them describe the same frame.
	float roots = 0.0f;
	for (const auto &span : FrameGraph::Spans()) {
		if (span.Depth == 0) {
			roots += span.Milliseconds;
		}
	}

	const float accounted = roots + FrameGraph::UnmarkedMilliseconds();
	REQUIRE(accounted >= FrameGraph::FrameMilliseconds() - 0.5f);
	REQUIRE(accounted <= FrameGraph::FrameMilliseconds() + 0.5f);
	REQUIRE(FrameGraph::UnmarkedMilliseconds() >= 2.0f);
}

TEST_CASE("a frame nobody collected reports no unmarked time", "[framegraph]") {
	FrameGraph::SetEnabled(false);

	FrameGraph::BeginFrame();
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	FrameGraph::EndFrame();

	// Collection is off, so there is no frame to be missing anything from. A
	// number here would be a reading of something nobody measured.
	REQUIRE(FrameGraph::UnmarkedMilliseconds() == 0.0f);
}

TEST_CASE("the published frame survives the next one being built", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("first"); }
	FrameGraph::EndFrame();

	// The overlay reads last frame's spans while this frame is in flight.
	FrameGraph::BeginFrame();
	REQUIRE(FrameGraph::Spans().size() == 1);
	REQUIRE(FrameGraph::Spans()[0].Name == "first");
	FrameGraph::EndFrame();
}

TEST_CASE("a scope on another thread is dropped and counted", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("main");
		std::thread worker([] { ENGINE_PROFILE("worker"); });
		worker.join();
	}
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().size() == 1);
	REQUIRE(FrameGraph::Spans()[0].Name == "main");
	REQUIRE(FrameGraph::Dropped() == 1);
}

// --- parentage ---------------------------------------------------------------

TEST_CASE("a span records which span it opened inside", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("root");
		{
			ENGINE_PROFILE("first");
			{ ENGINE_PROFILE("grandchild"); }
		}
		{ ENGINE_PROFILE("second"); }
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 4);

	// The depth column alone cannot tell "second" apart from a second child of
	// "first" — both are depth 1 following a depth-2 row. Which is why the
	// parent is stored rather than re-derived.
	REQUIRE(spans[0].Parent == FrameGraph::NO_PARENT);
	REQUIRE(spans[1].Name == "first");
	REQUIRE(spans[1].Parent == 0);
	REQUIRE(spans[2].Name == "grandchild");
	REQUIRE(spans[2].Parent == 1);
	REQUIRE(spans[3].Name == "second");
	REQUIRE(spans[3].Parent == 0);
}

// --- the depth budget ---------------------------------------------------------

namespace {
	// Opens `remaining` nested scopes and returns. Recursive so that the
	// nesting is real rather than a loop that opens and closes.
	void Nest(uint32_t remaining) {
		if (remaining == 0) {
			return;
		}
		ENGINE_PROFILE("nested");
		Nest(remaining - 1);
	}
}

TEST_CASE("nesting past the depth budget is dropped and counted", "[framegraph]") {
	Collecting collecting;

	constexpr uint32_t OVERSHOOT = 5;

	FrameGraph::BeginFrame();
	Nest(FrameGraph::MAXIMUM_DEPTH + OVERSHOOT);
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().size() == FrameGraph::MAXIMUM_DEPTH);
	REQUIRE(FrameGraph::Dropped() == OVERSHOOT);

	for (const auto &span : FrameGraph::Spans()) {
		REQUIRE(span.Depth < FrameGraph::MAXIMUM_DEPTH);
	}
}

TEST_CASE("a sibling after a too-deep subtree is at the right depth", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("root");
		Nest(FrameGraph::MAXIMUM_DEPTH + 3);
		{
			// This is the assertion the depth budget exists to keep true. The
			// dropped scopes still moved the depth, so if their closes did not
			// move it back, this sibling is recorded several levels too deep and
			// the whole tree below it is wrong.
			ENGINE_PROFILE("sibling");
		}
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	const auto sibling =
		std::find_if(spans.begin(), spans.end(), [](const auto &span) { return span.Name == "sibling"; });

	REQUIRE(sibling != spans.end());
	REQUIRE(sibling->Depth == 1);
	REQUIRE(sibling->Parent == 0);
}

// --- runtime names ------------------------------------------------------------

TEST_CASE("a copied name outlives the string it came from", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		// Destroyed before the frame ends, and long before the overlay would
		// read the published span. The whole reason the copying form exists.
		const std::string built = std::string("chunk.") + std::to_string(42);
		ENGINE_PROFILE_DYNAMIC("script", std::string_view(built), ProfileCategory::Script);
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 1);
	REQUIRE(spans[0].Name == "chunk.42");
	REQUIRE(spans[0].Category == ProfileCategory::Script);
}

TEST_CASE("a copied name falls back when the caller had nothing to say", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE_DYNAMIC("script", std::string_view(), ProfileCategory::Script); }
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().size() == 1);
	REQUIRE(FrameGraph::Spans()[0].Name == "script");
}

TEST_CASE("copied names do not collide across frames", "[framegraph]") {
	Collecting collecting;

	for (int frame = 0; frame < 3; frame++) {
		FrameGraph::BeginFrame();
		{
			const std::string built = "frame." + std::to_string(frame);
			ENGINE_PROFILE_DYNAMIC("script", std::string_view(built), ProfileCategory::Script);
		}
		FrameGraph::EndFrame();

		// The name pool is reused between frames rather than freed, so a stale
		// buffer would show the previous frame's text here.
		REQUIRE(FrameGraph::Spans().size() == 1);
		REQUIRE(FrameGraph::Spans()[0].Name == "frame." + std::to_string(frame));
	}
}

// --- the recent maximum -------------------------------------------------------

namespace {
	// A span that costs a measurable amount, so a spike is distinguishable
	// from an ordinary frame rather than being lost in clock noise.
	void BurnMilliseconds(double milliseconds) {
		const uint64_t until =
			engine::core::Clock::Nanoseconds() + static_cast<uint64_t>(milliseconds * 1'000'000.0);
		while (engine::core::Clock::Nanoseconds() < until) {}
	}
}

TEST_CASE("the recent maximum keeps a spike an ordinary frame would hide", "[framegraph]") {
	Collecting collecting;

	// Thirty cheap frames and one expensive one, which is the shape the column
	// exists for: repainted at any rate a person can watch, the panel shows the
	// cheap reading and the spike is the one worth knowing about.
	for (int frame = 0; frame < 30; frame++) {
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE("occasionally-slow");
			if (frame == 12) {
				BurnMilliseconds(4.0);
			}
		}
		FrameGraph::EndFrame();
	}

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 1);

	// The last frame was a cheap one.
	REQUIRE(spans[0].Milliseconds < 1.0f);
	// And the window still remembers the twelfth.
	REQUIRE(FrameGraph::RecentMaximum("occasionally-slow") >= 3.5f);
}

TEST_CASE("the recent maximum is a single reading, not a total", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	for (int repeat = 0; repeat < 4; repeat++) {
		ENGINE_PROFILE("four-times");
		BurnMilliseconds(1.0);
	}
	FrameGraph::EndFrame();

	// Four opens of about a millisecond each. A total would read near 4; what
	// the column has to show is the worst of the four, so that it compares with
	// the per-frame figure printed beside it.
	const float worst = FrameGraph::RecentMaximum("four-times");
	REQUIRE(worst >= 0.5f);
	REQUIRE(worst < 3.0f);
}

TEST_CASE("a span that stops running decays out of the window", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("transient");
		BurnMilliseconds(2.0);
	}
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::RecentMaximum("transient") >= 1.5f);

	// Every frame writes a zero for a span it did not contain, so the ring
	// eventually forgets. Without that, one expensive frame would keep its
	// reading on the panel forever.
	for (size_t frame = 0; frame < FrameGraph::RECENT_FRAMES; frame++) {
		FrameGraph::BeginFrame();
		{ ENGINE_PROFILE("something-else"); }
		FrameGraph::EndFrame();
	}

	REQUIRE(FrameGraph::RecentMaximum("transient") == 0.0f);
}

TEST_CASE("a span nobody has seen has no recent maximum", "[framegraph]") {
	Collecting collecting;
	REQUIRE(FrameGraph::RecentMaximum("never-run") == 0.0f);
}

TEST_CASE("closing the panel forgets the window", "[framegraph]") {
	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE("watched");
			BurnMilliseconds(2.0);
		}
		FrameGraph::EndFrame();
		REQUIRE(FrameGraph::HistoryFrames() == 1);
	}

	// Collection only runs while the panel is open. Keeping frames from the
	// last time it was open would put a gap of arbitrary length in the middle
	// of the window and give the column a worst reading from a different scene.
	Collecting reopened;
	REQUIRE(FrameGraph::HistoryFrames() == 0);
	REQUIRE(FrameGraph::RecentMaximum("watched") == 0.0f);
}

// --- the snapshot ---------------------------------------------------------------

TEST_CASE("a snapshot names the spans and the worst frames", "[framegraph]") {
	const auto path = std::filesystem::temp_directory_path() / "atomic-framegraph-test.txt";
	std::filesystem::remove(path);

	{
		Collecting collecting;
		for (int frame = 0; frame < 20; frame++) {
			FrameGraph::BeginFrame();
			{
				ENGINE_PROFILE("steady");
				{
					ENGINE_PROFILE("spiky");
					if (frame == 9) {
						BurnMilliseconds(3.0);
					}
				}
			}
			FrameGraph::EndFrame();
		}

		REQUIRE(FrameGraph::WriteSnapshot(path));
	}

	std::ifstream in(path);
	REQUIRE(in);
	const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	in.close();
	std::filesystem::remove(path);

	REQUIRE(text.find("steady") != std::string::npos);
	REQUIRE(text.find("spiky") != std::string::npos);
	REQUIRE(text.find("20 frames") != std::string::npos);
	REQUIRE(text.find("worst") != std::string::npos);
	// The columns a spike hunt reads.
	REQUIRE(text.find("p99") != std::string::npos);
}

TEST_CASE(
	"a snapshot with nothing retained refuses rather than writing an "
	"empty file",
	"[framegraph]"
) {
	FrameGraph::SetEnabled(false);

	const auto path = std::filesystem::temp_directory_path() / "atomic-framegraph-empty.txt";
	std::filesystem::remove(path);

	// F8 without F5 has nothing to say, and a zero-byte file that looks like a
	// capture is worse than a refusal the caller can report.
	REQUIRE_FALSE(FrameGraph::WriteSnapshot(path));
	REQUIRE_FALSE(std::filesystem::exists(path));
}
