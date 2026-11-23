#include <engine/graph/Execution.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.graph.builtinnodesgolden")
TEST_TIER("fast")

using engine::core::Name;
using namespace engine::graph;

TEST_CASE("built-in node graph output matches the recorded runtime", "[graph][golden]") {
	NodeRuntime runtime(BuiltinNodeSchemas());
	const auto node = [&](const char *id, const char *schema) {
		REQUIRE(runtime.AddNode({.Id = Name(id), .Schema = Name(schema)}) == NodeExecutionStatus::Ok);
	};
	const auto wire = [&](const char *from, const char *fromPort, const char *to, const char *toPort) {
		REQUIRE(runtime.Connect({Name(from), fromPort, Name(to), toPort}) == NodeExecutionStatus::Ok);
	};
	node("a", "value.Number");
	node("b", "value.Number");
	node("sum", "math.Add");
	node("scaled", "math.Multiply");
	node("clamped", "math.Clamp");
	node("remapped", "math.Remap");
	node("big", "compare.GreaterThan");
	node("small", "logic.Not");
	node("pick", "logic.Switch");
	node("text", "convert.ToString");
	node("route", "flow.Branch");
	node("merged", "flow.Merge");
	REQUIRE(runtime.SetInput(Name("a"), "value", 3.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("b"), "value", 4.5) == NodeExecutionStatus::Ok);
	wire("a", "value", "sum", "left");
	wire("b", "value", "sum", "right");
	wire("sum", "value", "scaled", "left");
	REQUIRE(runtime.SetInput(Name("scaled"), "right", 0.25) == NodeExecutionStatus::Ok);
	wire("scaled", "value", "clamped", "value");
	REQUIRE(runtime.SetInput(Name("clamped"), "max", 1.5) == NodeExecutionStatus::Ok);
	wire("sum", "value", "remapped", "value");
	REQUIRE(runtime.SetInput(Name("remapped"), "sourceMax", 10.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("remapped"), "targetMin", -1.0) == NodeExecutionStatus::Ok);
	wire("remapped", "value", "big", "left");
	REQUIRE(runtime.SetInput(Name("big"), "right", 0.25) == NodeExecutionStatus::Ok);
	wire("big", "value", "small", "value");
	wire("small", "result", "pick", "condition");
	wire("clamped", "value", "pick", "true");
	wire("remapped", "value", "pick", "false");
	wire("pick", "value", "text", "value");
	wire("big", "value", "route", "condition");
	wire("text", "value", "route", "value");
	wire("route", "true", "merged", "first");
	wire("sum", "value", "merged", "second");
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);

	const auto number = [&](const char *id) {
		return std::get<double>(runtime.Find(Name(id))->Outputs.at("value"));
	};
	CHECK(number("sum") == 7.5);
	CHECK(number("scaled") == 1.875);
	CHECK(number("clamped") == 1.5);
	CHECK(number("remapped") == 0.5);
	CHECK(std::get<bool>(runtime.Find(Name("big"))->Outputs.at("value")));
	CHECK_FALSE(std::get<bool>(runtime.Find(Name("small"))->Outputs.at("result")));
	CHECK(std::get<std::string>(runtime.Find(Name("text"))->Outputs.at("value")) == "0.5");
	CHECK(std::get<std::string>(runtime.Find(Name("merged"))->Outputs.at("value")) == "0.5");
	CHECK_FALSE(runtime.Find(Name("route"))->Outputs.contains("false"));
}
