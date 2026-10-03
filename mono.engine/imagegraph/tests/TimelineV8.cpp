#include "TimelineDrivers.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.timeline.v8")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

namespace {
	Document SourceDriverGraph(const KeyframeSourceDriver &driver) {
		Document document;
		document.FormatVersion = 8;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
		document.Outputs = {{"out", "number", "number"}};
		document.Keyframes = {
			{"number", "value", 2, 0.0, "source", KeyframeEase{}},
			{"number", "value", 12, 10.0, "source", KeyframeEase{}}
		};
		document.Keyframes.front().SourceDriver = driver;
		document.Keyframes.back().SourceDriver = driver;
		document.Tracks = {{"number", "value", "hold", -1}};
		document.Timeline = TimelineSettings{16, 0, 15, "loop", 30};
		return document;
	}
}

TEST_CASE(
	"v8 named source drivers persist and replay exact timeline seeks", "[imagegraph][timeline_drivers]"
) {
	const std::array<KeyframeSourceDriver, 6> drivers{
		KeyframeLinearDriver{2},
		KeyframeSnapDriver{3},
		KeyframeBounceDriver{},
		KeyframeElasticDriver{},
		KeyframeCurveDriver{},
		KeyframeSineDriver{1, 2, 0, 0}
	};
	const std::array<double, 6> expected{22, 6, 6, 14, 6, 6};
	for (size_t index = 0; index < drivers.size(); index++) {
		Document source = SourceDriverGraph(drivers[index]), parsed;
		Diagnostic diagnostic;
		Plan plan;
		const auto text = Write(source);
		REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
		CHECK(parsed == source);
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		for (uint64_t tick : {0, 8, 3, 8, 12}) {
			EvaluatedValue value, replay;
			EvaluationRequest request;
			request.Tick = tick;
			REQUIRE(EvaluateValue(parsed, plan, "out", request, value, diagnostic) == Status::Ok);
			REQUIRE(EvaluateValue(parsed, plan, "out", request, replay, diagnostic) == Status::Ok);
			CHECK(value.Data == replay.Data);
			if (tick == 0) CHECK(std::get<double>(value.Data) == 0);
			if (tick == 8) CHECK(std::abs(std::get<double>(value.Data) - expected[index]) < 1e-12);
			if (tick == 12)
				CHECK(
					std::abs(
						std::get<double>(value.Data) - (index == 0	 ? 34
														: index == 1 ? 9
														: index == 5 ? 8
																	 : 10)
					) < 1e-12
				);
		}
	}
}

TEST_CASE(
	"v8 source driver metadata rejects ambiguous records and preserves older project settings",
	"[imagegraph][timeline_drivers]"
) {
	Diagnostic diagnostic;
	Plan plan;
	Document source = SourceDriverGraph(KeyframeLinearDriver{});
	source.Keyframes.front().SineDriver = KeyframeSineDriver{};
	CHECK(Compile(source, plan, diagnostic) == Status::InvalidValue);
	source.Keyframes.front().SineDriver.reset();
	source.FormatVersion = 7;
	CHECK(Compile(source, plan, diagnostic) == Status::UnsupportedVersion);
	for (uint32_t version = 1; version <= 7; version++) {
		Document old;
		old.FormatVersion = version;
		if (version == 7) old.Project = ProjectSettings{64, 32, 2, 5, {{1, 2, 3, 4}}};
		const auto project = old.Project;
		REQUIRE(Migrate(old, diagnostic) == Status::Ok);
		CHECK(old.FormatVersion == 9);
		CHECK(old.Project == project);
	}
	source.FormatVersion = 8;
	auto text = Write(source);
	Document retained;
	retained.Nodes = {{"keep", "value.number", "", {}, {{"value", 1.0}}}};
	const auto before = retained;
	const auto end = text.find('\n', text.find("key_source_driver"));
	text.insert(end + 1, "key_source_driver \"number\" \"value\" 2 \"snap\" 1\n");
	CHECK(Read(text, retained, diagnostic) == Status::Malformed);
	CHECK(retained == before);
	source.Keyframes.front().SourceDriver =
		KeyframeCurveDriver{Curve{{0, 1, 0, 2, 4, 0}, {{0, 0, .5, .25, 0, 0}}}};
	REQUIRE(Read(Write(source), retained, diagnostic) == Status::Ok);
	CHECK(retained == source);
}

TEST_CASE(
	"v8 persisted source integer keys round after drivers and report bad domains",
	"[imagegraph][timeline_drivers]"
) {
	Document source;
	source.FormatVersion = 8;
	source.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}}
	};
	source.Outputs = {{"image", "solid", "image"}};
	source.Keyframes = {
		{"solid", "width", 0, int64_t{2}, "source", KeyframeEase{}},
		{"solid", "width", 4, int64_t{7}, "source", KeyframeEase{}}
	};
	source.Keyframes.front().SourceDriver = KeyframeLinearDriver{.25};
	source.Tracks = {{"solid", "width", "hold", -1}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(source), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	Image image;
	EvaluationRequest request;
	request.Tick = 1;
	REQUIRE(Evaluate(parsed, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 4);
	parsed.Keyframes.front().SourceDriver = KeyframeLinearDriver{1e308};
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(parsed, plan, "image", request, image, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "solid");
	CHECK(diagnostic.Port == "width");
}

TEST_CASE(
	"v8 persisted quaternion tracks keep explicit raw and Euler modes", "[imagegraph][timeline_drivers]"
) {
	Document source;
	source.FormatVersion = 8;
	source.Nodes = {{"angles", "pc.quarternion_to_euler", "", {}, {{"rotation", Quaternion{}}}}};
	source.Outputs = {{"angles", "angles", "euler_angles"}};
	source.Keyframes = {
		{"angles", "rotation", 0, Quaternion{0, 0, 0, 0}, "source", KeyframeEase{}},
		{"angles", "rotation", 4, Quaternion{90, 0, 0, 0}, "source", KeyframeEase{}}
	};
	source.Tracks = {{"angles", "rotation", "hold", -1, 1}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(source), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == source);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	for (uint64_t tick : {2, 4, 0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue value;
		REQUIRE(EvaluateValue(parsed, plan, "angles", request, value, diagnostic) == Status::Ok);
		CHECK((std::get<Vector3>(value.Data) == Vector3{-double(tick) * 22.5, 0, 0}));
	}
	parsed.Tracks.front().QuaternionMode = 0;
	parsed.Keyframes.front().Data = Quaternion{};
	parsed.Keyframes.back().Data = Quaternion{-std::sqrt(.5), 0, 0, std::sqrt(.5)};
	REQUIRE(Read(Write(parsed), source, diagnostic) == Status::Ok);
	CHECK(source == parsed);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	EvaluationRequest request;
	request.Tick = 2;
	REQUIRE(EvaluateValue(parsed, plan, "angles", request, value, diagnostic) == Status::Ok);
	CHECK((std::get<Vector3>(value.Data) == Vector3{-45, 0, 0}));
}

TEST_CASE(
	"v8 source drivers respect fractional loop ping wrap and cut transitions",
	"[imagegraph][timeline_drivers]"
) {
	Document source = SourceDriverGraph(KeyframeLinearDriver{1});
	source.Keyframes.back().SourceDriver.reset();
	source.Timeline->Frames = 16;
	Diagnostic diagnostic;
	Plan plan;
	const std::array<std::string, 3> ends{"loop", "ping", "wrap"};
	for (const auto &end : ends) {
		source.Tracks.front().End = end;
		REQUIRE(Compile(source, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = 13;
		request.Subframe = .5;
		EvaluatedValue value;
		REQUIRE(EvaluateValue(source, plan, "out", request, value, diagnostic) == Status::Ok);
		// Independent mapped positions: loop3.5 -> base1.5+3.5=5; ping10.5 ->8.5+10.5=19;
		// wrap final key has no driver, its closing segment base7.5.
		CHECK(
			std::abs(
				std::get<double>(value.Data) - (end == "loop"	? 5
												: end == "ping" ? 19
																: 7.5)
			) < 1e-12
		);
	}
	source.Tracks.front().End = "hold";
	source.Keyframes.back().Ease->InType = "cut";
	REQUIRE(Compile(source, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	EvaluationRequest request;
	request.Tick = 8;
	REQUIRE(EvaluateValue(source, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 0);
}

TEST_CASE(
	"v8 empty source array keys preserve endpoint shape and interpolate scalar zero",
	"[imagegraph][timeline_drivers]"
) {
	for (const auto &driver : std::array<std::optional<KeyframeSourceDriver>, 3>{
			 std::nullopt,
			 KeyframeSourceDriver{KeyframeLinearDriver{2}},
			 KeyframeSourceDriver{KeyframeSineDriver{1, 2, 0, 0}}
		 }) {
		Document source = SourceDriverGraph(KeyframeLinearDriver{});
		source.Nodes.front().Type = "pc.number";
		source.Outputs.front().Port = "number";
		for (auto &key : source.Keyframes) {
			key.Data = ArrayValue{ValueType::Scalar, {}};
			key.SourceDriver = driver;
		}
		Document parsed;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Read(Write(source), parsed, diagnostic) == Status::Ok);
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		for (uint64_t tick : {0, 12, 8, 0, 12}) {
			EvaluationRequest request;
			request.Tick = tick;
			EvaluatedValue value;
			REQUIRE(EvaluateValue(parsed, plan, "out", request, value, diagnostic) == Status::Ok);
			if (tick == 8) {
				REQUIRE(std::holds_alternative<double>(value.Data));
				CHECK(
					std::abs(
						std::get<double>(value.Data) -
						(driver && std::holds_alternative<KeyframeLinearDriver>(*driver) ? 16 : 0)
					) < 1e-12
				);
			} else
				CHECK((value.Data == Value{ArrayValue{ValueType::Scalar, {}}}));
		}
	}
}

TEST_CASE(
	"source sine uses its own durable record while legacy sine remains unchanged",
	"[imagegraph][timeline_drivers]"
) {
	Document source = SourceDriverGraph(KeyframeSineDriver{1, 2, 0, 0});
	Document legacy = source;
	legacy.FormatVersion = 6;
	for (auto &key : legacy.Keyframes) {
		key.SourceDriver.reset();
		key.SineDriver = KeyframeSineDriver{1, 2, 0, 0};
	}
	CHECK(Write(legacy).find("key_source_driver") == std::string::npos);
	CHECK(Write(legacy).find("key_driver \"number\" \"value\" 2 \"sine\" 1 2 0 0") != std::string::npos);
	CHECK(
		Write(source).find("key_source_driver \"number\" \"value\" 2 \"sine\" 1 2 0 0") != std::string::npos
	);
	Diagnostic diagnostic;
	Plan legacyPlan, sourcePlan;
	REQUIRE(Compile(legacy, legacyPlan, diagnostic) == Status::Ok);
	REQUIRE(Compile(source, sourcePlan, diagnostic) == Status::Ok);
	for (uint64_t tick : {0, 2, 4, 8, 12}) {
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue oldValue, newValue;
		REQUIRE(EvaluateValue(legacy, legacyPlan, "out", request, oldValue, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(source, sourcePlan, "out", request, newValue, diagnostic) == Status::Ok);
		CHECK(std::abs(std::get<double>(oldValue.Data) - std::get<double>(newValue.Data)) < 1e-12);
	}
}

TEST_CASE(
	"source Quaternion conversion rejects invalid metadata and leaves output intact",
	"[imagegraph][timeline_drivers]"
) {
	Quaternion result{1, 2, 3, 4};
	CHECK_FALSE(ConvertSourceQuaternion(Quaternion{}, 2, result));
	CHECK_FALSE(
		ConvertSourceQuaternion(Quaternion{std::numeric_limits<double>::infinity(), 0, 0, 1}, 0, result)
	);
	CHECK((result == Quaternion{1, 2, 3, 4}));
	REQUIRE(ConvertSourceQuaternion(Quaternion{90, 0, 0, 999}, 1, result));
	CHECK(std::abs(result.X + std::sqrt(.5)) < 1e-12);
	CHECK(std::abs(result.W - std::sqrt(.5)) < 1e-12);
}

TEST_CASE(
	"source Number rounds each flat array component with half-even ties", "[imagegraph][timeline_drivers]"
) {
	Document graph;
	graph.FormatVersion = 8;
	graph.Nodes = {
		{"number",
		 "pc.number",
		 "",
		 {},
		 {{"value", ArrayValue{ValueType::Scalar, {-2.5, -1.5, -.5, .5, 1.5, 2.5}}}, {"integer", true}}}
	};
	graph.Outputs = {{"out", "number", "number"}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(graph), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(parsed, plan, "out", EvaluationRequest{}, value, diagnostic) == Status::Ok);
	CHECK((value.Data == Value{ArrayValue{ValueType::Scalar, {-2., -2., 0., 0., 2., 2.}}}));
	std::get<ArrayValue>(parsed.Nodes.front().Values.front().Data).Elements.front() =
		std::numeric_limits<double>::infinity();
	CHECK(Compile(parsed, plan, diagnostic) == Status::TypeMismatch);
	std::get<ArrayValue>(parsed.Nodes.front().Values.front().Data)
		.Elements.assign(Limits::MaximumArrayElements + 1, 0.0);
	CHECK(Compile(parsed, plan, diagnostic) == Status::LimitExceeded);
}

TEST_CASE(
	"persisted verified source choices keep fractional interpolation and legacy enum literals",
	"[imagegraph][timeline_drivers][source_choice]"
) {
	Document graph;
	graph.FormatVersion = 8;
	graph.Nodes = {{"number", "pc.number", "", {}, {{"value", 2.0}, {"style", EnumValue{0}}}}};
	graph.Outputs = {{"out", "number", "number"}};
	graph.Keyframes = {
		{"number", "style", 0, EnumValue{0}, "source", KeyframeEase{}},
		{"number", "style", 4, EnumValue{1}, "source", KeyframeEase{}}
	};
	graph.Tracks = {{"number", "style", "hold", -1}};
	CHECK(Write(graph).find(" e 0") != std::string::npos);
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(graph), parsed, diagnostic) == Status::Ok);
	CHECK(parsed.Keyframes.front().Data == Value{EnumValue{0}});
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	for (uint64_t tick : {2, 4, 0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		std::vector<AuthoredValue> values;
		REQUIRE(ResolveNodeValues(parsed, plan, "out", "number", request, values, diagnostic) == Status::Ok);
		const auto style = std::find_if(values.begin(), values.end(), [](const auto &value) {
			return value.Port == "style";
		});
		REQUIRE(style != values.end());
		CHECK(style->Data == Value{double(tick) / 4});
	}
	parsed.Keyframes.front().SourceDriver = KeyframeLinearDriver{.25};
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	std::vector<AuthoredValue> values;
	REQUIRE(ResolveNodeValues(parsed, plan, "out", "number", request, values, diagnostic) == Status::Ok);
	const auto style =
		std::find_if(values.begin(), values.end(), [](const auto &value) { return value.Port == "style"; });
	REQUIRE(style != values.end());
	CHECK(style->Data == Value{1.0});
	Document strict;
	strict.FormatVersion = 8;
	strict.Nodes = {{"window", "pc.audio_window", "", {}, {{"location_unit", .5}}}};
	CHECK(Compile(strict, plan, diagnostic) == Status::TypeMismatch);
}

TEST_CASE(
	"animated source choice arrays keep shorter endpoint shape and real components",
	"[imagegraph][timeline_drivers][source_choice]"
) {
	Document graph;
	graph.FormatVersion = 8;
	graph.Nodes = {{"fft", "pc.fft", "", {}, {{"data", ArrayValue{ValueType::Scalar, {0., 1., 0., 0.}}}}}};
	graph.Outputs = {{"out", "fft", "array"}};
	graph.Keyframes = {
		{"fft",
		 "preprocess_function",
		 0,
		 ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{2}, int64_t{99}}},
		 "source",
		 KeyframeEase{}},
		{"fft",
		 "preprocess_function",
		 4,
		 ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{1}}},
		 "source",
		 KeyframeEase{}}
	};
	graph.Tracks = {{"fft", "preprocess_function", "hold", -1}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(graph), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	EvaluatedValue output;
	REQUIRE(EvaluateValue(parsed, plan, "out", request, output, diagnostic) == Status::Ok);
	// The quarter-period impulse distinguishes truncation to None/Hann from rounded Blackman.
	CHECK((output.Data == Value{ArrayValue{ValueType::Scalar, {}, {{1., 1., 1.}, {.5, .5, .5}}}}));
	std::vector<AuthoredValue> values;
	REQUIRE(ResolveNodeValues(parsed, plan, "out", "fft", request, values, diagnostic) == Status::Ok);
	const auto choice = std::find_if(values.begin(), values.end(), [](const auto &value) {
		return value.Port == "preprocess_function";
	});
	REQUIRE(choice != values.end());
	CHECK((choice->Data == Value{ArrayValue{ValueType::Scalar, {.5, 1.5}}}));
	parsed.Keyframes.back().Data = ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{2}}};
	Document whole;
	REQUIRE(Read(Write(parsed), whole, diagnostic) == Status::Ok);
	REQUIRE(Compile(whole, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(whole, plan, "out", request, output, diagnostic) == Status::Ok);
	const double blackman = double(float(.34));
	CHECK(
		(output.Data ==
		 Value{ArrayValue{ValueType::Scalar, {}, {{1., 1., 1.}, {blackman, blackman, blackman}}}})
	);
}
