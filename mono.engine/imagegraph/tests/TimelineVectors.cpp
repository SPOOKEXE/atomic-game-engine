#include "Timeline.hpp"
#include "TimelineOverrides.hpp"
#include "TimelineSchedule.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.timeline_vectors")
TEST_DEPENDS("engine.imagegraph.document")
using namespace engine::imagegraph;

namespace {
	void Blend(const Value &from, const Value &to, const std::array<Value, 5> &expected) {
		Value result = std::string("last good");
		for (uint64_t elapsed = 0; elapsed <= 4; ++elapsed) {
			REQUIRE(detail::Interpolate(from, to, elapsed, 4, result) == Status::Ok);
			CHECK(result == expected[elapsed]);
			REQUIRE(detail::InterpolateEased(from, to, elapsed / 4.0, result) == Status::Ok);
			CHECK(result == expected[elapsed]);
		}
	}

	void Refuses(const Value &from, const Value &to, double ratio = .5) {
		const Value previous = Vector4{19, 23, 29, 31};
		Value result = previous;
		CHECK(detail::InterpolateEased(from, to, ratio, result) == Status::InvalidValue);
		CHECK(result == previous);
		if (ratio == .5) {
			CHECK(detail::Interpolate(from, to, 1, 2, result) == Status::InvalidValue);
			CHECK(result == previous);
		}
	}

	const Value &Find(const std::vector<AuthoredValue> &values, std::string_view port) {
		const auto found =
			std::find_if(values.begin(), values.end(), [&](const auto &value) { return value.Port == port; });
		REQUIRE(found != values.end());
		return found->Data;
	}

	Document Persist(const Document &document, Plan &plan, Diagnostic &diagnostic) {
		Document decoded;
		REQUIRE(Read(Write(document), decoded, diagnostic) == Status::Ok);
		CHECK(decoded == document);
		const auto status = Compile(decoded, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return decoded;
	}
}

TEST_CASE("Native vector timeline interpolation blends every component", "[imagegraph][timeline_vectors]") {
	Blend(
		Vector3{-8, 4, 20},
		Vector3{8, -12, 4},
		{Vector3{-8, 4, 20}, Vector3{-4, 0, 16}, Vector3{0, -4, 12}, Vector3{4, -8, 8}, Vector3{8, -12, 4}}
	);
	Blend(
		Vector4{-8, 4, 20, -24},
		Vector4{8, -12, 4, 8},
		{Vector4{-8, 4, 20, -24},
		 Vector4{-4, 0, 16, -16},
		 Vector4{0, -4, 12, -8},
		 Vector4{4, -8, 8, 0},
		 Vector4{8, -12, 4, 8}}
	);
	const double maximum = std::numeric_limits<double>::max();
	Value result;
	REQUIRE(
		detail::Interpolate(Vector3{-maximum, 0, maximum}, Vector3{maximum, 0, -maximum}, 1, 2, result) ==
		Status::Ok
	);
	CHECK(result == Value{Vector3{0, 0, 0}});
	REQUIRE(
		detail::InterpolateEased(
			Vector4{-maximum, maximum, 0, 1}, Vector4{maximum, -maximum, 0, 1}, .5, result
		) == Status::Ok
	);
	CHECK(result == Value{Vector4{0, 0, 0, 1}});
}

TEST_CASE("Vector interpolation failures preserve the previous value", "[imagegraph][timeline_vectors]") {
	for (double invalid :
		 {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		for (size_t axis = 0; axis < 4; ++axis) {
			Vector4 bad{1, 2, 3, 4};
			if (axis == 0) bad.X = invalid;
			if (axis == 1) bad.Y = invalid;
			if (axis == 2) bad.Z = invalid;
			if (axis == 3) bad.W = invalid;
			Refuses(bad, Vector4{});
			Refuses(Vector4{}, bad);
			if (axis < 3) {
				const Vector3 bad3{bad.X, bad.Y, bad.Z};
				Refuses(bad3, Vector3{});
				Refuses(Vector3{}, bad3);
			}
		}
	}
	const double maximum = std::numeric_limits<double>::max();
	Refuses(Vector3{0, 0, -maximum}, Vector3{0, 0, maximum}, 2);
	Refuses(Vector4{0, 0, 0, -maximum}, Vector4{0, 0, 0, maximum}, 2);
	Refuses(Vector3{}, Vector3{}, std::numeric_limits<double>::infinity());
	Refuses(Vector4{}, Vector4{}, std::numeric_limits<double>::quiet_NaN());
}

TEST_CASE(
	"Persisted Transform position keys resolve integer and fractional clocks",
	"[imagegraph][timeline_vectors]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"surface",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
		{"transform",
		 "image.transform_3d",
		 "",
		 {},
		 {{"position", Vector3{}},
		  {"anchor", Vector3{}},
		  {"rotation", Quaternion{}},
		  {"scale", Vector3{1, 1, 1}},
		  {"texture_tiling", Vector2{1, 1}},
		  {"projection", EnumValue{1}},
		  {"fov", 45.0},
		  {"view_range", Vector2{.001, 10}},
		  {"depth_range", Vector2{0, 1}}}}
	};
	document.Links = {{"surface", "image", "transform", "surface"}};
	document.Outputs = {{"out", "transform", "rendered"}};
	document.Keyframes = {
		{"transform", "position", 0, Vector3{-8, 4, 20}, "linear"},
		{"transform", "position", 8, Vector3{8, -12, 4}, "linear"}
	};
	for (bool sourceEase : {false, true}) {
		if (sourceEase) document.Tracks = {{"transform", "position", "hold", -1}};
		if (sourceEase)
			for (auto &key : document.Keyframes) {
				key.Interpolation = "source";
				key.Ease = KeyframeEase{};
			}
		Plan plan;
		Diagnostic diagnostic;
		const auto decoded = Persist(document, plan, diagnostic);
		std::vector<AuthoredValue> values;
		REQUIRE(
			ResolveNodeValues(decoded, plan, "out", "transform", {.Tick = 2}, values, diagnostic) ==
			Status::Ok
		);
		CHECK(Find(values, "position") == Value{Vector3{-4, 0, 16}});
		if (!sourceEase) {
			REQUIRE(
				ResolveNodeValues(
					decoded, plan, "out", "transform", {.Tick = 2, .Subframe = .5}, values, diagnostic
				) == Status::Ok
			);
			CHECK(Find(values, "position") == Value{Vector3{-3, -1, 15}});
		}
	}
}

TEST_CASE("Persisted Vector4 dynamic keys resolve through the graph", "[imagegraph][timeline_vectors]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"vectors", "value.array", "", {}, {{"spread", false}}}};
	document.Nodes.front().DynamicInputs = {{"item", ValueType::Vector4, Vector4{}}};
	document.Outputs = {{"out", "vectors", "array"}};
	document.Keyframes = {
		{"vectors", "item", 0, Vector4{-8, 4, 20, -24}, "linear"},
		{"vectors", "item", 8, Vector4{8, -12, 4, 8}, "linear"}
	};
	Plan plan;
	Diagnostic diagnostic;
	const auto decoded = Persist(document, plan, diagnostic);
	// Dynamic keys replace their defaults in the same override path used by graph evaluation.
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides sampled;
	const std::array<uint8_t, 1> needed{1};
	REQUIRE(
		detail::ResolveTimelineOverrides(decoded, needed, {.Tick = 2}, budget, sampled, diagnostic) ==
		Status::Ok
	);
	REQUIRE(sampled.Nodes.size() == 1);
	CHECK(
		sampled.Nodes.front().Authored.DynamicInputs.front().Default ==
		std::optional<Value>{Vector4{-4, 0, 16, -16}}
	);
}
