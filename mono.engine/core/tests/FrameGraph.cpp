#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

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
	// zero - which is the same wrong answer the panel had before, arrived at
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

TEST_CASE("a dropped scope reaches the metrics sink", "[framegraph]") {
	// **The half of the drop that a headless program can see.** `Dropped()` is
	// read by the F5 overlay and by nothing else, so a server running parallel
	// compute lost spans and had no way to say so. `EndFrame` counts them into
	// `core::Metrics`, which the server drains and reports.
	engine::core::Metrics::Clear();

	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE("main");
			std::thread worker([] { ENGINE_PROFILE("worker"); });
			worker.join();
		}
		FrameGraph::EndFrame();
	}

	const auto counted = engine::core::Metrics::Get(FrameGraph::DROPPED_COUNTER);
	REQUIRE(counted.has_value());
	CHECK(counted->Value == 1.0);

	// Nothing is added on a frame that dropped nothing, so an absent row means
	// "lost no spans" rather than "nobody looked".
	engine::core::Metrics::Clear();
	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		{ ENGINE_PROFILE("main"); }
		FrameGraph::EndFrame();
	}
	CHECK_FALSE(engine::core::Metrics::Get(FrameGraph::DROPPED_COUNTER).has_value());
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
	// "first" - both are depth 1 following a depth-2 row. Which is why the
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

	// **The loop is timed as well as profiled**, and the assertion below is
	// against that rather than against a number of milliseconds. A ceiling like
	// "under 3 ms" is a claim about the machine: burns of a millisecond each
	// take that long on an idle box and can take three times as long on a loaded
	// one, so the test failed about one run in six while the code was correct.
	//
	// **A ratio was the first fix and it was not enough, for a reason worth
	// keeping.** The comment here used to say the wall clock and the profiler
	// stretch together - true only if the stretch is *uniform*, and a scheduler
	// does not work that way. It preempts one span. With four repeats a single
	// stall of three milliseconds puts the maximum past sixty percent of a loop
	// that should have been four, and the case failed under this repository's
	// own parallel test sweep while the code was right.
	//
	// **Sixteen repeats rather than four**, which widens the gap the assertion
	// is measuring instead of loosening the assertion. A total now reads about
	// sixteen times a single reading, so one preempted burn among sixteen is
	// still far below the ceiling - the discrimination gets stronger and the
	// flake goes, which is the opposite trade from raising the bound.
	constexpr int REPEATS = 16;

	const auto started = std::chrono::steady_clock::now();

	FrameGraph::BeginFrame();
	for (int repeat = 0; repeat < REPEATS; repeat++) {
		ENGINE_PROFILE("four-times");
		BurnMilliseconds(1.0);
	}
	FrameGraph::EndFrame();

	const float spent =
		std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();

	// Sixteen opens of about a millisecond each. **A total would read near the
	// whole loop; a single reading reads near a sixteenth of it** - and that is
	// the difference the column has to show, so that it compares with the
	// per-frame figure printed beside it.
	//
	// A third, which is nowhere near either answer: a sixteenth is far under it
	// and a total is far over, and one stalled burn among sixteen cannot cross
	// it however slow the machine got.
	const float worst = FrameGraph::RecentMaximum("four-times");
	INFO("worst " << worst << " ms of " << spent << " ms spent over " << REPEATS << " opens");
	REQUIRE(worst >= 0.5f);
	REQUIRE(worst < spent * 0.33f);
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

// --- timings measured somewhere else ------------------------------------------
//
// A worker's span cannot be recorded live - `Push` refuses anything off the
// owning thread - so the producer measures itself and hands the number back.
// What these check is that the handed-over number lands in the tree without
// corrupting the arithmetic of the spans that were measured here.

TEST_CASE("a reported span carries the duration it was given", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	FrameGraph::Report("worker", ProfileCategory::ECS, 12.5f);
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 1);
	REQUIRE(spans[0].Name == "worker");
	REQUIRE(spans[0].Milliseconds == 12.5f);
	REQUIRE(spans[0].Reported);
	REQUIRE(spans[0].Category == ProfileCategory::ECS);
}

TEST_CASE("a reported child does not make its parent's self time negative", "[framegraph]") {
	// The case the whole `Reported` flag exists for. Eight workers each
	// reporting five milliseconds under a batch that took almost none is
	// forty milliseconds of work inside a scope that waited for one - and
	// subtracting that from the parent would produce a number that is not
	// merely wrong, it is a different quantity.
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE_CAT("batch", ProfileCategory::ECS);
		for (int worker = 0; worker < 8; worker++) {
			FrameGraph::Report("worker", ProfileCategory::ECS, 5.0f);
		}
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 9);

	// The parent keeps the wall time it actually spent, not wall minus forty.
	REQUIRE(spans[0].Name == "batch");
	REQUIRE(spans[0].SelfMilliseconds >= 0.0f);
	REQUIRE(spans[0].SelfMilliseconds == spans[0].Milliseconds);
}

TEST_CASE("a measured child is still subtracted from its parent", "[framegraph]") {
	// The other half of the same rule: skipping reported children must not
	// have stopped ordinary nesting from working.
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE_CAT("outer", ProfileCategory::Engine);
		{
			ENGINE_PROFILE_CAT("inner", ProfileCategory::Engine);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);
	REQUIRE(spans[0].SelfMilliseconds < spans[0].Milliseconds);
	REQUIRE(spans[1].Milliseconds >= 1.0f);
}

TEST_CASE("reported time counts towards its category", "[framegraph]") {
	Collecting collecting;

	FrameGraph::BeginFrame();
	FrameGraph::Report("worlds", ProfileCategory::ECS, 7.0f);
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::CategoryMilliseconds(ProfileCategory::ECS) == 7.0f);
}

TEST_CASE("a reported span is a leaf and never becomes a parent", "[framegraph]") {
	// Its work happened elsewhere, so nothing recorded on this thread was
	// inside it. A sibling opened afterwards must sit beside it, not under it.
	Collecting collecting;

	FrameGraph::BeginFrame();
	FrameGraph::Report("worker", ProfileCategory::ECS, 1.0f);
	{ ENGINE_PROFILE_CAT("after", ProfileCategory::Engine); }
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);
	REQUIRE(spans[0].Depth == spans[1].Depth);
}

TEST_CASE("a negative duration is refused rather than clamped", "[framegraph]") {
	// It means the producer measured wrongly - two clocks, or a start it never
	// set. Folding it to zero would put a plausible bar on the graph where the
	// honest outcome is a missing one and a bump in the drop count.
	Collecting collecting;

	FrameGraph::BeginFrame();
	FrameGraph::Report("impossible", ProfileCategory::ECS, -1.0f);
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().empty());
	REQUIRE(FrameGraph::Dropped() == 1);
}

TEST_CASE("a report from a worker thread is dropped, not recorded", "[framegraph]") {
	// The same rule `Push` applies. `Report` exists so a worker can hand its
	// number to the owner, not so a worker can write into the owner's tree.
	Collecting collecting;

	FrameGraph::BeginFrame();
	std::thread([] { FrameGraph::Report("elsewhere", ProfileCategory::ECS, 3.0f); }).join();
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().empty());
	REQUIRE(FrameGraph::Dropped() == 1);
}

TEST_CASE("reporting while collection is off does nothing at all", "[framegraph]") {
	FrameGraph::SetEnabled(false);
	FrameGraph::BeginFrame();
	FrameGraph::Report("worker", ProfileCategory::ECS, 4.0f);
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::Spans().empty());
}

TEST_CASE("a reported span can be named at runtime", "[framegraph]") {
	Collecting collecting;

	const std::string host = "host.shared.0";

	FrameGraph::BeginFrame();
	FrameGraph::ReportNamed("host", host, ProfileCategory::Simulation, 2.5f);
	FrameGraph::ReportNamed("host", {}, ProfileCategory::Simulation, 1.5f);
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);
	REQUIRE(spans[0].Name == "host.shared.0");
	REQUIRE(spans[0].Milliseconds == 2.5f);

	// Empty text falls back to the stable name rather than drawing a blank bar.
	REQUIRE(spans[1].Name == "host");
}

TEST_CASE("every category has a name of its own", "[framegraph]") {
	// `GetCategoryName` is a switch, and a switch over an enum class with no
	// default is a warning at worst - a category added without a case falls
	// through to "?" and the panel draws two bars with the same label. There
	// is nothing in a build that catches that, so this does.
	std::vector<std::string_view> names;
	for (size_t index = 0; index < static_cast<size_t>(ProfileCategory::Count); index++) {
		const std::string_view name = engine::core::GetCategoryName(static_cast<ProfileCategory>(index));

		REQUIRE(name != "?");
		REQUIRE_FALSE(name.empty());
		REQUIRE(std::find(names.begin(), names.end(), name) == names.end());
		names.push_back(name);
	}

	// The sentinel is not a category and must not be given a name that reads
	// like one, or a loop written with `<=` draws a bar for it.
	REQUIRE(engine::core::GetCategoryName(ProfileCategory::Count) == "?");
}

TEST_CASE("the subsystem categories carved out of ECS total separately", "[framegraph]") {
	Collecting collecting;

	// The whole point of `Physics`, `Network` and `Assets`: this work used to
	// land in `ECS` and `Engine`, where a slow broadphase and a slow scheduler
	// were one number.
	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE_CAT("ecs.systems", ProfileCategory::ECS);
		{
			ENGINE_PROFILE_CAT("physics.solve", ProfileCategory::Physics);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	{
		ENGINE_PROFILE_CAT("replica.poll", ProfileCategory::Network);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	{
		ENGINE_PROFILE_CAT("HashTree::Verify", ProfileCategory::Assets);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	FrameGraph::EndFrame();

	REQUIRE(FrameGraph::CategoryMilliseconds(ProfileCategory::Physics) >= 1.0f);
	REQUIRE(FrameGraph::CategoryMilliseconds(ProfileCategory::Network) >= 1.0f);
	REQUIRE(FrameGraph::CategoryMilliseconds(ProfileCategory::Assets) >= 1.0f);

	// The physics span is nested inside the ECS one, and self time is what the
	// totals are made of - so the scheduler keeps only what it spent itself.
	// This is the assertion that says the carve-out actually moved the time
	// rather than counting it twice.
	REQUIRE(
		FrameGraph::CategoryMilliseconds(ProfileCategory::ECS) <
		FrameGraph::CategoryMilliseconds(ProfileCategory::Physics)
	);
}

TEST_CASE("the tracked-name budget is released when collection stops", "[framegraph]") {
	// **A quota spent once per process rather than once per session.** The
	// window was cleared on every toggle and the name table that indexes it
	// was not, so a panel opened in one scene consumed names a panel opened
	// in the next could not get back. Filling it here is the only way to see
	// that: below the limit the two behaviours are identical.
	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		for (size_t index = 0; index < FrameGraph::MAXIMUM_HISTORY_NAMES; index++) {
			const std::string name = "filler." + std::to_string(index);
			ENGINE_PROFILE_DYNAMIC("filler", name, ProfileCategory::Engine);
		}
		FrameGraph::EndFrame();
	}

	// A second session, which must start with the whole budget.
	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE("the.next.scene");
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		FrameGraph::EndFrame();

		// Zero is what a name the history turned away reads as, and it is also
		// what a span costing nothing reads as - so this span sleeps, and the
		// assertion is about a duration rather than about presence.
		REQUIRE(FrameGraph::RecentMaximum("the.next.scene") >= 1.0f);
	}
}

TEST_CASE("a stopped session leaves no readings behind", "[framegraph]") {
	{
		Collecting collecting;
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE("first.session");
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		FrameGraph::EndFrame();
		REQUIRE(FrameGraph::RecentMaximum("first.session") >= 1.0f);
	}

	// Collection only runs while the panel is open, so keeping the last
	// session's frames would put a gap of arbitrary length in the middle of a
	// five-second window and give the column a worst case from another scene.
	{
		Collecting collecting;
		REQUIRE(FrameGraph::RecentMaximum("first.session") == 0.0f);
		REQUIRE(FrameGraph::HistoryFrames() == 0);
	}
}

TEST_CASE("two profiling scopes may share one C++ scope", "[framegraph]") {
	// **This case earns its keep at compile time.** `ENGINE_PROFILE_CAT`
	// expanded to Tracy's `ZoneScopedN`, which declares a variable with one
	// fixed name - so a second macro beside the first failed to build, in
	// `ENGINE_TRACY` configurations only, with an error naming a Tracy
	// internal from a header the caller never wrote. Nothing said so and
	// nothing would have caught it; the code below simply does not compile if
	// it comes back.
	Collecting collecting;

	const std::string runtime = "runtime.named";

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE("first");
		ENGINE_PROFILE_CAT("second", ProfileCategory::Render);
		ENGINE_PROFILE_DYNAMIC("third", runtime, ProfileCategory::Physics);
	}
	FrameGraph::EndFrame();

	// All three live in one C++ scope, so each opens inside the one before it
	// and they close in reverse. Nesting, not siblings - which is what the
	// stack the scopes are pushed onto means.
	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 3);
	REQUIRE(spans[0].Name == "first");
	REQUIRE(spans[0].Depth == 0);
	REQUIRE(spans[1].Name == "second");
	REQUIRE(spans[1].Depth == 1);
	REQUIRE(spans[1].Category == ProfileCategory::Render);
	REQUIRE(spans[2].Name == "runtime.named");
	REQUIRE(spans[2].Depth == 2);
	REQUIRE(spans[2].Category == ProfileCategory::Physics);
}

// --- folded stacks ------------------------------------------------------------
//
// The flamegraph half. The arithmetic is a free function over hand-built spans,
// so these are about the fold and not about whether a collector was running.

TEST_CASE("a folded line is a stack and its self time", "[framegraph]") {
	const std::vector<engine::core::FrameSpan> spans{
		{.Name = "tick", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .SelfMilliseconds = 1.0f},
		{.Name = "publish", .Depth = 1, .Parent = 0, .SelfMilliseconds = 2.0f},
		{.Name = "pack", .Depth = 2, .Parent = 1, .SelfMilliseconds = 4.0f},
	};

	engine::core::FoldedStacks totals;
	engine::core::AccumulateFoldedStacks(spans, totals);

	REQUIRE(totals.size() == 3);
	REQUIRE(totals.at("tick") == 1000.0);
	REQUIRE(totals.at("tick;publish") == 2000.0);
	REQUIRE(totals.at("tick;publish;pack") == 4000.0);
}

TEST_CASE("folding accumulates across frames rather than replacing them", "[framegraph]") {
	const std::vector<engine::core::FrameSpan> spans{
		{.Name = "tick", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .SelfMilliseconds = 0.5f},
	};

	// The whole reason this is a running total: a flamegraph of a stress run is
	// the run, not the last frame of it.
	engine::core::FoldedStacks totals;
	for (int frame = 0; frame < 4; frame++) {
		engine::core::AccumulateFoldedStacks(spans, totals);
	}

	REQUIRE(totals.at("tick") == 2000.0);
}

TEST_CASE("two spans of one name under two parents stay apart", "[framegraph]") {
	// A flamegraph's whole value over a flat table: `seal` under the delta path
	// and `seal` under the snapshot path are two different costs.
	const std::vector<engine::core::FrameSpan> spans{
		{.Name = "tick", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .SelfMilliseconds = 0.0f},
		{.Name = "delta", .Depth = 1, .Parent = 0, .SelfMilliseconds = 0.0f},
		{.Name = "seal", .Depth = 2, .Parent = 1, .SelfMilliseconds = 3.0f},
		{.Name = "snapshot", .Depth = 1, .Parent = 0, .SelfMilliseconds = 0.0f},
		{.Name = "seal", .Depth = 2, .Parent = 3, .SelfMilliseconds = 5.0f},
	};

	engine::core::FoldedStacks totals;
	engine::core::AccumulateFoldedStacks(spans, totals);

	REQUIRE(totals.at("tick;delta;seal") == 3000.0);
	REQUIRE(totals.at("tick;snapshot;seal") == 5000.0);
}

TEST_CASE("a semicolon in a name does not become a stack frame", "[framegraph]") {
	// Runtime names are copied from scripts and asset paths, so the separator is
	// text somebody can write. Splitting on it would invent a frame.
	const std::vector<engine::core::FrameSpan> spans{
		{.Name = "a;b", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .SelfMilliseconds = 1.0f},
	};

	engine::core::FoldedStacks totals;
	engine::core::AccumulateFoldedStacks(spans, totals);

	REQUIRE(totals.count("a;b") == 0);
	REQUIRE(totals.at("a:b") == 1000.0);
}

TEST_CASE("a folded capture covers every frame the collector saw", "[framegraph]") {
	const auto path = std::filesystem::temp_directory_path() / "atomic-framegraph-test.folded";
	std::filesystem::remove(path);

	{
		Collecting collecting;
		FrameGraph::SetFoldingEnabled(true);

		for (int frame = 0; frame < 5; frame++) {
			FrameGraph::BeginFrame();
			{
				ENGINE_PROFILE("outer");
				ENGINE_PROFILE("inner");
				BurnMilliseconds(0.2);
			}
			FrameGraph::EndFrame();
		}

		REQUIRE(FrameGraph::FoldedFrames() == 5);
		REQUIRE(FrameGraph::WriteFolded(path));
		FrameGraph::SetFoldingEnabled(false);
	}

	std::ifstream in(path);
	REQUIRE(in);
	const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	in.close();
	std::filesystem::remove(path);

	// The nested span carries the time, so it is the line with a number on it.
	REQUIRE(text.find("outer;inner ") != std::string::npos);
}

TEST_CASE("turning folding on clears what the last capture accumulated", "[framegraph]") {
	Collecting collecting;

	FrameGraph::SetFoldingEnabled(true);
	FrameGraph::BeginFrame();
	{ ENGINE_PROFILE("first.capture"); }
	FrameGraph::EndFrame();
	REQUIRE(FrameGraph::FoldedFrames() == 1);

	// A second capture is a second run. Carrying the first one's stacks into it
	// would compare a change against a graph containing the code before it.
	FrameGraph::SetFoldingEnabled(true);
	REQUIRE(FrameGraph::FoldedFrames() == 0);

	const auto path = std::filesystem::temp_directory_path() / "atomic-framegraph-second.folded";
	std::filesystem::remove(path);
	REQUIRE_FALSE(FrameGraph::WriteFolded(path));
	REQUIRE_FALSE(std::filesystem::exists(path));

	FrameGraph::SetFoldingEnabled(false);
}

// --- the event scheduler -----------------------------------------------------
//
// The rules are process-wide, the same as the collector, so every case here
// disarms on the way out.

namespace {
	struct Armed {
		explicit Armed(std::vector<engine::core::FrameTrigger> rules) : Rules(std::move(rules)) {
			FrameGraph::SetTriggers(Rules);
			FrameGraph::ClearTrigger();
		}
		~Armed() {
			FrameGraph::SetTriggers({});
			FrameGraph::ClearTrigger();
		}

		std::vector<engine::core::FrameTrigger> Rules;
	};

	// A frame containing one span that takes at least `milliseconds`.
	void SlowFrame(std::string_view name, int milliseconds) {
		FrameGraph::BeginFrame();
		{
			ENGINE_PROFILE_DYNAMIC("rule", name, ProfileCategory::Engine);
			std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
		}
		FrameGraph::EndFrame();
	}
}

TEST_CASE("a rule stops the graph on the frame that meets it", "[framegraph]") {
	Collecting collecting;
	Armed armed({engine::core::FrameTrigger{.Name = "slow", .Threshold = 1.0f}});

	REQUIRE(FrameGraph::Triggered() == nullptr);

	SlowFrame("slow", 3);

	const engine::core::FrameTriggerHit *hit = FrameGraph::Triggered();
	REQUIRE(hit != nullptr);
	REQUIRE(hit->Subject == "slow");
	REQUIRE(hit->Reading > 1.0f);
}

TEST_CASE("the latch holds past the frame that set it", "[framegraph]") {
	Collecting collecting;
	Armed armed({engine::core::FrameTrigger{.Name = "slow", .Threshold = 1.0f}});

	SlowFrame("slow", 3);
	REQUIRE(FrameGraph::Triggered() != nullptr);

	// **The point of the latch.** A reader looks a few times a second and the
	// spike is one frame; a hit that cleared itself on the next frame would be
	// gone before anything saw it.
	FrameGraph::BeginFrame();
	FrameGraph::EndFrame();
	REQUIRE(FrameGraph::Triggered() != nullptr);

	FrameGraph::ClearTrigger();
	REQUIRE(FrameGraph::Triggered() == nullptr);
}

TEST_CASE("an under rule waits for the span rather than firing without it", "[framegraph]") {
	Collecting collecting;
	Armed armed({engine::core::FrameTrigger{
		.Name = "absent",
		.Test = engine::core::TriggerTest::Below,
		.Threshold = 1000.0f,
	}});

	// The span never runs, so there is no reading. Read as a zero this would
	// fire on every frame, which is the bug the optional exists to prevent.
	SlowFrame("something else", 1);
	REQUIRE(FrameGraph::Triggered() == nullptr);

	SlowFrame("absent", 1);
	REQUIRE(FrameGraph::Triggered() != nullptr);
}

TEST_CASE("a rule that is off does not fire", "[framegraph]") {
	Collecting collecting;
	Armed armed({engine::core::FrameTrigger{.Name = "slow", .Threshold = 1.0f, .Enabled = false}});

	SlowFrame("slow", 3);
	REQUIRE(FrameGraph::Triggered() == nullptr);
}

TEST_CASE("device time is its own category and does not inflate render", "[framegraph]") {
	// **The claim `ProfileCategory::Gpu` exists to make true.** `Render` is CPU
	// wall clock spent walking a draw list and recording commands; GPU time is
	// the device executing them afterwards, overlapping that recording and the
	// next frame's. Added together they read as a renderer costing twice what it
	// does, and - worse - a frame CPU-bound on command recording becomes
	// indistinguishable from one GPU-bound on fill rate, which is the single
	// thing a reader most wants this panel to separate.
	Collecting collecting;

	FrameGraph::BeginFrame();
	{
		ENGINE_PROFILE_CAT("record", ProfileCategory::Render);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

		// What `render::Renderer::CollectTimings` does once a query pool
		// resolves: a duration measured by the device, handed over after the
		// fact because nothing on the CPU can hold a scope open across a pass.
		FrameGraph::Report("opaque", ProfileCategory::Gpu, 9.0f);
	}
	FrameGraph::EndFrame();

	const float device = FrameGraph::CategoryMilliseconds(ProfileCategory::Gpu);
	REQUIRE(device > 8.9f);
	REQUIRE(device < 9.1f);

	// The CPU side kept its own number. Nine milliseconds of device time did not
	// land on it.
	const float render = FrameGraph::CategoryMilliseconds(ProfileCategory::Render);
	REQUIRE(render > 0.0f);
	REQUIRE(render < 9.0f);
}

TEST_CASE("device time reaches the flamegraph as its own line", "[framegraph]") {
	// A folded stack is what a flamegraph renderer reads, and until v0.19 no GPU
	// measurement reached one: the device timings existed but only a Studio panel
	// ever read them, so every `render` bar in a flamegraph was command recording
	// and the device was absent from the picture entirely.
	const std::vector<engine::core::FrameSpan> spans{
		{.Name = "frame", .Depth = 0, .Parent = FrameGraph::NO_PARENT, .SelfMilliseconds = 1.0f},
		{.Name = "record",
		 .Depth = 1,
		 .Parent = 0,
		 .SelfMilliseconds = 2.0f,
		 .Category = ProfileCategory::Render},
		{.Name = "opaque",
		 .Depth = 1,
		 .Parent = 0,
		 .SelfMilliseconds = 9.0f,
		 .Category = ProfileCategory::Gpu,
		 .Reported = true},
	};

	engine::core::FoldedStacks totals;
	engine::core::AccumulateFoldedStacks(spans, totals);

	// **Its own line, kept apart from the recording that dispatched it.** A
	// reported span keeps its own stack rather than being merged, which is why
	// the totals of a run that used a GPU add to more than its wall clock - the
	// same arithmetic a worker pool already produces, not a new exception.
	REQUIRE(totals.at("frame;opaque") == 9000.0);
	REQUIRE(totals.at("frame;record") == 2000.0);
	REQUIRE(totals.at("frame") == 1000.0);
}
