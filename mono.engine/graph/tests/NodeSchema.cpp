#include <engine/graph/NodeSchema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.graph.nodeschema")
TEST_DEPENDS("engine.graph.enginegraph")

using namespace engine::graph;

namespace {
	const NodeSchema &Schema(std::string_view id) {
		const NodeSchema *schema = BuiltinNodeSchemas().Find(id);
		REQUIRE(schema != nullptr);
		return *schema;
	}

	NodeEvaluationStatus
	Evaluate(std::string_view id, NodeValues inputs, NodeValues &outputs, std::string &reason) {
		return EvaluateNode(Schema(id), inputs, outputs, &reason);
	}
}

TEST_CASE("node port types have the intended any connection rules", "[graph][nodeschema]") {
	CHECK(ComparePortTypes("number", "number") == NodeTypeCompatibility::Exact);
	CHECK(ComparePortTypes("number", "any") == NodeTypeCompatibility::DestinationAny);
	CHECK(ComparePortTypes("any", "number") == NodeTypeCompatibility::SourceAny);
	CHECK(ComparePortTypes("number", "string") == NodeTypeCompatibility::Incompatible);
	CHECK(ArePortTypesCompatible("any", "number"));
	CHECK(ArePortTypesCompatible("image", "any"));
	CHECK_FALSE(ArePortTypesCompatible("image", "number"));

	CHECK(ValueTypeOf(NodeValue{3.0}) == VALUE_TYPE_NUMBER);
	CHECK(ValueTypeOf(NodeValue{true}) == VALUE_TYPE_BOOLEAN);
	CHECK(ValueTypeOf(NodeValue{std::string("node")}) == VALUE_TYPE_STRING);
	CHECK(ValueTypeOf(NodeValue{NodeOpaqueValue{"image", {}}}) == VALUE_TYPE_IMAGE);
	CHECK(ValueTypeOf(NodeValue{NodeOpaqueValue{"plugin.mesh", {}}}) == "plugin.mesh");
	CHECK(ValueTypeOf(NodeValue{}) == "");
}

TEST_CASE("node schemas reject invalid bypass mappings", "[graph][nodeschema]") {
	NodeSchema missingInput{
		.Id = "test.missing",
		.Title = "Missing",
		.Ports = {{.Id = "out", .ValueType = "number", .Direction = NodePortDirection::Output}},
		.BypassMappings = {{"in", "out"}},
		.Evaluate = {},
	};
	CHECK(ValidateNodeSchema(missingInput) == NodeSchemaStatus::UnknownBypassInput);

	NodeSchema incompatible{
		.Id = "test.incompatible",
		.Title = "Incompatible",
		.Ports =
			{
				{.Id = "in", .ValueType = "number"},
				{.Id = "out", .ValueType = "string", .Direction = NodePortDirection::Output},
			},
		.BypassMappings = {{"in", "out"}},
		.Evaluate = {},
	};
	CHECK(ValidateNodeSchema(incompatible) == NodeSchemaStatus::InvalidBypassType);

	NodeSchema duplicate{
		.Id = "test.duplicate",
		.Title = "Duplicate",
		.Ports = {{.Id = "value", .ValueType = "number"}},
		.BypassMappings = {},
		.Evaluate = {},
	};
	NodeSchemaRegistry registry;
	REQUIRE(registry.Add(std::move(duplicate)) == NodeSchemaStatus::Ok);
	CHECK(registry.Add(registry.All().front()) == NodeSchemaStatus::DuplicateSchemaId);
}

TEST_CASE("built-in node schemas contain the initial logical scalar and flow set", "[graph][nodeschema]") {
	const NodeSchemaRegistry &schemas = BuiltinNodeSchemas();
	CHECK(schemas.Count() == 17);
	for (const std::string_view id : {
			 "logic.And",
			 "logic.Or",
			 "logic.Not",
			 "logic.Switch",
			 "math.Add",
			 "math.Multiply",
			 "math.Clamp",
			 "math.Remap",
			 "compare.Equal",
			 "compare.GreaterThan",
			 "flow.Branch",
			 "flow.Merge",
			 "flow.Reroute",
			 "value.Number",
			 "value.Boolean",
			 "value.String",
			 "convert.ToString",
		 }) {
		CHECK(schemas.Find(id) != nullptr);
	}
	const NodeSchema &reroute = Schema("flow.Reroute");
	REQUIRE(reroute.BypassMappings.size() == 1);
	CHECK(reroute.BypassMappings.front().Input == "value");
	CHECK(reroute.BypassMappings.front().Output == "value");
}

TEST_CASE("logical and scalar built-ins evaluate typed inputs", "[graph][nodeschema]") {
	NodeValues outputs;
	std::string reason;
	REQUIRE(
		Evaluate("logic.And", {{"left", true}, {"right", false}}, outputs, reason) == NodeEvaluationStatus::Ok
	);
	CHECK(std::get<bool>(outputs.at("value")) == false);

	REQUIRE(
		Evaluate("math.Add", {{"left", 2.5}, {"right", 4.0}}, outputs, reason) == NodeEvaluationStatus::Ok
	);
	CHECK(std::get<double>(outputs.at("value")) == 6.5);

	REQUIRE(Evaluate("math.Clamp", {{"value", 4.0}}, outputs, reason) == NodeEvaluationStatus::Ok);
	CHECK(std::get<double>(outputs.at("value")) == 1.0);

	REQUIRE(Evaluate("value.Number", {{"value", 12.0}}, outputs, reason) == NodeEvaluationStatus::Ok);
	CHECK(std::get<double>(outputs.at("value")) == 12.0);
	REQUIRE(Evaluate("value.Boolean", {}, outputs, reason) == NodeEvaluationStatus::Ok);
	CHECK_FALSE(std::get<bool>(outputs.at("value")));

	REQUIRE(
		Evaluate(
			"math.Remap",
			{{"value", 5.0},
			 {"sourceMin", 0.0},
			 {"sourceMax", 10.0},
			 {"targetMin", 20.0},
			 {"targetMax", 40.0}},
			outputs,
			reason
		) == NodeEvaluationStatus::Ok
	);
	CHECK(std::get<double>(outputs.at("value")) == 30.0);
}

TEST_CASE("any inputs validate their actual values only during evaluation", "[graph][nodeschema]") {
	NodeValues outputs;
	std::string reason;
	REQUIRE(
		Evaluate("logic.Switch", {{"condition", true}, {"true", 4.0}}, outputs, reason) ==
		NodeEvaluationStatus::Ok
	);
	CHECK(std::get<double>(outputs.at("value")) == 4.0);

	REQUIRE(
		Evaluate(
			"compare.Equal", {{"left", std::string("same")}, {"right", std::string("same")}}, outputs, reason
		) == NodeEvaluationStatus::Ok
	);
	CHECK(std::get<bool>(outputs.at("value")));

	CHECK(
		Evaluate("math.Add", {{"left", true}, {"right", 2.0}}, outputs, reason) ==
		NodeEvaluationStatus::InputTypeMismatch
	);
	CHECK(reason.find("left") != std::string::npos);
}

TEST_CASE("node evaluation refuses undeclared input and output ports", "[graph][nodeschema]") {
	NodeValues outputs;
	std::string reason;
	CHECK(
		Evaluate("value.Number", {{"unknown", 4.0}}, outputs, reason) == NodeEvaluationStatus::UnknownInput
	);
	CHECK(reason.find("unknown") != std::string::npos);

	NodeSchema unexpectedOutput{
		.Id = "test.unexpected-output",
		.Title = "Unexpected output",
		.Ports = {{.Id = "value", .ValueType = "number", .Direction = NodePortDirection::Output}},
		.BypassMappings = {},
		.Evaluate = [](const NodeSchema &, const NodeValues &, NodeValues &values, std::string &) {
			values["unknown"] = 4.0;
			return true;
		},
	};
	CHECK(EvaluateNode(unexpectedOutput, {}, outputs, &reason) == NodeEvaluationStatus::UnknownOutput);
	CHECK(reason.find("unknown") != std::string::npos);
}

TEST_CASE("flow and conversion built-ins preserve or explicitly convert values", "[graph][nodeschema]") {
	NodeValues outputs;
	std::string reason;
	REQUIRE(
		Evaluate("flow.Branch", {{"condition", false}, {"value", std::string("branch")}}, outputs, reason) ==
		NodeEvaluationStatus::Ok
	);
	CHECK(outputs.contains("false"));
	CHECK_FALSE(outputs.contains("true"));
	CHECK(std::get<std::string>(outputs.at("false")) == "branch");

	REQUIRE(Evaluate("flow.Merge", {{"second", true}}, outputs, reason) == NodeEvaluationStatus::Ok);
	CHECK(std::get<bool>(outputs.at("value")));

	REQUIRE(Evaluate("convert.ToString", {{"value", true}}, outputs, reason) == NodeEvaluationStatus::Ok);
	CHECK(std::get<std::string>(outputs.at("value")) == "true");
}
