#include "../src/TimelineSchedule.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.timeline_schedule")

using engine::imagegraph::detail::AdvancePlayback;
using engine::imagegraph::detail::AdvancePlaybackTime;
using engine::imagegraph::detail::BeginPlayback;
using engine::imagegraph::detail::CurveSide;
using engine::imagegraph::detail::EaseKeys;
using engine::imagegraph::detail::FrameWindow;
using engine::imagegraph::detail::KeyBlend;
using engine::imagegraph::detail::KeyChoice;
using engine::imagegraph::detail::KeyEase;
using engine::imagegraph::detail::KeyEnd;
using engine::imagegraph::detail::KeySelection;
using engine::imagegraph::detail::PlaybackClock;
using engine::imagegraph::detail::PlaybackCursor;
using engine::imagegraph::detail::PlaybackEnd;
using engine::imagegraph::detail::PlaybackStep;
using engine::imagegraph::detail::SeekFrame;
using engine::imagegraph::detail::SelectKeys;
using engine::imagegraph::detail::StopPlayback;

TEST_CASE("timeline playback advances, stops, loops and reverses at source boundaries", "[imagegraph]") {
	const FrameWindow window{6, 2, 5};
	PlaybackStep step;
	REQUIRE(BeginPlayback(window, false, step));
	CHECK(step.Cursor.Frame == 2);
	CHECK(step.Cursor.Playing);
	CHECK(step.Restart);
	REQUIRE(BeginPlayback(window, true, step));
	CHECK(step.Cursor.Rendering);
	CHECK_FALSE(step.Restart);
	REQUIRE(StopPlayback(window, step));
	CHECK(step.Cursor.Frame == 2);
	CHECK_FALSE(step.Cursor.Playing);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::Stop, {5, 1, true}, step));
	CHECK(step.Cursor.Frame == 5);
	CHECK_FALSE(step.Cursor.Playing);
	CHECK_FALSE(step.Restart);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::Loop, {5, 1, true}, step));
	CHECK(step.Cursor.Frame == 2);
	CHECK(step.Restart);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::Loop, {5, 1, true, true}, step));
	CHECK(step.Cursor.Frame == 5);
	CHECK_FALSE(step.Cursor.Playing);
	CHECK_FALSE(step.Cursor.Rendering);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::PingPong, {5, 1, true}, step));
	CHECK(step.Cursor.Frame == 4);
	CHECK(step.Cursor.Direction == -1);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::PingPong, {1, -1, true}, step));
	CHECK(step.Cursor.Frame == 0);
	CHECK(step.Cursor.Direction == 1);
	REQUIRE(AdvancePlayback(window, PlaybackEnd::Loop, {2, 1, false}, step));
	CHECK(step.Cursor.Frame == 2);
	CHECK_FALSE(step.Restart);
	CHECK_FALSE(AdvancePlayback({0, 0, 0}, PlaybackEnd::Loop, {0, 1, true}, step));
	CHECK_FALSE(AdvancePlayback(window, PlaybackEnd::Loop, {5, 0, true}, step));
}

TEST_CASE(
	"timeline seek quantizes finite positive frames and rejects unsupported fractional output", "[imagegraph]"
) {
	uint64_t frame = 99;
	REQUIRE(SeekFrame(2.49, true, frame));
	CHECK(frame == 2);
	REQUIRE(SeekFrame(2.5, true, frame));
	CHECK(frame == 2);
	REQUIRE(SeekFrame(3.5, true, frame));
	CHECK(frame == 4);
	REQUIRE(SeekFrame(3.0, false, frame));
	CHECK(frame == 3);
	CHECK_FALSE(SeekFrame(3.25, false, frame));
	CHECK_FALSE(SeekFrame(-1, true, frame));
	CHECK_FALSE(SeekFrame(std::numeric_limits<double>::infinity(), true, frame));
}

TEST_CASE("timeline clock carries excess time while advancing only one frame per call", "[imagegraph]") {
	PlaybackStep step;
	PlaybackClock clock;
	REQUIRE(AdvancePlaybackTime({8, 0, 7}, PlaybackEnd::Loop, {0, 1, true}, {}, 0.35, 10, 1, step, clock));
	CHECK(step.Cursor.Frame == 1);
	CHECK(std::abs(clock.PendingSeconds - 0.25) < 1e-12);
	PlaybackClock next;
	REQUIRE(AdvancePlaybackTime({8, 0, 7}, PlaybackEnd::Loop, step.Cursor, clock, 0, 10, 1, step, next));
	CHECK(step.Cursor.Frame == 2);
	CHECK(std::abs(next.PendingSeconds - 0.15) < 1e-12);
	REQUIRE(AdvancePlaybackTime({8, 0, 7}, PlaybackEnd::Stop, {7, 1, true}, clock, 0, 10, 1, step, next));
	CHECK_FALSE(step.Cursor.Playing);
	CHECK(next.PendingSeconds == 0);
	CHECK_FALSE(AdvancePlaybackTime({8, 0, 7}, PlaybackEnd::Loop, {0, 1, true}, {}, -1, 10, 1, step, next));
}

TEST_CASE("keyframe schedule resolves hold loop ping and wrap with source indexing", "[imagegraph]") {
	const std::array<uint64_t, 3> ticks{2, 4, 5};
	KeySelection selected;
	REQUIRE(SelectKeys(ticks, 0, 8, KeyEnd::Hold, 0, selected));
	CHECK(selected.From == 0);
	CHECK(selected.To == 0);
	REQUIRE(SelectKeys(ticks, 3, 8, KeyEnd::Hold, 0, selected));
	CHECK(selected.From == 0);
	CHECK(selected.To == 1);
	CHECK(selected.Numerator == 1);
	CHECK(selected.Denominator == 2);
	REQUIRE(SelectKeys(ticks, 6, 8, KeyEnd::Loop, 0, selected));
	CHECK(selected.From == 0);
	CHECK(selected.To == 1);
	CHECK(selected.Numerator == 1);
	REQUIRE(SelectKeys(ticks, 6, 8, KeyEnd::Ping, 0, selected));
	CHECK(selected.From == 1);
	CHECK(selected.To == 2);
	CHECK(selected.Numerator == 0);
	REQUIRE(SelectKeys(ticks, 0, 8, KeyEnd::Wrap, 0, selected));
	CHECK(selected.From == 2);
	CHECK(selected.To == 0);
	CHECK(selected.Numerator == 3);
	CHECK(selected.Denominator == 5);
	REQUIRE(SelectKeys(ticks, 6, 8, KeyEnd::Wrap, 0, selected));
	CHECK(selected.From == 2);
	CHECK(selected.To == 0);
	CHECK(selected.Numerator == 1);
	CHECK(selected.Denominator == 5);
	const std::array<uint64_t, 2> zeroFirst{0, 4};
	REQUIRE(SelectKeys(zeroFirst, 0, 6, KeyEnd::Wrap, 0, selected));
	CHECK(selected.From == 1);
	CHECK(selected.To == 0);
	CHECK(selected.Numerator == 2);
	CHECK(selected.Denominator == 2);
	REQUIRE(SelectKeys(ticks, 6, 8, KeyEnd::Loop, 1, selected));
	CHECK(selected.From == 2);
	CHECK(selected.To == 2);
}

TEST_CASE("keyframe schedule rejects unbounded or ambiguous authored positions", "[imagegraph]") {
	KeySelection selected;
	const std::array<uint64_t, 2> duplicate{2, 2};
	CHECK_FALSE(SelectKeys(duplicate, 2, 8, KeyEnd::Hold, 0, selected));
	const std::array<uint64_t, 2> keys{2, 5};
	CHECK_FALSE(SelectKeys(keys, 0, 5, KeyEnd::Wrap, 0, selected));
	CHECK_FALSE(SelectKeys(keys, 0, 8, KeyEnd::Hold, 2, selected));
	CHECK_FALSE(SelectKeys(keys, 0, 0, KeyEnd::Hold, 0, selected));
	CHECK_FALSE(SelectKeys(keys, 8, 8, KeyEnd::Wrap, 0, selected));
}

TEST_CASE("keyframe easing honors incoming cut, outgoing cut and source Bezier handles", "[imagegraph]") {
	KeyBlend blend;
	KeyEase ease;
	REQUIRE(EaseKeys(ease, 0.5, blend));
	CHECK(blend.Choice == KeyChoice::Blend);
	CHECK(blend.Ratio == 0.5);
	ease.InType = CurveSide::Cut;
	ease.OutType = CurveSide::Cut;
	REQUIRE(EaseKeys(ease, 0.5, blend));
	CHECK(blend.Choice == KeyChoice::From);
	ease.InType = CurveSide::Bezier;
	REQUIRE(EaseKeys(ease, 0.0, blend));
	CHECK(blend.Choice == KeyChoice::To);
	ease.OutType = CurveSide::Bezier;
	ease.OutX = 1.0 / 3;
	ease.InX = 1.0 / 3;
	ease.OutY = 0;
	ease.InY = 0;
	REQUIRE(EaseKeys(ease, 0.5, blend));
	CHECK(blend.Choice == KeyChoice::Blend);
	CHECK(blend.Ratio == 0.125);
	CHECK_FALSE(EaseKeys(ease, std::numeric_limits<double>::infinity(), blend));
	ease.OutX = std::numeric_limits<double>::infinity();
	CHECK_FALSE(EaseKeys(ease, 0.5, blend));
}
