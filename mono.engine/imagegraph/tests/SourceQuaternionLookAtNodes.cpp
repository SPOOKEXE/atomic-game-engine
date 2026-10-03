#include "NodeHarness.hpp"
namespace engine::imagegraph::detail {
	std::span<const ExecutorEntry> SourceQuaternionLookAtExecutors();
}
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_quaternion_lookat")
using namespace engine::imagegraph;
namespace {
	Document LookAtDocument() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"look", "pc.quarternion_lookat", "", {}, {}}};
		d.Outputs = {{"rotation", "look", "rotation"}};
		return d;
	}
	Value EvaluateLook(Document d, EvaluationRequest request = {}) {
		Plan p;
		Diagnostic diag;
		const auto compiled = Compile(d, p, diag);
		INFO(diag.Message);
		INFO(diag.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue v;
		const auto status = EvaluateValue(d, p, "rotation", request, v, diag);
		INFO(diag.Message);
		INFO(diag.Port);
		REQUIRE(status == Status::Ok);
		return v.Data;
	}
	void LookClose(Vector4 a, Vector4 b) {
		CHECK(a.X == Catch::Approx(b.X).margin(1e-13));
		CHECK(a.Y == Catch::Approx(b.Y).margin(1e-13));
		CHECK(a.Z == Catch::Approx(b.Z).margin(1e-13));
		CHECK(a.W == Catch::Approx(b.W).margin(1e-13));
	}
}
TEST_CASE(
	"Look At default graph preserves source handedness and Euler rounding",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	LookClose(std::get<Vector4>(EvaluateLook(d)), {-.5, .5, -.5, .5});
	d.Nodes[0].Values = {{"unit", EnumValue{1}}};
	CHECK(std::get<Vector3>(EvaluateLook(d)) == Vector3{-90, 0, -90});
	d.Nodes[0].Values = {{"target", Vector3{0, 1, 0}}};
	LookClose(std::get<Vector4>(EvaluateLook(d)), {0, -std::sqrt(.5), std::sqrt(.5), 0});
}
TEST_CASE(
	"Look At handles all180degree diagonal branches and degenerate up",
	"[imagegraph][source_quaternion_lookat]"
) {
	for (const auto &[target, up, expected] : std::array<std::tuple<Vector3, Vector3, Vector4>, 3>{
			 {{{0, 0, -1}, {0, -1, 0}, {1, 0, 0, 0}},
			  {{0, 0, -1}, {0, 1, 0}, {0, 1, 0, 0}},
			  {{0, 0, 1}, {0, -1, 0}, {0, 0, 1, 0}}}
		 }) {
		auto d = LookAtDocument();
		d.Nodes[0].Values = {{"target", target}, {"up", up}};
		LookClose(std::get<Vector4>(EvaluateLook(d)), expected);
	}
	for (Vector3 up : {Vector3{}, Vector3{1, 0, 0}, Vector3{-1, 0, 0}}) {
		auto d = LookAtDocument();
		d.Nodes[0].Values = {{"up", up}};
		CHECK(std::get<Vector4>(EvaluateLook(d)) == Vector4{0, 0, 0, 1});
		d.Nodes[0].Values.push_back({"unit", EnumValue{1}});
		CHECK(std::get<Vector3>(EvaluateLook(d)) == Vector3{});
	}
	auto d = LookAtDocument();
	d.Nodes[0].Values = {{"origin", Vector3{1, 0, 0}}, {"unit", EnumValue{1}}};
	CHECK(std::get<Vector4>(EvaluateLook(d)) == Vector4{0, 0, 0, 1});
}
TEST_CASE(
	"Look At preserves smallup nonunit quaternion native arithmetic profile",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes[0].Values = {{"target", Vector3{0, 0, 1}}, {"up", Vector3{0, .001, 0}}};
	const auto q = std::get<Vector4>(EvaluateLook(d));
	LookClose(q, {0, 0, 0, std::sqrt(2.002) * .5});
	CHECK(q.W * q.W == Catch::Approx(.5005));
}
TEST_CASE(
	"Look At vectors animate without replay history and serialize exact controls",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Keyframes = {
		{"look", "target", 0, Vector3{1, 0, 0}, "linear"}, {"look", "target", 2, Vector3{0, 0, 1}}
	};
	const auto animated = std::get<Vector4>(EvaluateLook(d, EvaluationRequest{.Tick = 1}));
	auto midpoint = LookAtDocument();
	midpoint.Nodes[0].Values = {{"target", Vector3{.5, 0, .5}}};
	LookClose(animated, std::get<Vector4>(EvaluateLook(midpoint)));
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(EvaluateLook(restored, EvaluationRequest{.Tick = 1}) == Value{animated});
	CHECK(EvaluateLook(d, EvaluationRequest{.Tick = 0}) == EvaluateLook(d));
}
TEST_CASE(
	"Look At typed vector rows batch independently and survive save", "[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes.push_back(
		{"targets",
		 "pc.vector3",
		 "",
		 {},
		 {{"x", 0.0}, {"y", 0.0}, {"z", ArrayValue{ValueType::Scalar, {1.0, -1.0}}}}}
	);
	d.Links = {{"targets", "vector", "look", "target"}};
	d.Nodes[0].Values = {{"up", Vector3{0, 1, 0}}};
	const auto result = std::get<ArrayValue>(EvaluateLook(d));
	REQUIRE(result.Items.size() == 2);
	CHECK(std::get<Vector4>(std::get<ElementValue>(result.Items[0].Data)) == Vector4{0, 0, 0, 1});
	CHECK(std::get<Vector4>(std::get<ElementValue>(result.Items[1].Data)) == Vector4{0, 1, 0, 0});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(EvaluateLook(restored) == Value{result});
}
TEST_CASE(
	"Look At linked numeric tuple resizes source coordinates and scalar repeats",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes.push_back({"tuple", "pc.array", "", {}, {}});
	d.Nodes.back().DynamicInputs = {{"a", ValueType::Scalar, 0.0}, {"b", ValueType::Scalar, 1.0}};
	d.Links = {{"tuple", "array", "look", "target"}};
	LookClose(std::get<Vector4>(EvaluateLook(d)), {0, -std::sqrt(.5), std::sqrt(.5), 0});
}
TEST_CASE(
	"Look At translated origin and linked scalar preserve coordinate semantics",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes[0].Values = {{"origin", Vector3{1, 3, 4}}, {"target", Vector3{2, 3, 4}}};
	LookClose(std::get<Vector4>(EvaluateLook(d)), {-.5, .5, -.5, .5});
	d = LookAtDocument();
	d.Junctions = {{"number", "", ValueType::Scalar, 1.0}};
	d.Links = {{"number", "value", "look", "target"}};
	auto expected = LookAtDocument();
	expected.Nodes[0].Values = {{"target", Vector3{1, 1, 1}}};
	CHECK(EvaluateLook(d) == EvaluateLook(expected));
}
TEST_CASE(
	"Look At finite overflow and bytebudget refusal preserve caller result",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes[0].Values = {{"target", Vector3{1e200, 0, 0}}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluatedValue output;
	output.Data = 42.0;
	CHECK(EvaluateValue(d, p, "rotation", {}, output, diag) == Status::UnsupportedExecution);
	CHECK(output.Data == Value{42.0});
	CHECK(diag.Port == "rotation");
	const auto *entry = FindCatalogueEntry("pc.quarternion_lookat");
	REQUIRE(entry);
	Node node{"look", "pc.quarternion_lookat", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	CHECK_FALSE(detail::SourceQuaternionLookAtExecutors()[0].Run(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
}

TEST_CASE(
	"Look At scalar and wholearray surface getters preserve source dimension padding",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes.push_back(
		{"surface",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3}}, {"height", int64_t{4}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	d.Links = {{"surface", "image", "look", "target"}};
	auto expected = LookAtDocument();
	expected.Nodes[0].Values = {{"target", Vector3{3, 4, 0}}};
	CHECK(EvaluateLook(d) == EvaluateLook(expected));
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Vector2;
	dimensions.Elements = {Vector2{3, 4}, Vector2{5, 6}};
	d.Nodes[1] = {
		"surface",
		"pc.solid",
		"",
		{},
		{{"dimension", dimensions}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 255, 255, 255}}}
	};
	d.Links[0].FromPort = "surface_out";
	expected.Nodes[0].Values[0].Data = Vector3{1, 1, 0};
	CHECK(EvaluateLook(d) == EvaluateLook(expected));
}

TEST_CASE(
	"Look At Unit rows preserve quaternion and Euler output lengths", "[imagegraph][source_quaternion_lookat]"
) {
	auto d = LookAtDocument();
	d.Nodes[0].Values = {{"unit", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}}};
	const auto result = std::get<ArrayValue>(EvaluateLook(d));
	REQUIRE(result.Items.size() == 2);
	LookClose(std::get<Vector4>(std::get<ElementValue>(result.Items[0].Data)), {-.5, .5, -.5, .5});
	CHECK(std::get<Vector3>(std::get<ElementValue>(result.Items[1].Data)) == Vector3{-90, 0, -90});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
	CHECK(EvaluateLook(restored) == Value{result});
}

namespace {
	Document LookAtGeneralRows(std::string_view port, std::string json) {
		auto document = LookAtDocument();
		document.Nodes.push_back(
			{"json", "pc.struct_json_parse", "", {}, {{"json_string", std::move(json)}}}
		);
		Node collect{"rows", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}};
		collect.DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
		document.Nodes.push_back(std::move(collect));
		document.Links = {
			{"json", "struct", "rows", "input_0"}, {"rows", "array", "look", std::string(port)}
		};
		document.Outputs.push_back({"rows", "rows", "array"});
		return document;
	}
	ArrayValue LookAtObservedGeneralRows(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		INFO(diagnostic.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue output;
		const auto evaluated = EvaluateValue(document, plan, "rows", {}, output, diagnostic);
		INFO(diagnostic.Message);
		INFO(diagnostic.Port);
		REQUIRE(evaluated == Status::Ok);
		const auto result = std::get<ArrayValue>(output.Data);
		REQUIRE(result.Items.size() == 2);
		return result;
	}
	Value LookAtGeneralLeaf(const SourceArrayItem &item) {
		const auto *leaf = std::get_if<ElementValue>(&item.Data);
		REQUIRE(leaf);
		return std::visit([](const auto &value) -> Value { return value; }, *leaf);
	}
}
TEST_CASE(
	"Look At actual collector general coordinate rows preserve all three vector getters",
	"[imagegraph][source_quaternion_lookat]"
) {
	struct Fixture {
		std::string_view Port, Json;
		Vector3 First, Second;
	};
	const Fixture fixtures[] = {
		{"origin", "[[0,0,0],1]", {0, 0, 0}, {1, 1, 1}},
		{"target", "[[1,0,0],1]", {1, 0, 0}, {1, 1, 1}},
		{"up", "[[0,0,-1],0]", {0, 0, -1}, {0, 0, 0}}
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Port);
		auto document = LookAtGeneralRows(fixture.Port, std::string(fixture.Json));
		const auto original = LookAtObservedGeneralRows(document);
		CHECK(std::holds_alternative<std::vector<SourceArrayItem>>(original.Items[0].Data));
		CHECK(std::holds_alternative<ElementValue>(original.Items[1].Data));
		const auto result = std::get<ArrayValue>(EvaluateLook(document));
		REQUIRE(result.Items.size() == 2);
		auto expected = LookAtDocument();
		expected.Nodes[0].Values = {{std::string(fixture.Port), fixture.First}};
		CHECK(LookAtGeneralLeaf(result.Items[0]) == EvaluateLook(expected));
		expected.Nodes[0].Values[0].Data = fixture.Second;
		CHECK(LookAtGeneralLeaf(result.Items[1]) == EvaluateLook(expected));
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		CHECK(EvaluateLook(restored) == Value{result});
		CHECK(LookAtObservedGeneralRows(document) == original);
	}
}
TEST_CASE(
	"Look At collector general Unit leaves preserve different output tuple lengths",
	"[imagegraph][source_quaternion_lookat]"
) {
	auto document = LookAtGeneralRows("unit", "[false,1]");
	const auto original = LookAtObservedGeneralRows(document);
	CHECK(std::holds_alternative<bool>(std::get<ElementValue>(original.Items[0].Data)));
	CHECK(std::holds_alternative<double>(std::get<ElementValue>(original.Items[1].Data)));
	const auto result = std::get<ArrayValue>(EvaluateLook(document));
	REQUIRE(result.Items.size() == 2);
	LookClose(std::get<Vector4>(LookAtGeneralLeaf(result.Items[0])), {-.5, .5, -.5, .5});
	CHECK(std::get<Vector3>(LookAtGeneralLeaf(result.Items[1])) == Vector3{-90, 0, -90});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(EvaluateLook(restored) == Value{result});
}
TEST_CASE(
	"Look At late nonnumeric general rows refuse atomically after a valid first row",
	"[imagegraph][source_quaternion_lookat]"
) {
	for (const std::string_view port : {"origin", "target", "up", "unit"}) {
		INFO(port);
		const std::string json = port == "unit" ? "[0,\"invalid\"]" : "[[1,0,0],\"invalid\"]";
		auto document = LookAtGeneralRows(port, json);
		const auto original = LookAtObservedGeneralRows(document);
		REQUIRE(std::holds_alternative<std::string>(std::get<ElementValue>(original.Items[1].Data)));
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue output;
		output.Data = Vector4{2, 3, 4, 5};
		const auto sentinel = output;
		const auto evaluated = EvaluateValue(document, plan, "rotation", {}, output, diagnostic);
		INFO(diagnostic.Message);
		INFO(diagnostic.Port);
		CHECK(evaluated == (port == "unit" ? Status::UnsupportedExecution : Status::TypeMismatch));
		CHECK(diagnostic.NodeId == "look");
		CHECK(diagnostic.Port == port);
		CHECK(output.Data == sentinel.Data);
		CHECK(LookAtObservedGeneralRows(document) == original);
	}
}
