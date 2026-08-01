#include <engine/render/DebugPanels.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.render.debugpanels")
TEST_DEPENDS("engine.render.overlay")
TEST_DEPENDS("engine.core.framegraph")

using Catch::Approx;
using engine::core::FrameSpan;
using engine::core::ProfileCategory;
using engine::render::DebugPanelData;
using engine::render::DrawDebugPanels;
using engine::render::FrameStatistics;
using engine::render::OverlayImage;
using engine::render::ProfilerTab;

namespace {
	bool AnyPixelSet(const OverlayImage &image) {
		for (size_t index = 0; index < image.GetByteCount(); index++) {
			if (image.GetPixels()[index] != 0) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("statistics start empty", "[panels]") {
	FrameStatistics statistics;
	REQUIRE_FALSE(statistics.HasSamples());
	REQUIRE(statistics.Current() == Approx(0.0f));
	REQUIRE(statistics.Average() == Approx(0.0f));
}

TEST_CASE("a zero delta is not recorded", "[panels]") {
	FrameStatistics statistics;

	// 1/0 is the whole reason this guard exists.
	statistics.Record(0.0, 0.0f);
	statistics.Record(0.1, -1.0f);

	REQUIRE_FALSE(statistics.HasSamples());
}

TEST_CASE("a steady frame time gives that frame rate", "[panels]") {
	FrameStatistics statistics;
	for (int frame = 0; frame < 100; frame++) {
		statistics.Record(frame * (1.0 / 60.0), 1.0f / 60.0f);
	}

	REQUIRE(statistics.Current() == Approx(60.0f).margin(0.01));
	REQUIRE(statistics.Average() == Approx(60.0f).margin(0.01));
	REQUIRE(statistics.Minimum() == Approx(60.0f).margin(0.01));
	REQUIRE(statistics.Maximum() == Approx(60.0f).margin(0.01));
	REQUIRE(statistics.Jitter() == Approx(0.0f).margin(0.01));
}

TEST_CASE("minimum is the worst frame, not the smallest delta", "[panels]") {
	FrameStatistics statistics;

	// Ninety-nine good frames and one 100 ms hitch. The average stays high and
	// the minimum is what tells you the game stuttered — getting these the
	// wrong way round makes the panel say the opposite of the truth.
	double now = 0.0;
	for (int frame = 0; frame < 99; frame++) {
		statistics.Record(now, 1.0f / 200.0f);
		now += 1.0 / 200.0;
	}
	statistics.Record(now, 0.1f);

	REQUIRE(statistics.Minimum() == Approx(10.0f).margin(0.1));
	REQUIRE(statistics.Maximum() == Approx(200.0f).margin(0.1));
	REQUIRE(statistics.Average() > 60.0f);
	REQUIRE(statistics.Average() < 200.0f);
}

TEST_CASE("the average is the mean delta inverted, not the mean of the rates", "[panels]") {
	FrameStatistics statistics;

	// One frame at 10 ms and one at 30 ms: 50 frames over one second's worth of
	// deltas is 50 FPS. Averaging 100 and 33.3 would say 66.7, which overstates
	// it by a third.
	statistics.Record(0.0, 0.010f);
	statistics.Record(0.01, 0.030f);

	REQUIRE(statistics.Average() == Approx(50.0f).margin(0.01));
}

TEST_CASE("samples outside the window are dropped", "[panels]") {
	FrameStatistics statistics;

	statistics.Record(0.0, 0.5f);
	REQUIRE(statistics.SampleCount() == 1);

	statistics.Record(FrameStatistics::WINDOW_SECONDS + 1.0, 1.0f / 60.0f);
	REQUIRE(statistics.SampleCount() == 1);
	REQUIRE(statistics.Minimum() == Approx(60.0f).margin(0.1));
}

TEST_CASE("jitter is the mean change between frames", "[panels]") {
	FrameStatistics statistics;

	// 10 ms, 20 ms, 10 ms: two changes of 10 ms each.
	statistics.Record(0.00, 0.010f);
	statistics.Record(0.01, 0.020f);
	statistics.Record(0.03, 0.010f);

	REQUIRE(statistics.Jitter() == Approx(10.0f).margin(0.01));
}

TEST_CASE("the window survives the ring growing and wrapping", "[panels]") {
	FrameStatistics statistics;

	// Far past the initial capacity, so the storage doubles several times and
	// then the live range wraps the end of it repeatedly. Both are where a ring
	// goes wrong, and both are silent when they do — the numbers stay plausible
	// and stop being right.
	constexpr int COUNT = 5000;
	for (int index = 0; index < COUNT; index++) {
		statistics.Record(static_cast<double>(index) * 0.001, 0.010f);
	}

	REQUIRE(statistics.SampleCount() == COUNT);
	REQUIRE(statistics.Average() == Approx(100.0f).margin(0.1));
	REQUIRE(statistics.Minimum() == Approx(100.0f).margin(0.1));
	REQUIRE(statistics.Maximum() == Approx(100.0f).margin(0.1));
	REQUIRE(statistics.Jitter() == Approx(0.0f).margin(0.001));

	// Now slide the window across the buffer with a different value, so the
	// answer changes only if the old samples really did leave.
	for (int index = 0; index < COUNT; index++) {
		const double now = FrameStatistics::WINDOW_SECONDS + static_cast<double>(index) * 0.001;
		statistics.Record(now, 0.020f);
	}

	REQUIRE(statistics.Average() == Approx(50.0f).margin(0.1));
	REQUIRE(statistics.Minimum() == Approx(50.0f).margin(0.1));
}

TEST_CASE("a wrapped window reads its samples in order", "[panels]") {
	FrameStatistics statistics;

	// Enough to wrap, then one distinctly slow frame last. Current() reads the
	// newest sample, which after a wrap is not the highest index — taking the
	// back of the storage instead of the newest sample is the classic ring bug
	// and it reports a frame from twenty seconds ago as the current one.
	for (int index = 0; index < 600; index++) {
		statistics.Record(static_cast<double>(index) * 0.001, 0.010f);
	}
	statistics.Record(0.601, 0.040f);

	REQUIRE(statistics.Current() == Approx(25.0f).margin(0.1));
	REQUIRE(statistics.CurrentMilliseconds() == Approx(40.0f).margin(0.1));

	// And the worst frame is that one, not an average of everything.
	REQUIRE(statistics.Minimum() == Approx(25.0f).margin(0.1));
}

TEST_CASE("the extremes recover when the frame that set them ages out", "[panels]") {
	FrameStatistics statistics;

	// One catastrophic frame at the start, then a long run of good ones. While
	// it is in the window it is the worst; the moment it leaves, the answer has
	// to change — and a cached extreme that nothing invalidates would report a
	// 2 FPS floor for the rest of the session.
	statistics.Record(0.0, 0.500f);
	for (int index = 1; index < 200; index++) {
		statistics.Record(static_cast<double>(index) * 0.010, 0.010f);
	}

	REQUIRE(statistics.Minimum() == Approx(2.0f).margin(0.1));

	// Push it past the window.
	statistics.Record(FrameStatistics::WINDOW_SECONDS + 1.0, 0.010f);

	REQUIRE(statistics.Minimum() == Approx(100.0f).margin(0.1));
	REQUIRE(statistics.Maximum() == Approx(100.0f).margin(0.1));
	REQUIRE(statistics.Average() == Approx(100.0f).margin(0.1));
}

TEST_CASE("the running totals match a walk of the window", "[panels]") {
	FrameStatistics statistics;

	// Average and Jitter are accumulated as samples arrive and leave rather
	// than summed on demand. Every add has a matching subtract, and an eviction
	// that forgets to remove its pair leaves the mean drifting away from the
	// truth without ever looking obviously wrong.
	std::vector<float> live;
	double now = 0.0;
	for (int index = 0; index < 400; index++) {
		// Deliberately varied, so a dropped or double-counted term shows.
		const float delta = 0.008f + static_cast<float>(index % 7) * 0.001f;
		now += static_cast<double>(delta);
		statistics.Record(now, delta);
		live.push_back(delta);
	}

	// Drop what the window would have dropped.
	double cutoff = now - FrameStatistics::WINDOW_SECONDS;
	double running = 0.0;
	std::vector<float> kept;
	double stamp = 0.0;
	for (const float delta : live) {
		stamp += static_cast<double>(delta);
		if (stamp > cutoff) {
			kept.push_back(delta);
		}
	}
	(void)running;

	REQUIRE(statistics.SampleCount() == kept.size());

	double total = 0.0;
	double change = 0.0;
	for (size_t index = 0; index < kept.size(); index++) {
		total += kept[index];
		if (index > 0) {
			change += std::abs(kept[index] - kept[index - 1]);
		}
	}

	REQUIRE(
		statistics.Average() ==
		Approx(static_cast<float>(static_cast<double>(kept.size()) / total)).margin(0.01)
	);
	REQUIRE(
		statistics.Jitter() ==
		Approx(static_cast<float>(change / static_cast<double>(kept.size() - 1)) * 1000.0f).margin(0.01)
	);
}

TEST_CASE("Summarise agrees with asking one number at a time", "[panels]") {
	FrameStatistics statistics;

	statistics.Record(0.000, 0.010f);
	statistics.Record(0.010, 0.025f);
	statistics.Record(0.035, 0.012f);
	statistics.Record(0.047, 0.008f);

	// The panel reads the summary and the tests read the accessors, so the two
	// have to be the same numbers or the panel is showing something nothing
	// checks.
	const auto summary = statistics.Summarise();
	REQUIRE(summary.Current == Approx(statistics.Current()));
	REQUIRE(summary.CurrentMilliseconds == Approx(statistics.CurrentMilliseconds()));
	REQUIRE(summary.Minimum == Approx(statistics.Minimum()));
	REQUIRE(summary.Average == Approx(statistics.Average()));
	REQUIRE(summary.Maximum == Approx(statistics.Maximum()));
	REQUIRE(summary.Jitter == Approx(statistics.Jitter()));
}

TEST_CASE("clearing empties the window without losing the storage", "[panels]") {
	FrameStatistics statistics;

	for (int index = 0; index < 500; index++) {
		statistics.Record(static_cast<double>(index) * 0.001, 0.010f);
	}
	statistics.Clear();

	REQUIRE_FALSE(statistics.HasSamples());
	REQUIRE(statistics.SampleCount() == 0);
	REQUIRE(statistics.Current() == Approx(0.0f));

	// And it still records afterwards. A Clear that left the head and the count
	// disagreeing would read the wrong slot on the next sample.
	statistics.Record(1.0, 0.020f);
	REQUIRE(statistics.SampleCount() == 1);
	REQUIRE(statistics.Current() == Approx(50.0f).margin(0.1));
}

TEST_CASE("both panels off draws nothing at all", "[panels]") {
	OverlayImage image;
	image.Resize(320, 240);

	DebugPanelData data;
	data.ShowStatistics = false;
	data.ShowFrameGraph = false;

	DrawDebugPanels(image, data);

	// Nothing drawn means the renderer skips the upload and the overlay pass.
	REQUIRE_FALSE(image.IsDirty());
	REQUIRE_FALSE(AnyPixelSet(image));
}

TEST_CASE("the statistics panel draws even with no samples yet", "[panels]") {
	OverlayImage image;
	image.Resize(320, 240);

	DebugPanelData data;
	data.ShowStatistics = true;

	DrawDebugPanels(image, data);

	REQUIRE(image.IsDirty());
}

TEST_CASE("every profiler tab draws without a frame to show", "[panels]") {
	// An empty frame graph is the state right after F5 is pressed. Every tab
	// has to survive it, because that is the first thing a user sees.
	for (uint8_t index = 0; index < static_cast<uint8_t>(ProfilerTab::Count); index++) {
		OverlayImage image;
		image.Resize(640, 480);

		DebugPanelData data;
		data.ShowFrameGraph = true;
		data.Tab = static_cast<ProfilerTab>(index);

		DrawDebugPanels(image, data);
		REQUIRE(image.IsDirty());
	}
}

TEST_CASE("the flamegraph draws a frame's spans", "[panels]") {
	// Designated rather than positional. These used to be positional and every
	// one of them shifted a value into the wrong field the day FrameSpan grew
	// a Parent — silently, because the types line up.
	const std::vector<FrameSpan> spans{
		{.Name = "outer",
		 .Depth = 0,
		 .Parent = engine::core::FrameGraph::NO_PARENT,
		 .StartMilliseconds = 0.0f,
		 .Milliseconds = 8.0f,
		 .SelfMilliseconds = 2.0f,
		 .Category = ProfileCategory::Engine},
		{.Name = "inner",
		 .Depth = 1,
		 .Parent = 0,
		 .StartMilliseconds = 1.0f,
		 .Milliseconds = 6.0f,
		 .SelfMilliseconds = 6.0f,
		 .Category = ProfileCategory::Render},
	};

	OverlayImage image;
	image.Resize(640, 480);

	DebugPanelData data;
	data.ShowFrameGraph = true;
	data.Tab = ProfilerTab::Frame;
	data.Spans = spans;
	data.FrameMilliseconds = 8.0f;

	DrawDebugPanels(image, data);
	REQUIRE(image.IsDirty());
}

TEST_CASE("a deeper frame makes a taller panel", "[panels]") {
	auto heightOf = [](const std::vector<FrameSpan> &spans) {
		OverlayImage image;
		image.Resize(640, 480);

		DebugPanelData data;
		data.ShowFrameGraph = true;
		data.Tab = ProfilerTab::Frame;
		data.Spans = spans;
		data.FrameMilliseconds = 8.0f;
		DrawDebugPanels(image, data);

		// The panel is top-anchored, so its height is the last row with
		// anything in it. This used to scan for the *first* such row and
		// subtract, which was right only while the panel grew upwards from the
		// bottom — and which passes trivially once it does not, because the
		// first non-empty row is 0 for a top-anchored panel of any size.
		for (int y = image.GetHeight() - 1; y >= 0; y--) {
			for (int x = 0; x < image.GetWidth(); x++) {
				const size_t index = (static_cast<size_t>(y) * 640 + static_cast<size_t>(x)) * 4 + 3;
				if (image.GetPixels()[index] != 0) {
					return y + 1;
				}
			}
		}
		return 0;
	};

	const std::vector<FrameSpan> shallow{
		{.Name = "a",
		 .Depth = 0,
		 .Parent = engine::core::FrameGraph::NO_PARENT,
		 .StartMilliseconds = 0.0f,
		 .Milliseconds = 8.0f,
		 .SelfMilliseconds = 8.0f,
		 .Category = ProfileCategory::Engine},
	};
	std::vector<FrameSpan> deep = shallow;
	for (uint32_t depth = 1; depth <= 6; depth++) {
		deep.push_back(
			{.Name = "b",
			 .Depth = depth,
			 .Parent = depth - 1,
			 .StartMilliseconds = 0.0f,
			 .Milliseconds = 8.0f,
			 .SelfMilliseconds = 0.0f,
			 .Category = ProfileCategory::Engine}
		);
	}

	// A fixed half-screen panel spends most of its life as a large dark
	// rectangle over the game, which makes it a tool you close.
	REQUIRE(heightOf(deep) > heightOf(shallow));
}

TEST_CASE("the panel keeps room for every span alongside the unmarked row", "[panels]") {
	// The unmarked row and the column header are pinned above the scrollable
	// body. Neither scrolls, so leaving them out of the height budget does not
	// move the panel — it silently clips the last span off the bottom of it,
	// which is the same class of bug as not reporting unmarked time at all.
	auto heightOf = [](size_t count) {
		std::vector<FrameSpan> spans;
		for (size_t index = 0; index < count; index++) {
			spans.push_back(
				{.Name = "span",
				 .Depth = 0,
				 .Parent = engine::core::FrameGraph::NO_PARENT,
				 .StartMilliseconds = 0.0f,
				 .Milliseconds = 1.0f,
				 .SelfMilliseconds = 1.0f,
				 .Category = ProfileCategory::Engine}
			);
		}

		OverlayImage image;
		image.Resize(640, 480);

		DebugPanelData data;
		data.ShowFrameGraph = true;
		data.Tab = ProfilerTab::Frame;
		data.Spans = spans;
		data.FrameMilliseconds = 8.0f;
		data.UnmarkedMilliseconds = 8.0f - static_cast<float>(count);
		DrawDebugPanels(image, data);

		for (int y = image.GetHeight() - 1; y >= 0; y--) {
			for (int x = 0; x < image.GetWidth(); x++) {
				const size_t index = (static_cast<size_t>(y) * 640 + static_cast<size_t>(x)) * 4 + 3;
				if (image.GetPixels()[index] != 0) {
					return y + 1;
				}
			}
		}
		return 0;
	};

	// One more span is one more row, every time. A step that stops growing is
	// the panel running out of budget and dropping a row on the floor.
	const int step = heightOf(2) - heightOf(1);
	REQUIRE(step > 0);
	for (size_t count = 2; count < 8; count++) {
		REQUIRE(heightOf(count + 1) - heightOf(count) == step);
	}
}

TEST_CASE("the unmarked row is drawn even when there is nothing unmarked", "[panels]") {
	const std::vector<FrameSpan> spans{
		{.Name = "all of it",
		 .Depth = 0,
		 .Parent = engine::core::FrameGraph::NO_PARENT,
		 .StartMilliseconds = 0.0f,
		 .Milliseconds = 8.0f,
		 .SelfMilliseconds = 8.0f,
		 .Category = ProfileCategory::Engine},
	};

	auto heightOf = [&spans](float unmarked) {
		OverlayImage image;
		image.Resize(640, 480);

		DebugPanelData data;
		data.ShowFrameGraph = true;
		data.Tab = ProfilerTab::Frame;
		data.Spans = spans;
		data.FrameMilliseconds = 8.0f;
		data.UnmarkedMilliseconds = unmarked;
		DrawDebugPanels(image, data);

		for (int y = image.GetHeight() - 1; y >= 0; y--) {
			for (int x = 0; x < image.GetWidth(); x++) {
				const size_t index = (static_cast<size_t>(y) * 640 + static_cast<size_t>(x)) * 4 + 3;
				if (image.GetPixels()[index] != 0) {
					return y + 1;
				}
			}
		}
		return 0;
	};

	// A row that appears only when there is something to report makes its
	// absence ambiguous: the reader cannot tell "nothing unmarked" from "this
	// build does not measure that". Zero is a reading, and it is a good one.
	REQUIRE(heightOf(0.0f) == heightOf(4.0f));
}

TEST_CASE("the categories tab has room for the unmarked bar", "[panels]") {
	OverlayImage image;
	image.Resize(640, 480);

	DebugPanelData data;
	data.ShowFrameGraph = true;
	data.Tab = ProfilerTab::Categories;
	data.FrameMilliseconds = 8.0f;
	data.UnmarkedMilliseconds = 6.0f;

	// Category bars are self time, so without this one they sum to less than
	// the frame and nothing on the panel says why.
	DrawDebugPanels(image, data);
	REQUIRE(image.IsDirty());
}

TEST_CASE("unmarked time larger than the frame does not run off the panel", "[panels]") {
	OverlayImage image;
	image.Resize(640, 480);

	DebugPanelData data;
	data.ShowFrameGraph = true;
	data.Tab = ProfilerTab::Categories;
	data.FrameMilliseconds = 1.0f;
	// Not reachable through FrameGraph, which clamps. Reachable by anyone else
	// filling this struct, and a bar width is a multiply by a share.
	data.UnmarkedMilliseconds = 100.0f;

	DrawDebugPanels(image, data);
	REQUIRE(image.IsDirty());
}

TEST_CASE("a zero-length frame does not divide by it", "[panels]") {
	const std::vector<FrameSpan> spans{
		{.Name = "outer",
		 .Depth = 0,
		 .Parent = engine::core::FrameGraph::NO_PARENT,
		 .StartMilliseconds = 0.0f,
		 .Milliseconds = 0.0f,
		 .SelfMilliseconds = 0.0f,
		 .Category = ProfileCategory::Engine},
	};

	OverlayImage image;
	image.Resize(640, 480);

	DebugPanelData data;
	data.ShowFrameGraph = true;
	data.Spans = spans;
	data.FrameMilliseconds = 0.0f;

	DrawDebugPanels(image, data);
	REQUIRE(image.IsDirty());
}

TEST_CASE("drawing into an empty image is safe", "[panels]") {
	OverlayImage image;

	DebugPanelData data;
	data.ShowStatistics = true;
	data.ShowFrameGraph = true;

	// A minimised window has no pixels, and the panels still get asked to draw.
	DrawDebugPanels(image, data);
	REQUIRE_FALSE(image.IsDirty());
}

TEST_CASE("every tab has a name", "[panels]") {
	for (uint8_t index = 0; index < static_cast<uint8_t>(ProfilerTab::Count); index++) {
		REQUIRE(engine::render::GetProfilerTabName(static_cast<ProfilerTab>(index)) != "?");
	}
}
