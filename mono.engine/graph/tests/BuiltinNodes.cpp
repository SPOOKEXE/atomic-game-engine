// The built-in node set on the ang runner: typed evaluation, link rules, and
// the caching, bypass and Async completion behaviour graph hosts rely on.

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/graph/BuiltinNodes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.builtinnodes")
TEST_TIER("fast")

using namespace engine;
using ang::Value;

namespace {
	ang::Registry Builtins() {
		ang::Registry registry = ang::Registry::WithScalars();
		REQUIRE(graph::RegisterBuiltinNodes(registry).Ok);
		return registry;
	}

	// Runs one node with `inputs` supplied, returning its report and runner.
	struct Single {
		ang::Registry Registry = Builtins();
		std::unique_ptr<ang::Runner> Run;
		ang::RunReport Report;

		Single(const char *def, ang::RunInputs::mapped_type inputs) {
			ang::Graph document;
			REQUIRE(document.Apply(ang::edit::AddNode{{.Id = "n", .Def = def}}).Ok);
			ang::Plan plan;
			REQUIRE(ang::Compile(document, Registry, {}, plan).Ok);
			Run = std::make_unique<ang::Runner>(std::move(plan));
			Report = Run->Run({{"n", std::move(inputs)}});
		}

		const Value *Out(const char *port = "value") const {
			return Run->Output("n", port);
		}
		const std::string &Note() const {
			return Report.Nodes.front().Note;
		}
	};
}

TEST_CASE("built-in nodes register the logical scalar and flow set", "[graph][builtinnodes]") {
	const ang::Registry registry = Builtins();
	CHECK(registry.Nodes().size() == 17);
	const auto reroute = registry.FindNode("flow.Reroute");
	REQUIRE(reroute != nullptr);
	REQUIRE(reroute->Bypass.size() == 1);
	CHECK(reroute->Bypass.front().Input == "value");
	ang::Registry twice = Builtins();
	CHECK_FALSE(graph::RegisterBuiltinNodes(twice).Ok);

	CHECK(registry.CheckLink("number", "any").Ok);
	CHECK(registry.CheckLink("any", "number").Ok);
	CHECK_FALSE(registry.CheckLink("string", "number").Ok);
}

TEST_CASE("built-ins evaluate typed inputs and check any values at run time", "[graph][builtinnodes]") {
	CHECK_FALSE(*Single("logic.And", {{"left", true}, {"right", false}}).Out()->Get<bool>());
	CHECK(Single("math.Add", {{"left", 2.5}, {"right", 4.0}}).Out()->AsNumber() == 6.5);
	CHECK(Single("math.Clamp", {{"value", 4.0}}).Out()->AsNumber() == 1.0);
	CHECK_FALSE(*Single("value.Boolean", {}).Out()->Get<bool>());
	CHECK(
		Single("math.Remap", {{"value", 5.0}, {"sourceMax", 10.0}, {"targetMin", 20.0}, {"targetMax", 40.0}})
			.Out()
			->AsNumber() == 30.0
	);
	CHECK(Single("logic.Switch", {{"condition", true}, {"true", 4.0}}).Out()->AsNumber() == 4.0);
	CHECK(*Single("compare.Equal", {{"left", "same"}, {"right", "same"}}).Out()->Get<bool>());

	const Single branch("flow.Branch", {{"condition", false}, {"value", "branch"}});
	CHECK(branch.Out("true") == nullptr);
	CHECK(*branch.Out("false")->Get<std::string>() == "branch");
	CHECK(*Single("flow.Merge", {{"second", true}}).Out()->Get<bool>());
	CHECK(*Single("convert.ToString", {{"value", true}}).Out()->Get<std::string>() == "true");

	const Single empty("math.Remap", {{"value", 1.0}, {"sourceMax", 0.0}});
	CHECK(empty.Report.Failed == 1);
	CHECK(empty.Note() == "remap source range is empty");
	const Single missing("math.Add", {{"left", 1.0}});
	CHECK(missing.Report.Skipped == 1);
	CHECK(missing.Note().find("right") != std::string::npos);
}

TEST_CASE("an any output feeding a typed built-in fails on the actual value", "[graph][builtinnodes]") {
	const ang::Registry registry = Builtins();
	ang::Graph document;
	REQUIRE(document
				.Apply(
					std::vector<ang::Edit>{
						ang::edit::AddNode{{.Id = "text", .Def = "flow.Reroute"}},
						ang::edit::AddNode{{.Id = "add", .Def = "math.Add", .Params = {{"right", 1.0}}}},
						ang::edit::AddLink{{"text", "value", "add", "left"}},
					}
				)
				.Ok);
	ang::Plan plan;
	REQUIRE(ang::Compile(document, registry, {}, plan).Ok);
	ang::Runner runner(std::move(plan));
	const ang::RunReport report = runner.Run({{"text", {{"value", "not a number"}}}});
	CHECK(report.Failed == 1);
	CHECK(report.Nodes[1].Note == "input 'left' must be number");
}

TEST_CASE("only the dirty closure reruns and bypass forwards its route", "[graph][builtinnodes]") {
	ang::Registry registry = Builtins();
	std::atomic<int> evaluations{0};
	ang::NodeDef pass = *registry.FindNode("flow.Reroute");
	pass.Id = "test.pass";
	pass.Evaluate = [&](ang::EvalContext &context) {
		++evaluations;
		context.SetOutput("value", context.Input("value"));
		return ang::NodeResult::Success();
	};
	REQUIRE(registry.DefineNode(pass).Ok);

	ang::Graph document;
	REQUIRE(document
				.Apply(
					std::vector<ang::Edit>{
						ang::edit::AddNode{{.Id = "source", .Def = "test.pass", .Params = {{"value", 4.0}}}},
						ang::edit::AddNode{{.Id = "target", .Def = "test.pass"}},
						ang::edit::AddNode{{.Id = "other", .Def = "test.pass", .Params = {{"value", 1.0}}}},
						ang::edit::AddLink{{"source", "value", "target", "value"}},
					}
				)
				.Ok);
	const auto compile = [&] {
		ang::Plan plan;
		REQUIRE(ang::Compile(document, registry, {}, plan).Ok);
		return plan;
	};
	ang::Runner runner(compile());
	runner.Run();
	CHECK(evaluations == 3);
	CHECK(runner.Run().Cached == 3);
	CHECK(evaluations == 3);

	REQUIRE(document.Apply(ang::edit::SetParam{"source", "value", 9.0}).Ok);
	runner.SetPlan(compile());
	runner.Run();
	CHECK(evaluations == 5);
	CHECK(runner.Output("target", "value")->AsNumber() == 9.0);

	REQUIRE(document.Apply(ang::edit::SetBypass{"target", true}).Ok);
	REQUIRE(document.Apply(ang::edit::SetParam{"source", "value", 12.0}).Ok);
	runner.SetPlan(compile());
	const ang::RunReport bypassed = runner.Run();
	CHECK(evaluations == 6);
	CHECK(bypassed.Bypassed == 1);
	CHECK(runner.Output("target", "value")->AsNumber() == 12.0);
}

TEST_CASE("async built-in work completes on a later run and stale work is dropped", "[graph][builtinnodes]") {
	ang::Registry registry = Builtins();
	ang::NodeDef slow = *registry.FindNode("flow.Reroute");
	slow.Id = "test.async";
	slow.Mode = ang::ExecMode::Async;
	REQUIRE(registry.DefineNode(slow).Ok);
	ang::Graph document;
	REQUIRE(
		document.Apply(
					ang::edit::AddNode{{.Id = "async", .Def = "test.async", .Params = {{"value", 7.0}}}}
		).Ok
	);
	ang::Plan plan;
	REQUIRE(ang::Compile(document, registry, {.Runs = ang::Domain::Tick}, plan).Ok == false);
	REQUIRE(ang::Compile(document, registry, {.Runs = ang::Domain::Tool}, plan).Ok);

	ang::AsyncPool pool(1);
	ang::Runner runner(std::move(plan), {.Pool = &pool});
	CHECK(runner.Run().Waiting == 1);
	REQUIRE(document.Apply(ang::edit::SetParam{"async", "value", 11.0}).Ok);
	REQUIRE(ang::Compile(document, registry, {.Runs = ang::Domain::Tool}, plan).Ok);
	runner.SetPlan(std::move(plan));
	CHECK(runner.Run().Waiting == 1);
	pool.Drain();
	const ang::RunReport landed = runner.Run();
	CHECK(landed.Ran == 1);
	CHECK(runner.Output("async", "value")->AsNumber() == 11.0);
}
