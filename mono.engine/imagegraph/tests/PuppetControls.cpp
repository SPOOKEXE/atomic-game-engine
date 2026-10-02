#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.puppet_controls")
using namespace engine::imagegraph;
namespace {
	ArrayValue Control(double x) {
		ArrayValue value;
		value.ElementType = ValueType::Scalar;
		value.Elements = {0.0, x, 2.0, 3.0, 4.0, 5.0, 6.0};
		return value;
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		Node node;
		node.Id = "warp";
		node.Type = "pc.mesh_warp";
		node.DynamicInputs = {{"control_point_0", ValueType::Struct, Control(0)}};
		Node image;
		image.Id = "image";
		image.Type = "image.solid";
		image.Values = {
			{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}
		};
		document.Nodes = {image, node};
		document.Links = {{"image", "image", "warp", "surface_in"}};
		document.Outputs = {{"out", "warp", "surface_out"}};
		document.Keyframes = {
			{"warp", "control_point_0", 0, Control(0), "linear"},
			{"warp", "control_point_0", 10, Control(10), "linear"}
		};
		return document;
	}
}
TEST_CASE(
	"Puppet input snapshots sample seven-scalar control arrays without executing warp", "[imagegraph][puppet]"
) {
	auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(document, plan, "warp", {.Tick = 5}, snapshot, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	auto control = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "control_point_0";
	});
	REQUIRE(control != snapshot.Values().end());
	const auto &array = std::get<ArrayValue>(control->Data);
	REQUIRE(array.Elements.size() == 7);
	CHECK(std::get<double>(array.Elements[1]) == 5.0);
	const std::string text = Write(document);
	Document decoded;
	REQUIRE(Read(text, decoded, diagnostic) == Status::Ok);
	CHECK(decoded == document);
}
TEST_CASE("Puppet defaults and keys reject malformed aliased arrays", "[imagegraph][puppet]") {
	for (int mutation = 0; mutation < 4; ++mutation)
		for (bool key : {false, true}) {
			auto document = Graph();
			auto malformed = Control(0);
			if (mutation == 0) malformed.Elements.pop_back();
			if (mutation == 1) malformed.ElementType = ValueType::Integer;
			if (mutation == 2) malformed.Elements[1] = std::numeric_limits<double>::infinity();
			if (mutation == 3) malformed.Elements[1] = std::string("bad");
			if (key)
				document.Keyframes[0].Data = malformed;
			else
				document.Nodes[1].DynamicInputs[0].Default = malformed;
			Plan plan;
			Diagnostic diagnostic;
			CHECK(Compile(document, plan, diagnostic) != Status::Ok);
		}
}

TEST_CASE(
	"Puppet linear and source keys interpolate all seven slots at fractional frames", "[imagegraph][puppet]"
) {
	for (bool sourceEase : {false, true}) {
		auto document = Graph();
		auto &first = std::get<ArrayValue>(document.Keyframes[0].Data);
		auto &last = std::get<ArrayValue>(document.Keyframes[1].Data);
		for (size_t index = 0; index < 7; ++index) {
			first.Elements[index] = static_cast<double>(index);
			last.Elements[index] = static_cast<double>(index) + 10;
		}
		if (sourceEase) {
			document.Tracks = {{"warp", "control_point_0", "hold", -1}};
			for (auto &key : document.Keyframes) {
				key.Interpolation = "source";
				key.Ease = KeyframeEase{};
			}
		}
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto sampled =
			EvaluateNodeInputs(document, plan, "warp", {.Tick = 2, .Subframe = .5}, snapshot, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(sampled == Status::Ok);
		const auto control = [&] {
			return std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "control_point_0";
			});
		};
		REQUIRE(control() != snapshot.Values().end());
		const auto &array = std::get<ArrayValue>(control()->Data);
		REQUIRE(array.Elements.size() == 7);
		for (size_t index = 0; index < 7; ++index)
			CHECK(std::get<double>(array.Elements[index]) == static_cast<double>(index) + 2.5);
		const auto previous = control()->Data;
		const auto bytes = snapshot.RetainedBytes();
		CHECK(
			EvaluateNodeInputs(document, plan, "warp", {.Tick = 5}, snapshot, diagnostic, 1) ==
			Status::LimitExceeded
		);
		CHECK(snapshot.RetainedBytes() == bytes);
		CHECK(control()->Data == previous);
	}
}
