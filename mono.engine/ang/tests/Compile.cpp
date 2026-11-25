#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.ang.compile")
TEST_DEPENDS("engine.ang.registry")
TEST_DEPENDS("engine.ang.graph")
TEST_DEPENDS("engine.graphcore.compile")

using namespace engine::ang;

namespace {
	CompileResult
	TryCompile(const Graph &graph, const Registry &registry, Plan &plan, Domain domain = Domain::Tool) {
		return Compile(graph, registry, {.Runs = domain}, plan);
	}
}

TEST_CASE("compile orders waves and resolves link sources", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("sum", "test.add"));
	tests::Add(graph, tests::Node("one", "test.const"));
	tests::Add(graph, tests::Node("two", "test.const"));
	tests::Wire(graph, "one", "value", "sum", "a");
	tests::Wire(graph, "two", "value", "sum", "b");
	const Plan plan = tests::MustCompile(graph, registry);
	REQUIRE(plan.Waves.size() == 2);
	CHECK(plan.Waves[0] == std::vector<uint32_t>{1, 2});
	CHECK(plan.Waves[1] == std::vector<uint32_t>{0});
	const PlannedNode &sum = plan.Nodes[static_cast<size_t>(plan.Find("sum"))];
	REQUIRE(sum.Inputs.size() == 2);
	REQUIRE(sum.Inputs[1].Sources.size() == 1);
	CHECK(sum.Inputs[1].Sources[0].Node == 2);
	CHECK(sum.Wave == 1);
	CHECK(plan.Find("missing") == -1);
}

TEST_CASE("the serial order follows the requested policy", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("sum", "test.add"));
	tests::Add(graph, tests::Node("one", "test.const"));
	tests::Add(graph, tests::Node("two", "test.const"));
	tests::Wire(graph, "one", "value", "sum", "a");
	Plan plan;
	REQUIRE(Compile(graph, registry, {}, plan).Ok);
	CHECK(plan.Order == std::vector<uint32_t>{1, 2, 0});
	REQUIRE(Compile(graph, registry, {.Order = engine::graphcore::OrderPolicy::LowestReady}, plan).Ok);
	CHECK(plan.Order == std::vector<uint32_t>{1, 0, 2});
	CHECK(plan.Waves[0] == std::vector<uint32_t>{1, 2});
}

TEST_CASE("a refused compile keeps the previous plan", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("a", "test.add"));
	tests::Add(graph, tests::Node("b", "test.add"));
	tests::Wire(graph, "a", "sum", "b", "a");
	Plan plan = tests::MustCompile(graph, registry);
	const uint64_t hash = plan.Hash;
	tests::Wire(graph, "b", "sum", "a", "a");
	const CompileResult cycle = TryCompile(graph, registry, plan);
	REQUIRE(!cycle.Ok);
	REQUIRE(cycle.Diagnostics.size() == 1);
	CHECK(cycle.Diagnostics[0].GraphStatus == engine::graphcore::CompileStatus::Cycle);
	CHECK(cycle.Diagnostics[0].Cycle.size() == 3);
	CHECK(plan.Hash == hash);
}

TEST_CASE("type mismatches refuse and conversions are planned", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("text", "test.text"));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "text", "text", "add", "a");
	Plan plan;
	const CompileResult refused = TryCompile(graph, registry, plan);
	REQUIRE(!refused.Ok);
	CHECK(refused.Diagnostics[0].Code == DiagnosticCode::TypeMismatch);
	CHECK(refused.Diagnostics[0].Node == "add");

	Graph widened;
	tests::Add(widened, tests::Node("int", "test.int"));
	tests::Add(widened, tests::Node("add", "test.add"));
	tests::Wire(widened, "int", "value", "add", "a");
	const Plan converted = tests::MustCompile(widened, registry);
	CHECK(static_cast<bool>(converted.Nodes[1].Inputs[0].Sources[0].Convert));
}

TEST_CASE("generic ports resolve from the first link and respect constraints", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("text", "test.text"));
	tests::Add(graph, tests::Node("pass", "test.pass"));
	tests::Add(graph, tests::Node("again", "test.pass"));
	tests::Wire(graph, "text", "text", "pass", "in");
	tests::Wire(graph, "pass", "out", "again", "in");
	const Plan plan = tests::MustCompile(graph, registry);
	CHECK(plan.Nodes[2].OutputTypes[0] == "string");
	CHECK(PortType(graph, registry, "pass", "out", Direction::Output) == "string");

	// The resolved output now refuses a number input.
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "again", "out", "add", "a");
	Plan refused;
	CHECK(!TryCompile(graph, registry, refused).Ok);

	// A constraint refuses an image.
	Graph images;
	tests::Add(images, tests::Node("image", "test.image"));
	tests::Add(images, tests::Node("pass", "test.pass"));
	tests::Wire(images, "image", "image", "pass", "in");
	const CompileResult conflict = TryCompile(images, registry, refused);
	REQUIRE(!conflict.Ok);
	CHECK(conflict.Diagnostics[0].Code == DiagnosticCode::GenericConflict);
}

TEST_CASE("async nodes are refused in Tick graphs only", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("slow", "test.slow"));
	Plan plan;
	const CompileResult tick = TryCompile(graph, registry, plan, Domain::Tick);
	REQUIRE(!tick.Ok);
	CHECK(tick.Diagnostics[0].Code == DiagnosticCode::AsyncInTick);
	CHECK(tick.Diagnostics[0].Message.find("slow") != std::string::npos);
	CHECK(TryCompile(graph, registry, plan, Domain::Frame).Ok);
	CHECK(TryCompile(graph, registry, plan, Domain::Tool).Ok);
}

TEST_CASE("unknown node types load and unknown ports refuse", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("ghost", "plugin.missing"));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "ghost", "anything", "add", "a");
	const Plan plan = tests::MustCompile(graph, registry);
	CHECK(plan.Nodes[0].Def == nullptr);
	CHECK(plan.Nodes[0].OutputNames == std::vector<std::string>{"anything"});

	Graph bad;
	tests::Add(bad, tests::Node("one", "test.const"));
	tests::Add(bad, tests::Node("add", "test.add"));
	tests::Wire(bad, "one", "nope", "add", "a");
	Plan refused;
	const CompileResult result = TryCompile(bad, registry, refused);
	REQUIRE(!result.Ok);
	CHECK(result.Diagnostics[0].Code == DiagnosticCode::UnknownPort);
}

TEST_CASE("bypass without a route refuses", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	NodeInstance node = tests::Node("one", "test.const");
	node.Bypassed = true;
	tests::Add(graph, node);
	Plan plan;
	const CompileResult result = TryCompile(graph, registry, plan);
	REQUIRE(!result.Ok);
	CHECK(result.Diagnostics[0].Code == DiagnosticCode::InvalidBypass);
}

TEST_CASE("hashes cover params and upstream only, so dirtiness is the edited closure", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("one", "test.const", {{"value", Value(1.0)}}));
	tests::Add(graph, tests::Node("two", "test.const", {{"value", Value(2.0)}}));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "one", "value", "add", "a");
	const Plan before = tests::MustCompile(graph, registry);
	// Recompiling an unchanged graph settles on the same hashes.
	CHECK(tests::MustCompile(graph, registry).Hash == before.Hash);
	// Position is not content.
	REQUIRE(graph.Apply(edit::MoveNode{"one", {100.0f, 0.0f}}).Ok);
	CHECK(tests::MustCompile(graph, registry).Hash == before.Hash);
	REQUIRE(graph.Apply(edit::SetParam{"one", "value", Value(5.0)}).Ok);
	const Plan after = tests::MustCompile(graph, registry);
	CHECK(after.DirtySince(before) == std::vector<bool>{true, false, true});
}

TEST_CASE("subgraphs flatten with prefixed ids and refuse recursion", "[ang][compile]") {
	Registry registry = tests::MakeRegistry();
	auto body = std::make_shared<Graph>();
	tests::Add(*body, tests::Node("inner", "test.add", {}));
	REQUIRE(
		registry
			.DefineSubgraph(
				{.Id = "sub.addone",
				 .Body = body,
				 .Inputs = {{.Port = {.Name = "x", .Type = "number"}, .Node = "inner", .NodePort = "a"}},
				 .Outputs = {{.Port = {.Name = "y", .Type = "number"}, .Node = "inner", .NodePort = "sum"}}}
			)
			.Ok
	);
	Graph graph;
	tests::Add(graph, tests::Node("one", "test.const"));
	tests::Add(graph, tests::Node("wrap", "sub.addone"));
	tests::Add(graph, tests::Node("after", "test.add"));
	tests::Wire(graph, "one", "value", "wrap", "x");
	tests::Wire(graph, "wrap", "y", "after", "a");
	const Plan plan = tests::MustCompile(graph, registry);
	CHECK(plan.Find("wrap/inner") >= 0);
	CHECK(plan.Find("wrap") == -1);
	const PlannedNode &after = plan.Nodes[static_cast<size_t>(plan.Find("after"))];
	CHECK(after.Inputs[0].Sources[0].Node == static_cast<uint32_t>(plan.Find("wrap/inner")));

	tests::Wire(graph, "one", "value", "wrap", "nope");
	Plan refused;
	const CompileResult port = TryCompile(graph, registry, refused);
	REQUIRE(!port.Ok);
	CHECK(port.Diagnostics[0].Code == DiagnosticCode::SubgraphPort);

	auto loop = std::make_shared<Graph>();
	tests::Add(*loop, tests::Node("self", "sub.loop"));
	REQUIRE(registry.DefineSubgraph({.Id = "sub.loop", .Body = loop}).Ok);
	Graph outer;
	tests::Add(outer, tests::Node("l", "sub.loop"));
	const CompileResult recursion = TryCompile(outer, registry, refused);
	REQUIRE(!recursion.Ok);
	CHECK(recursion.Diagnostics[0].Code == DiagnosticCode::SubgraphRecursion);
}

TEST_CASE("limits come from the options and refuse before planning", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("one", "test.const"));
	tests::Add(graph, tests::Node("two", "test.const"));
	Plan plan;
	const CompileResult result = Compile(graph, registry, {.MaximumNodes = 1}, plan);
	REQUIRE(!result.Ok);
	CHECK(result.Diagnostics[0].GraphStatus == engine::graphcore::CompileStatus::LimitExceeded);
}

TEST_CASE("new links are checked for ports, room, types and cycles", "[ang][compile]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("one", "test.const"));
	tests::Add(graph, tests::Node("a", "test.add"));
	tests::Add(graph, tests::Node("b", "test.add"));
	tests::Add(graph, tests::Node("text", "test.text"));
	tests::Add(graph, tests::Node("int", "test.int"));
	tests::Wire(graph, "a", "sum", "b", "a");
	CHECK(CheckNewLink(graph, registry, {"one", "value", "a", "a"}).Ok);
	CHECK(!CheckNewLink(graph, registry, {"one", "value", "b", "a"}).Ok); // full
	CHECK(!CheckNewLink(graph, registry, {"b", "sum", "a", "b"}).Ok);	  // cycle
	CHECK(!CheckNewLink(graph, registry, {"a", "sum", "a", "b"}).Ok);	  // self
	CHECK(!CheckNewLink(graph, registry, {"text", "text", "a", "b"}).Ok); // type
	CHECK(!CheckNewLink(graph, registry, {"one", "nope", "a", "b"}).Ok);  // port
	CHECK(!CheckNewLink(graph, registry, {"a", "sum", "b", "a"}).Ok);	  // duplicate
	const LinkVerdict converts = CheckNewLink(graph, registry, {"int", "value", "a", "b"});
	CHECK(converts.Ok);
	CHECK(converts.Converts);
}

TEST_CASE("cycle detection follows links only", "[ang][compile]") {
	Graph graph;
	tests::Add(graph, tests::Node("a", "anything"));
	tests::Add(graph, tests::Node("b", "anything"));
	tests::Add(graph, tests::Node("c", "anything"));
	tests::Wire(graph, "a", "out", "b", "in");
	tests::Wire(graph, "b", "out", "c", "in");
	CHECK(WouldCycle(graph, {"c", "out", "a", "in"}));
	CHECK(WouldCycle(graph, {"a", "out", "a", "in"}));
	CHECK(!WouldCycle(graph, {"a", "out", "c", "other"}));
}
