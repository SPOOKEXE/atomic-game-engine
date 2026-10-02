#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.frame_time")
using namespace engine::imagegraph;

TEST_CASE("FrameTime splits canonical signed author clocks atomically", "[imagegraph][frame_time]") {
	for (double real : {-10.75, -2.5, -.25, 0., .25, 2.5, 10.75}) {
		FrameTime time;
		REQUIRE(SplitFrameTime(real, time));
		CHECK(FrameTimeToReal(time) == real);
		FrameTime inverse;
		REQUIRE(SplitFrameTime(static_cast<double>(FrameTimeToReal(time)), inverse));
		CHECK(inverse == time);
	}
	FrameTime time{3, .25, false}, sentinel = time;
	CHECK_FALSE(SplitFrameTime(std::numeric_limits<double>::infinity(), time));
	CHECK(time == sentinel);
	CHECK_FALSE(SplitFrameTime(Limits::MaximumTick + .5, time));
	CHECK(time == sentinel);
	CHECK_FALSE(SplitFrameTime(5.5, time, true, 5));
	CHECK(time == sentinel);
	CHECK_FALSE(SplitFrameTime(std::ldexp(1., 64), time, false, std::numeric_limits<uint64_t>::max()));
	CHECK(time == sentinel);
	REQUIRE(SplitFrameTime(-0., time));
	CHECK(time == FrameTime{});
	CHECK_FALSE(ValidFrameTime({0, 0, true}));
	CHECK_FALSE(ValidFrameTime({0, 1, false}));
	CHECK_FALSE(ValidFrameTime({0, std::numeric_limits<double>::quiet_NaN(), false}));
	for (auto [input, expected] :
		 {std::pair{-2.5, -2.}, std::pair{-3.5, -4.}, std::pair{2.5, 2.}, std::pair{3.5, 4.}}) {
		REQUIRE(SplitFrameTime(input, time, true));
		CHECK(FrameTimeToReal(time) == expected);
	}
}

TEST_CASE("FrameTime exact ordering retains nearby fractions at uint64 scale", "[imagegraph][frame_time]") {
	const uint64_t high = std::numeric_limits<uint64_t>::max() - 1;
	FrameTime a{high, .25, false}, b{high, .75, false}, c{high + 1, 0, false};
	REQUIRE(ValidFrameTime(a, std::numeric_limits<uint64_t>::max()));
	CHECK(CompareFrameTime(a, b) == -1);
	CHECK(CompareFrameTime(b, c) == -1);
	CHECK(FrameTimeDelta(b, a) == .5L);
	CHECK(FrameTimeDelta(c, b) == .25L);
	a.NegativeFrame = b.NegativeFrame = c.NegativeFrame = true;
	CHECK(CompareFrameTime(a, b) == 1);
	CHECK(CompareFrameTime(b, c) == 1);
	CHECK(FrameTimeDelta(b, a) == -.5L);
	CHECK(FrameTimeDelta(c, b) == -.25L);
	CHECK(FrameTimeDelta({2, .25, false}, {3, .75, true}) == 6.L);
	CHECK(FrameTimeDelta({3, .75, true}, {2, .25, false}) == -6.L);
	Keyframe key;
	EvaluationRequest request;
	REQUIRE(SetFrameTime(key, {1, .5, true}));
	REQUIRE(SetFrameTime(request, GetFrameTime(key)));
	CHECK(GetFrameTime(request) == GetFrameTime(key));
	CHECK_FALSE(SetFrameTime(key, {0, 0, true}));
	CHECK((GetFrameTime(key) == FrameTime{1, .5, true}));
}

TEST_CASE("FrameTime shifts preserve exact components and fail atomically", "[imagegraph][frame_time]") {
	FrameTime result{7, .25, false};
	const double tiny = std::numeric_limits<double>::denorm_min();
	REQUIRE(ShiftFrameTime({10'000'000, tiny, false}, {10'000'000, 0, false}, {}, result));
	CHECK(result == FrameTime{0, tiny, false});
	REQUIRE(ShiftFrameTime({1, 0, false}, {0, tiny, false}, {}, result));
	CHECK(result == FrameTime{1, 0, false});
	REQUIRE(ShiftFrameTime({0, tiny, false}, {1, 0, false}, {1, 0, false}, result));
	CHECK(result == FrameTime{0, tiny, false});
	REQUIRE(ShiftFrameTime({1, 0, false}, {1, 0, false}, {0, tiny, false}, result));
	CHECK(result == FrameTime{0, tiny, false});
	REQUIRE(ShiftFrameTime({0, tiny, true}, {1, 0, true}, {1, 0, true}, result, false));
	CHECK(result == FrameTime{0, tiny, true});

	const uint64_t maximum = std::numeric_limits<uint64_t>::max();
	REQUIRE(ShiftFrameTime({maximum - 2, tiny, false}, {maximum - 2, 0, false}, {1, 0, false}, result));
	CHECK(result == FrameTime{1, tiny, false});
	REQUIRE(ShiftFrameTime({0, tiny, false}, {maximum - 1, 0, false}, {maximum - 1, 0, false}, result));
	CHECK(result == FrameTime{0, tiny, false});
	REQUIRE(ShiftFrameTime(
		{Limits::MaximumTick, 0, false},
		{Limits::MaximumTick, 0, true},
		{Limits::MaximumTick, 0, true},
		result
	));
	CHECK(result == FrameTime{Limits::MaximumTick, 0, false});
	REQUIRE(ShiftFrameTime({maximum, 0, true}, {maximum, 0, false}, {}, result));
	CHECK(result == FrameTime{});

	REQUIRE(ShiftFrameTime({}, {2, .5, false}, {}, result));
	CHECK(result == FrameTime{});
	REQUIRE(ShiftFrameTime({}, {2, .5, false}, {}, result, false));
	CHECK(result == FrameTime{2, .5, true});

	const FrameTime sentinel = result;
	CHECK_FALSE(ShiftFrameTime({Limits::MaximumTick, 0, false}, {}, {1, 0, false}, result));
	CHECK(result == sentinel);
	CHECK_FALSE(ShiftFrameTime({maximum, 0, false}, {}, {maximum, 0, false}, result));
	CHECK(result == sentinel);
	CHECK_FALSE(ShiftFrameTime({0, 0, true}, {}, {}, result));
	CHECK(result == sentinel);
}

TEST_CASE("FrameTime scales around a fixed anchor with bounded signed results", "[imagegraph][frame_time]") {
	FrameTime result{7, .25, false};
	REQUIRE(ScaleFrameTime({5, 0, false}, {}, {10, 0, false}, {20, 0, false}, result));
	CHECK(result == FrameTime{10, 0, false});
	REQUIRE(ScaleFrameTime({2, .25, false}, {2, .25, false}, {4, 0, false}, {6, .5, false}, result));
	CHECK(result == FrameTime{2, .25, false});
	REQUIRE(ScaleFrameTime({4, 0, false}, {2, .25, false}, {4, 0, false}, {6, .5, false}, result));
	CHECK(result == FrameTime{6, .5, false});
	REQUIRE(ScaleFrameTime({2, 0, true}, {2, 0, true}, {4, 0, false}, {}, result));
	CHECK(result == FrameTime{});
	REQUIRE(ScaleFrameTime({5, 0, false}, {2, 0, false}, {4, 0, false}, {}, result));
	CHECK(result == FrameTime{});
	REQUIRE(ScaleFrameTime({5, 0, false}, {2, 0, false}, {4, 0, false}, {}, result, false));
	CHECK(result == FrameTime{1, 0, true});

	const double tiny = std::numeric_limits<double>::denorm_min();
	const uint64_t nearMaximum = Limits::MaximumTick - 1;
	for (bool negative : {false, true}) {
		REQUIRE(ScaleFrameTime(
			{nearMaximum, tiny, negative},
			{nearMaximum, 0, negative},
			{Limits::MaximumTick, 0, negative},
			{Limits::MaximumTick + 1, 0, negative},
			result,
			false
		));
		CHECK(result == FrameTime{nearMaximum, tiny * 2, negative});
		const FrameTime preserved = result;
		CHECK_FALSE(ScaleFrameTime(
			{Limits::MaximumTick, tiny, negative},
			{Limits::MaximumTick, 0, negative},
			{Limits::MaximumTick + 1, 0, negative},
			{Limits::MaximumTick + 2, 0, negative},
			result,
			false
		));
		CHECK(result == preserved);
	}

	const FrameTime sentinel = result;
	CHECK_FALSE(ScaleFrameTime({1, 0, false}, {2, 0, false}, {2, 0, false}, {3, 0, false}, result));
	CHECK(result == sentinel);
	CHECK_FALSE(ScaleFrameTime(
		{Limits::MaximumTick, 0, false}, {}, {1, 0, false}, {Limits::MaximumTick, 0, false}, result
	));
	CHECK(result == sentinel);
	const uint64_t maximum = std::numeric_limits<uint64_t>::max();
	CHECK_FALSE(ScaleFrameTime({maximum, 0, false}, {maximum - 1, 0, false}, {}, {}, result));
	CHECK(result == sentinel);
	// This affine result is bounded, but its scaled offset exceeds uint64 before cancellation.
	CHECK_FALSE(ScaleFrameTime(
		{maximum - 1, 0, true}, {maximum, 0, false}, {maximum, 0, true}, {5'000'000, 0, true}, result, false
	));
	CHECK(result == sentinel);
	CHECK_FALSE(ScaleFrameTime({0, 0, true}, {}, {1, 0, false}, {2, 0, false}, result));
	CHECK(result == sentinel);
}
