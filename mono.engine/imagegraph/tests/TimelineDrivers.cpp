#include "TimelineDrivers.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.timeline_drivers")

using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE(
	"source linear and snap drivers use absolute time and even ties", "[imagegraph][timeline_drivers]"
) {
	double result = -99;
	REQUIRE(ApplyLinearDriver(2, 12, 1, result));
	CHECK(result == 14);
	REQUIRE(ApplyLinearDriver(2, .5, -2, result));
	CHECK(result == 1);
	for (const auto &[value, expected] : std::array<std::pair<double, double>, 6>{
			 {{.5, 0}, {1.5, 2}, {2.5, 2}, {-.5, 0}, {-1.5, -2}, {-2.5, -2}}
		 }) {
		REQUIRE(ApplySnapDriver(value, 1, result));
		CHECK(result == expected);
	}
	REQUIRE(ApplySnapDriver(.7, 0, result));
	CHECK(result == .7);
	REQUIRE(ApplySnapDriver(3, -2, result));
	CHECK(result == 4);
	result = -99;
	CHECK_FALSE(ApplyLinearDriver(1, 1e308, 1e308, result));
	CHECK_FALSE(ApplySnapDriver(1e308, 1e-308, result));
	CHECK(result == -99);
}

TEST_CASE(
	"source bounce and elastic drivers normalize spacing and preserve interval endpoints",
	"[imagegraph][timeline_drivers]"
) {
	double ratio;
	for (bool elastic : {false, true}) {
		REQUIRE(DriverBounceRatio(.6, .6, 3, .5, 2, elastic, ratio));
		CHECK(std::abs(ratio - (elastic ? 1.4 : .6)) < 1e-12);
		REQUIRE(DriverBounceRatio(.2, .2, 3, .5, 2, elastic, ratio));
		CHECK(std::abs(ratio - .4) < 1e-12);
		for (double boundary : {.4, 1.0}) {
			REQUIRE(DriverBounceRatio(boundary, boundary, 3, .5, 2, elastic, ratio));
			CHECK(ratio == 1);
		}
		REQUIRE(DriverBounceRatio(.25, .25, 0, .5, 2, elastic, ratio));
		CHECK(ratio == .25);
	}
	ratio = -99;
	CHECK_FALSE(DriverBounceRatio(.5, .5, 1025, .5, 2, false, ratio));
	CHECK_FALSE(DriverBounceRatio(.5, .5, 3, std::numeric_limits<double>::infinity(), 2, false, ratio));
	CHECK(ratio == -99);
}

TEST_CASE(
	"source curve drivers interpolate a thirty two interval lookup map", "[imagegraph][timeline_drivers]"
) {
	// These handles give x(t)=t and y(t)=t*t, so expected table samples need no curve evaluator.
	const Curve quadratic{{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0, 1.0 / 3, 0}, {-1.0 / 3, -2.0 / 3, 1, 1, 0, 0}}};
	DriverCurveMap map;
	REQUIRE(BuildDriverCurveMap(quadratic, map));
	for (size_t index = 0; index < map.size(); index++) {
		const double x = static_cast<double>(index) / 32;
		CHECK(std::abs(map[index] - x * x) < 1e-12);
	}
	const double sample = 10.5 / 32;
	const double expected = ((10.0 / 32) * (10.0 / 32) + (11.0 / 32) * (11.0 / 32)) / 2;
	CHECK(std::abs(SampleDriverCurveMap(map, sample) - expected) < 1e-12);
	CHECK(std::abs(SampleDriverCurveMap(map, sample) - sample * sample) > 1e-5);
	CHECK(SampleDriverCurveMap(map, -1) == 0);
	CHECK(SampleDriverCurveMap(map, 2) == 1);
	CHECK(SampleDriverCurveMap(map, std::numeric_limits<double>::quiet_NaN()) == 0);
	Curve invalid = quadratic;
	invalid.Header[1] = 0;
	const auto retained = map;
	CHECK_FALSE(BuildDriverCurveMap(invalid, map));
	CHECK(map == retained);
}

TEST_CASE(
	"source drivers round integer components after modulation and truncate numeric arrays",
	"[imagegraph][timeline_drivers]"
) {
	const KeyframeSourceDriver linear = KeyframeLinearDriver{.25};
	Value value = std::string("unchanged");
	REQUIRE(ApplySourceDriver(&linear, int64_t{2}, int64_t{7}, .25, .25, 1, true, -1, value) == Status::Ok);
	CHECK(std::get<int64_t>(value) == 4); // 3.25 + .25 = 3.5, then even rounding.
	REQUIRE(
		ApplySourceDriver(&linear, Vector2{2, 4}, Vector2{6, 8}, .5, .5, 2, true, -1, value) == Status::Ok
	);
	CHECK((std::get<Vector2>(value) == Vector2{4.5, 6.5}));
	REQUIRE(
		ApplySourceDriver(&linear, Vector3{2, 4, 6}, Vector3{6, 8, 10}, .5, .5, 2, true, -1, value) ==
		Status::Ok
	);
	CHECK((std::get<Vector3>(value) == Vector3{4.5, 6.5, 8.5}));
	REQUIRE(
		ApplySourceDriver(&linear, Vector4{2, 4, 6, 8}, Vector4{6, 8, 10, 12}, .5, .5, 2, true, -1, value) ==
		Status::Ok
	);
	CHECK((std::get<Vector4>(value) == Vector4{4.5, 6.5, 8.5, 10.5}));
	ArrayValue first{ValueType::Scalar, {1.0, 3.0, 99.0}}, last{ValueType::Scalar, {5.0, 7.0}};
	REQUIRE(ApplySourceDriver(&linear, first, last, .5, .5, 2, true, -1, value) == Status::Ok);
	CHECK((std::get<ArrayValue>(value) == ArrayValue{ValueType::Scalar, {3.5, 5.5}}));
	first = {ValueType::Integer, {int64_t{2}, int64_t{-3}, int64_t{99}}};
	last = {ValueType::Integer, {int64_t{7}, int64_t{2}}};
	REQUIRE(ApplySourceDriver(&linear, first, last, .25, .25, 1, true, -1, value) == Status::Ok);
	CHECK((std::get<ArrayValue>(value) == ArrayValue{ValueType::Integer, {int64_t{4}, int64_t{-2}}}));
	first = {ValueType::Scalar, {}};
	last = {ValueType::Scalar, {1.0}};
	REQUIRE(ApplySourceDriver(&linear, first, last, .5, .5, 2, true, -1, value) == Status::Ok);
	CHECK(std::get<double>(value) == .5);
	const KeyframeSourceDriver bounce = KeyframeBounceDriver{};
	CHECK(
		ApplySourceDriver(&bounce, first, last, .5, .5, 2, true, -1, value) == Status::UnsupportedExecution
	);
	first.Nested = {{1.0}};
	CHECK(
		ApplySourceDriver(&linear, first, last, .5, .5, 2, true, -1, value) == Status::UnsupportedExecution
	);
	CHECK(
		ApplySourceDriver(&linear, std::string("x"), std::string("y"), .5, .5, 2, true, -1, value) ==
		Status::UnsupportedExecution
	);
	CHECK(
		ApplySourceDriver(&linear, int64_t{INT64_MAX}, int64_t{INT64_MAX}, 0, 0, 1, false, -1, value) ==
		Status::InvalidValue
	);
}

TEST_CASE(
	"source Area choices and packed colour drivers keep their actual source shapes",
	"[imagegraph][timeline_drivers]"
) {
	const KeyframeSourceDriver snap = KeyframeSnapDriver{1};
	Value value;
	REQUIRE(
		ApplySourceDriver(
			&snap, Area{1.2, 2.7, 3.4, 4.6, 0, 1}, Area{1.2, 2.7, 3.4, 4.6, 1, 2}, .5, .5, 1, true, -1, value
		) == Status::Ok
	);
	CHECK((std::get<Area>(value) == Area{1, 3, 3, 5, 0, 2}));
	const KeyframeSourceDriver linear = KeyframeLinearDriver{.1};
	CHECK(
		ApplySourceDriver(&linear, Area{}, Area{}, 0, 0, 1, false, -1, value) == Status::UnsupportedExecution
	);
	const KeyframeSourceDriver colour = KeyframeLinearDriver{1};
	REQUIRE(
		ApplySourceDriver(&colour, Colour{2, 4, 6, 8}, Colour{3, 5, 7, 9}, .5, .5, 1, true, -1, value) ==
		Status::Ok
	);
	// Half-even channel merging produces (2,4,6,8), then packed +1 changes only red.
	CHECK((std::get<Colour>(value) == Colour{3, 4, 6, 8}));
	CHECK(
		ApplySourceDriver(&linear, Colour{}, Colour{}, 0, 0, 1, false, -1, value) ==
		Status::UnsupportedExecution
	);
	CHECK(
		ApplySourceDriver(
			&colour, Colour{255, 255, 255, 255}, Colour{255, 255, 255, 255}, 0, 0, 1, false, -1, value
		) == Status::UnsupportedExecution
	);
}

TEST_CASE(
	"source quaternion drivers distinguish raw Slerp and Euler degree conversion",
	"[imagegraph][timeline_drivers]"
) {
	Value value;
	REQUIRE(
		ApplySourceDriver(
			nullptr, Quaternion{0, 0, 0, 2}, Quaternion{0, 0, 0, 2}, .5, .5, 1, true, 0, value
		) == Status::Ok
	);
	CHECK((std::get<Quaternion>(value) == Quaternion{0, 0, 0, 2.5}));
	CHECK(
		ApplySourceDriver(nullptr, Quaternion{}, Quaternion{}, .5, .5, 1, true, -1, value) ==
		Status::UnsupportedExecution
	);
	const double half = std::sqrt(.5);
	for (int axis = 0; axis < 3; axis++) {
		Quaternion degrees{0, 0, 0, 0};
		if (axis == 0)
			degrees.X = 90;
		else if (axis == 1)
			degrees.Y = 90;
		else
			degrees.Z = 90;
		REQUIRE(ApplySourceDriver(nullptr, degrees, degrees, 0, 0, 1, false, 1, value) == Status::Ok);
		const auto q = std::get<Quaternion>(value);
		CHECK(std::abs(q.W - half) < 1e-12);
		CHECK(std::abs(q.X - (axis == 0 ? -half : 0)) < 1e-12);
		CHECK(std::abs(q.Y - (axis == 1 ? -half : 0)) < 1e-12);
		CHECK(std::abs(q.Z - (axis == 2 ? -half : 0)) < 1e-12);
	}
	const KeyframeSourceDriver linear = KeyframeLinearDriver{45};
	REQUIRE(
		ApplySourceDriver(
			&linear, Quaternion{45, 45, 45, 9}, Quaternion{45, 45, 45, 9}, 0, 0, 1, false, 1, value
		) == Status::Ok
	);
	const auto q = std::get<Quaternion>(value);
	CHECK(std::abs(q.X + half) < 1e-12);
	CHECK(std::abs(q.Y) < 1e-12);
	CHECK(std::abs(q.Z) < 1e-12);
	CHECK(std::abs(q.W - half) < 1e-12);
}

TEST_CASE(
	"source curve drivers evaluate short, constant, shifted and step records",
	"[imagegraph][timeline_drivers]"
) {
	DriverCurveMap map;
	Curve one{{0, 1, 0, 2, 4, 0}, {{0, 0, .5, .25, 0, 0}}};
	REQUIRE(BuildDriverCurveMap(one, map));
	for (double value : map)
		CHECK(value == 2.5);
	Curve header{{0, 1, 0, .25, 2, 0}, {}};
	REQUIRE(BuildDriverCurveMap(header, map));
	for (double value : map)
		CHECK(value == .6875);
	Curve step{{0, 1, 1, 0, 1, 0}, {{0, 0, 0, .25, 0, 0}, {0, 0, 1, .75, 0, 0}}};
	REQUIRE(BuildDriverCurveMap(step, map));
	for (double value : map)
		CHECK(value == .25); // Source step holds the previous anchor, including x=1.
	Curve constant = KeyframeCurveDriver{}.Data;
	constant.Header[1] = 0;
	constant.Anchors[0][3] = 1;
	constant.Anchors[0][5] = 0;
	constant.Anchors[1][1] = 0;
	REQUIRE(BuildDriverCurveMap(constant, map));
	for (double value : map)
		CHECK(value == 1);
	Curve shifted = KeyframeCurveDriver{}.Data;
	shifted.Header[0] = .25;
	shifted.Header[1] = 2;
	REQUIRE(BuildDriverCurveMap(shifted, map));
	CHECK(map.front() == 0);
	CHECK(map.back() == .25);
}

TEST_CASE(
	"source sine drivers apply the same captured frame phase to numeric components",
	"[imagegraph][timeline_drivers]"
) {
	KeyframeSourceDriver sine = KeyframeSineDriver{1, 2, 0, 0};
	Value value;
	REQUIRE(
		ApplySourceDriver(&sine, Vector3{1, 2, 3}, Vector3{1, 2, 3}, .25, .25, 4, true, -1, value, 16) ==
		Status::Ok
	);
	CHECK((std::get<Vector3>(value) == Vector3{3, 4, 5}));
	sine = KeyframeSineDriver{1, 2, 0, 1};
	REQUIRE(
		ApplySourceDriver(
			&sine, Vector4{1, 2, 3, 4}, Vector4{1, 2, 3, 4}, .25, .25, 4, true, -1, value, 16
		) == Status::Ok
	);
	CHECK((std::get<Vector4>(value) == Vector4{2, 3, 4, 5}));
	REQUIRE(ApplySourceDriver(&sine, int64_t{2}, int64_t{2}, .25, .25, 4, true, -1, value, 16) == Status::Ok);
	CHECK(std::get<int64_t>(value) == 3);
	sine = KeyframeSineDriver{1, 2, .25, 0};
	REQUIRE(ApplySourceDriver(&sine, 1.0, 1.0, 0, .5, 0, false, -1, value, 16) == Status::Ok);
	CHECK(std::get<double>(value) == 3);
	CHECK(ApplySourceDriver(&sine, 1.0, 1.0, 0, .5, 0, false, -1, value, 0) == Status::InvalidValue);
}

TEST_CASE(
	"source endpoint drivers return the last key before ratio arithmetic", "[imagegraph][timeline_drivers]"
) {
	Value value;
	for (const KeyframeSourceDriver &driver :
		 {KeyframeSourceDriver{KeyframeBounceDriver{3, .5, -1}},
		  KeyframeSourceDriver{KeyframeElasticDriver{3, .5, -1}}}) {
		REQUIRE(ApplySourceDriver(&driver, 7.0, 7.0, 0, 0, 10, false, -1, value) == Status::Ok);
		CHECK(std::get<double>(value) == 7);
		CHECK(ApplySourceDriver(&driver, 7.0, 10.0, 0, 0, 10, true, -1, value) == Status::InvalidValue);
	}
	KeyframeCurveDriver curve;
	curve.Data.Header[3] = 2;
	curve.Data.Header[4] = 4;
	const KeyframeSourceDriver driver = curve;
	REQUIRE(ApplySourceDriver(&driver, 7.0, 7.0, 0, .5, 10, false, -1, value) == Status::Ok);
	CHECK(std::get<double>(value) == 7);
}

TEST_CASE(
	"source raw quaternion Slerp follows the shortest sign and trigonometric branch",
	"[imagegraph][timeline_drivers]"
) {
	Quaternion result;
	REQUIRE(SourceRawQuaternionSlerp(Quaternion{}, Quaternion{0, 0, 0, -1}, .5, result));
	CHECK((result == Quaternion{}));
	REQUIRE(SourceRawQuaternionSlerp(Quaternion{}, Quaternion{-1, 0, 0, 0}, .5, result));
	CHECK(std::abs(result.X + std::sqrt(.5)) < 1e-12);
	CHECK(std::abs(result.W - std::sqrt(.5)) < 1e-12);
}
