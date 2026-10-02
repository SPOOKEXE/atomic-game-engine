#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_builtin_random")
using namespace engine::imagegraph;
TEST_CASE(
	"Builtin random recordings validate source draw bounds before owned admission", "[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture;
	capture.Authored.Id = "random";
	capture.Authored.Type = "pc.mk_sparkle";
	capture.Inputs.push_back({"seed", int64_t{7}});
	capture.Draws = {
		{SourceBuiltinRandomOperation::Random, 0, 1, .5},
		{SourceBuiltinRandomOperation::IRandom, 0, 3.5, 3},
		{SourceBuiltinRandomOperation::IRandomRange, 4.9, -2.2, -3},
		{SourceBuiltinRandomOperation::CRand, 0, 0, INT_MAX}
	};
	Diagnostic diagnostic;
	uint64_t bytes = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	REQUIRE(bytes > sizeof(capture));
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, bytes - 1, bytes, diagnostic) == Status::LimitExceeded
	);
	for (size_t index = 0; index < capture.Draws.size(); ++index) {
		auto invalid = capture;
		invalid.Draws[index].Result = index == 0 ? 1 : .5;
		REQUIRE(
			ValidateBuiltinRandomCaptures({&invalid, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
			Status::InvalidValue
		);
	}

	auto invalidTime = capture;
	for (const FrameTime time :
		 {FrameTime{0, 0, true},
		  FrameTime{Limits::MaximumTick + 1, 0, false},
		  FrameTime{Limits::MaximumTick, .5, false}}) {
		invalidTime.Tick = time.Tick;
		invalidTime.Subframe = time.Subframe;
		invalidTime.NegativeFrame = time.NegativeFrame;
		CHECK(
			ValidateBuiltinRandomCaptures(
				{&invalidTime, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic
			) == Status::InvalidValue
		);
	}
	auto oversized = capture;
	oversized.Authored.Values.assign(Limits::MaximumArrayElements + 1, {"seed", 7.0});
	CHECK(
		ValidateBuiltinRandomCaptures({&oversized, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::LimitExceeded
	);
	auto invalid = capture;
	invalid.Draws.back().Lower = 1;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&invalid, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::InvalidValue
	);
	invalid = capture;
	invalid.Draws.front().Result = std::numeric_limits<double>::quiet_NaN();
	REQUIRE(
		ValidateBuiltinRandomCaptures({&invalid, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::InvalidValue
	);
	invalid = capture;
	invalid.Inputs.push_back(invalid.Inputs.front());
	REQUIRE(
		ValidateBuiltinRandomCaptures({&invalid, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::DuplicateId
	);
	std::vector<SourceBuiltinRandomCapture> duplicate{capture, capture};
	REQUIRE(
		ValidateBuiltinRandomCaptures(duplicate, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::DuplicateId
	);
	duplicate.back().Tick = 1;
	REQUIRE(
		ValidateBuiltinRandomCaptures(duplicate, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	capture.Draws.assign(65536, {SourceBuiltinRandomOperation::CRand, 0, 0, 42});
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	capture.Draws.push_back(capture.Draws.back());
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::LimitExceeded
	);
}

TEST_CASE(
	"Builtin random preparation resolves controls without consuming source draws", "[source_builtin_random]"
) {
	Document document;
	document.FormatVersion = 9;
	ArrayValue palette;
	palette.ElementType = ValueType::Colour;
	palette.Elements.push_back(Colour{1, 1, 1, 1});
	document.Nodes = {
		{"spark", "pc.mk_sparkle", "", {}, {{"seed", 7.0}, {"colors", palette}, {"size", int64_t{8}}}}
	};
	document.Outputs = {{"image", "spark", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	SourceBuiltinRandomCapture prepared;
	EvaluationRequest request;
	request.Tick = 9;
	request.Subframe = .25;
	const auto status =
		PrepareSourceBuiltinRandomCapture(document, plan, "spark", request, prepared, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(prepared.Authored == document.Nodes.front());
	CHECK(prepared.Tick == 9);
	CHECK(prepared.Subframe == .25);
	CHECK(prepared.Draws.empty());
	const auto seed =
		std::find_if(prepared.Inputs.begin(), prepared.Inputs.end(), [](const AuthoredValue &input) {
			return input.Port == "seed";
		});
	REQUIRE(seed != prepared.Inputs.end());
	CHECK(std::get<double>(seed->Data) == 7);
	const auto before = prepared;
	SourceBuiltinRandomCapture fresh;
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "spark", request, fresh, diagnostic, 1024 * 1024) ==
		Status::Ok
	);
	auto retained = prepared;
	retained.Subframe = 2; // Invalid recording metadata does not prevent bounded storage replacement.
	retained.Draws.reserve(65536);
	const auto prior = retained;
	const auto retainedCapacity = retained.Draws.capacity();
	CHECK(
		PrepareSourceBuiltinRandomCapture(
			document, plan, "spark", request, retained, diagnostic, 1024 * 1024
		) == Status::LimitExceeded
	);
	CHECK(retained == prior);
	CHECK(retained.Draws.capacity() == retainedCapacity);
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "spark", request, retained, diagnostic) ==
		Status::Ok
	);
	CHECK(retained == prepared);

	CHECK(
		PrepareSourceBuiltinRandomCapture(document, plan, "spark", request, prepared, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(prepared == before);
	auto oversized = document;
	oversized.Nodes.front().Values.assign(Limits::MaximumArrayElements + 1, {"seed", 7.0});
	CHECK(
		PrepareSourceBuiltinRandomCapture(oversized, plan, "spark", request, prepared, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(prepared == before);

	CHECK(
		PrepareSourceBuiltinRandomCapture(document, plan, "missing", request, prepared, diagnostic) ==
		Status::UnknownNode
	);
	CHECK(prepared == before);
}

TEST_CASE(
	"Global builtin recordings are admitted before an unrelated graph output and share its byte budget",
	"[source_builtin_random]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{64, 64}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{91, 32, 17, 255}}}}
	};
	document.Outputs = {{"image", "solid", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceBuiltinRandomCapture capture;
	capture.Authored.Id = "unselected";
	capture.Authored.Type = "pc.mk_sparkle";
	capture.Draws.assign(65536, {SourceBuiltinRandomOperation::CRand, 0, 0, 42});
	capture.Draws.clear(); // Retained capacity remains resident even with no recorded calls.
	uint64_t bytes = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	const uint64_t aggregateBudget = bytes + 128;
	Image output;
	REQUIRE(
		Evaluate(document, plan, "image", EvaluationRequest{}, output, diagnostic, aggregateBudget) ==
		Status::Ok
	);
	REQUIRE(output.Width == 64);
	const Image previous = output;
	EvaluationRequest request;
	request.BuiltinRandomCaptures = {&capture, 1};
	CHECK(
		Evaluate(document, plan, "image", request, output, diagnostic, aggregateBudget) ==
		Status::LimitExceeded
	);
	CHECK(output.Width == previous.Width);
	CHECK(output.Height == previous.Height);
	CHECK(output.Pixels == previous.Pixels);
	capture.Draws.push_back({SourceBuiltinRandomOperation::CRand, 0, 0, -1});
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "unselected");
	CHECK(output.Pixels == previous.Pixels);
}

TEST_CASE(
	"Captured seed observations and inclusive real ranges preserve typed source calls",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture;
	capture.Authored.Id = "stream";
	capture.Authored.Type = "pc.rigid_object_spawner";
	capture.Draws = {
		{SourceBuiltinRandomOperation::SeedObservation, 0, 0, -.25},
		{SourceBuiltinRandomOperation::RandomRange, -2, 3, -2},
		{SourceBuiltinRandomOperation::RandomRange, -2, 3, 3},
		{SourceBuiltinRandomOperation::RandomRange, 4, 4, 4},
		{SourceBuiltinRandomOperation::RandomRange, 3, -2, .125}
	};
	Diagnostic diagnostic;
	uint64_t bytes = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	const auto retained = capture;
	REQUIRE(bytes > 0);
	const auto admitted = bytes;
	CHECK(
		ValidateBuiltinRandomCaptures({&capture, 1}, admitted - 1, bytes, diagnostic) == Status::LimitExceeded
	);
	CHECK(capture == retained);
	for (const auto invalid : std::array<SourceBuiltinRandomDraw, 6>{
			 {{SourceBuiltinRandomOperation::SeedObservation, 1, 0, 7},
			  {SourceBuiltinRandomOperation::SeedObservation, 0, 1, 7},
			  {SourceBuiltinRandomOperation::SeedObservation, 0, 0, std::numeric_limits<double>::infinity()},
			  {SourceBuiltinRandomOperation::RandomRange, -2, 3, -2.001},
			  {SourceBuiltinRandomOperation::RandomRange, 4, 4, 4.001},
			  {static_cast<SourceBuiltinRandomOperation>(255), 0, 0, 0}}
		 }) {
		capture.Draws = {invalid};
		CHECK(
			ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(diagnostic.NodeId == "stream");
		CHECK_FALSE(diagnostic.Message.empty());
	}
}
