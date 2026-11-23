// The built-in node graph run through ang, compared with the output the
// retired graph::NodeRuntime produced for the same graph.

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/graph/BuiltinNodes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.builtinnodesgolden")
TEST_TIER("fast")

using namespace engine;

TEST_CASE("built-in node graph output matches the recorded runtime", "[graph][golden]") {
	ang::Registry registry = ang::Registry::WithScalars();
	REQUIRE(graph::RegisterBuiltinNodes(registry).Ok);
	std::vector<ang::Edit> edits;
	const auto node = [&](const char *id, const char *def, std::map<std::string, ang::Value> params = {}) {
		edits.push_back(ang::edit::AddNode{{.Id = id, .Def = def, .Params = std::move(params)}});
	};
	const auto wire = [&](const char *from, const char *fromPort, const char *to, const char *toPort) {
		edits.push_back(ang::edit::AddLink{{from, fromPort, to, toPort}});
	};
	node("a", "value.Number", {{"value", 3.0}});
	node("b", "value.Number", {{"value", 4.5}});
	node("sum", "math.Add");
	node("scaled", "math.Multiply", {{"right", 0.25}});
	node("clamped", "math.Clamp", {{"max", 1.5}});
	node("remapped", "math.Remap", {{"sourceMax", 10.0}, {"targetMin", -1.0}});
	node("big", "compare.GreaterThan", {{"right", 0.25}});
	node("small", "logic.Not");
	node("pick", "logic.Switch");
	node("text", "convert.ToString");
	node("route", "flow.Branch");
	node("merged", "flow.Merge");
	wire("a", "value", "sum", "left");
	wire("b", "value", "sum", "right");
	wire("sum", "value", "scaled", "left");
	wire("scaled", "value", "clamped", "value");
	wire("sum", "value", "remapped", "value");
	wire("remapped", "value", "big", "left");
	wire("big", "value", "small", "value");
	wire("small", "result", "pick", "condition");
	wire("clamped", "value", "pick", "true");
	wire("remapped", "value", "pick", "false");
	wire("pick", "value", "text", "value");
	wire("big", "value", "route", "condition");
	wire("text", "value", "route", "value");
	wire("route", "true", "merged", "first");
	wire("sum", "value", "merged", "second");
	ang::Graph document;
	REQUIRE(document.Apply(edits).Ok);
	ang::Plan plan;
	REQUIRE(ang::Compile(document, registry, {}, plan).Ok);
	ang::Runner runner(std::move(plan));
	const ang::RunReport report = runner.Run();
	CHECK(report.Failed == 0);
	CHECK(report.Skipped == 0);

	const auto number = [&](const char *id) {
		const ang::Value *value = runner.Output(id, "value");
		REQUIRE(value != nullptr);
		return value->AsNumber();
	};
	const auto text = [&](const char *id, const char *port) {
		const ang::Value *value = runner.Output(id, port);
		REQUIRE(value != nullptr);
		REQUIRE(value->Get<std::string>() != nullptr);
		return *value->Get<std::string>();
	};
	CHECK(number("sum") == 7.5);
	CHECK(number("scaled") == 1.875);
	CHECK(number("clamped") == 1.5);
	CHECK(number("remapped") == 0.5);
	CHECK(*runner.Output("big", "value")->Get<bool>());
	CHECK_FALSE(*runner.Output("small", "result")->Get<bool>());
	CHECK(text("text", "value") == "0.5");
	CHECK(text("merged", "value") == "0.5");
	CHECK(runner.Output("route", "false") == nullptr);
}
