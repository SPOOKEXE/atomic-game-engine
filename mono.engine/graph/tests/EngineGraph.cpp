#include <engine/graph/EngineGraph.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.enginegraph")
TEST_DEPENDS("engine.graph.schedule")
TEST_TIER("fast")

using namespace engine::graph;
using engine::core::Name;

namespace {
	EngineNode
	MakeNode(const char *name, EngineStage stage, std::vector<Name> reads, std::vector<Name> writes) {
		return EngineNode{
			.Name = Name(name),
			.Stage = stage,
			.Reads = std::move(reads),
			.Writes = std::move(writes),
		};
	}

	EngineGraph AcceptedGraph() {
		EngineGraph graph;
		REQUIRE(graph.AddResource({Name("accepted-state"), EngineResourceLifetime::World}));
		REQUIRE(graph.AddResource({Name("accepted-image"), EngineResourceLifetime::Persistent}));
		REQUIRE(graph.AddNode(MakeNode("accepted-input", EngineStage::Input, {}, {Name("accepted-state")}))
					.IsValid());
		auto compute = MakeNode(
			"accepted-compute", EngineStage::Animation, {Name("accepted-state")}, {Name("accepted-image")}
		);
		compute.Queue = ExecutionQueue::Compute;
		compute.AsyncEligible = true;
		compute.Profile = false;
		REQUIRE(graph.AddNode(std::move(compute)).IsValid());
		auto render = MakeNode("accepted-render", EngineStage::Render, {Name("accepted-image")}, {});
		render.Queue = ExecutionQueue::Graphics;
		REQUIRE(graph.AddNode(std::move(render)).IsValid());
		return graph;
	}

	void CheckRetained(const EngineSchedule &actual, const EngineSchedule &accepted) {
		REQUIRE(actual.Waves.size() == accepted.Waves.size());
		for (size_t wave = 0; wave < accepted.Waves.size(); wave++) {
			CHECK(actual.Waves[wave].Concurrent == accepted.Waves[wave].Concurrent);
			REQUIRE(actual.Waves[wave].Nodes.size() == accepted.Waves[wave].Nodes.size());
			for (size_t node = 0; node < accepted.Waves[wave].Nodes.size(); node++) {
				const auto &held = actual.Waves[wave].Nodes[node];
				const auto &expected = accepted.Waves[wave].Nodes[node];
				CHECK(held.Node == expected.Node);
				CHECK(held.Queue == expected.Queue);
				CHECK(held.AsyncEligible == expected.AsyncEligible);
				CHECK(held.Profile == expected.Profile);
			}
		}
		REQUIRE(actual.Barriers.size() == accepted.Barriers.size());
		for (size_t index = 0; index < accepted.Barriers.size(); index++) {
			const auto &held = actual.Barriers[index];
			const auto &expected = accepted.Barriers[index];
			CHECK(held.Resource == expected.Resource);
			CHECK(held.From == expected.From);
			CHECK(held.To == expected.To);
			CHECK(held.ProducerWave == expected.ProducerWave);
			CHECK(held.ConsumerWave == expected.ConsumerWave);
		}
		REQUIRE(actual.Resources.size() == accepted.Resources.size());
		for (size_t index = 0; index < accepted.Resources.size(); index++) {
			const auto &held = actual.Resources[index];
			const auto &expected = accepted.Resources[index];
			CHECK(held.Resource == expected.Resource);
			CHECK(held.Lifetime == expected.Lifetime);
			CHECK(held.FirstWave == expected.FirstWave);
			CHECK(held.LastWave == expected.LastWave);
		}
	}

	RenderGraph ImportedGraph() {
		RenderGraph graph;
		const auto first = graph.AddResource({.Name = Name("import-first"), .Kind = ResourceKind::Storage});
		const auto second = graph.AddResource({.Name = Name("import-second"), .Kind = ResourceKind::Storage});
		REQUIRE(graph.AddNode({.Name = Name("first"), .Kind = Name("compute"), .Writes = {first}}).IsValid());
		REQUIRE(
			graph
				.AddNode(
					{.Name = Name("second"), .Kind = Name("compute"), .Reads = {first}, .Writes = {second}}
				)
				.IsValid()
		);
		return graph;
	}

	void CheckGraphRetained(const EngineGraph &actual, const EngineGraph &accepted) {
		REQUIRE(actual.NodeCount() == accepted.NodeCount());
		for (size_t index = 0; index < accepted.NodeCount(); index++) {
			const auto &held = actual.AllNodes()[index];
			const auto &expected = accepted.AllNodes()[index];
			CHECK(held.Name == expected.Name);
			CHECK(held.Stage == expected.Stage);
			CHECK(held.Queue == expected.Queue);
			CHECK(held.Reads == expected.Reads);
			CHECK(held.Writes == expected.Writes);
			CHECK(held.AsyncEligible == expected.AsyncEligible);
			CHECK(held.Profile == expected.Profile);
		}
		REQUIRE(actual.Resources().size() == accepted.Resources().size());
		for (size_t index = 0; index < accepted.Resources().size(); index++) {
			const auto &held = actual.Resources()[index];
			const auto &expected = accepted.Resources()[index];
			CHECK(held.Name == expected.Name);
			CHECK(held.Lifetime == expected.Lifetime);
			CHECK(held.External == expected.External);
		}
	}

	class Recorder final : public EngineNodeRunner {
	  public:
		bool BeginWave(uint32_t, bool, std::span<const EngineBarrier>) override {
			return true;
		}

		bool Run(const EngineRunContext &context) override {
			Ran.emplace_back(context.Work.Name.Text());
			return true;
		}

		bool EndWave(uint32_t) override {
			return true;
		}

		std::vector<std::string> Ran;
	};
}

TEST_CASE("the six engine graphs compile in their fixed order", "[graph][engine]") {
	EngineGraph graph;
	for (const EngineStage stage : {
			 EngineStage::Input,
			 EngineStage::Ai,
			 EngineStage::World,
			 EngineStage::Physics,
			 EngineStage::Animation,
			 EngineStage::Render,
		 }) {
		REQUIRE(graph.AddNode(MakeNode(Describe(stage), stage, {}, {})).IsValid());
	}

	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(graph, schedule, offender) == EngineGraphStatus::Ok);
	REQUIRE(schedule.Waves.size() == 6);
	for (uint32_t wave = 0; wave < schedule.Waves.size(); wave++) {
		REQUIRE(schedule.Waves[wave].Nodes.size() == 1);
		CHECK(schedule.Waves[wave].Nodes.front().Node.Value == wave + 1);
	}
}

TEST_CASE("independent work shares a wave and queue handovers are explicit", "[graph][engine]") {
	EngineGraph graph;
	REQUIRE(graph.AddResource({Name("state"), EngineResourceLifetime::World}));
	REQUIRE(graph.AddResource({Name("image"), EngineResourceLifetime::Tick}));
	EngineNode input = MakeNode("input", EngineStage::Input, {}, {Name("state")});
	input.Queue = ExecutionQueue::Cpu;
	REQUIRE(graph.AddNode(std::move(input)).IsValid());
	EngineNode animation = MakeNode("animation", EngineStage::Animation, {Name("state")}, {Name("image")});
	animation.Queue = ExecutionQueue::Compute;
	animation.AsyncEligible = true;
	REQUIRE(graph.AddNode(std::move(animation)).IsValid());
	EngineNode render = MakeNode("render", EngineStage::Render, {Name("image")}, {});
	render.Queue = ExecutionQueue::Graphics;
	REQUIRE(graph.AddNode(std::move(render)).IsValid());

	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(graph, schedule, offender) == EngineGraphStatus::Ok);
	REQUIRE(schedule.Barriers.size() == 2);
	CHECK(schedule.Barriers[0].Resource == Name("state"));
	CHECK(schedule.Barriers[1].Resource == Name("image"));
	REQUIRE(schedule.Resources.size() == 2);
	CHECK(schedule.Resources[0].Lifetime == EngineResourceLifetime::World);
	CHECK(schedule.Resources[0].FirstWave < schedule.Resources[0].LastWave);

	Recorder recorder;
	REQUIRE(ExecuteEngineGraph(graph, schedule, recorder));
	CHECK(recorder.Ran == std::vector<std::string>{"input", "animation", "render"});
}

TEST_CASE("ready work is concurrent or stably serial by execution mode", "[graph][engine]") {
	EngineGraph graph;
	EngineNode cpu = MakeNode("cpu", EngineStage::World, {}, {});
	cpu.Queue = ExecutionQueue::Cpu;
	REQUIRE(graph.AddNode(std::move(cpu)).IsValid());
	EngineNode compute = MakeNode("compute", EngineStage::World, {}, {});
	compute.Queue = ExecutionQueue::Compute;
	compute.AsyncEligible = true;
	REQUIRE(graph.AddNode(std::move(compute)).IsValid());

	EngineSchedule concurrent;
	Name offender;
	REQUIRE(
		CompileEngineGraph(graph, concurrent, offender, EngineExecutionMode::Concurrent) ==
		EngineGraphStatus::Ok
	);
	REQUIRE(concurrent.Waves.size() == 1);
	REQUIRE(concurrent.Waves.front().Nodes.size() == 2);
	CHECK(concurrent.Waves.front().Concurrent);

	EngineSchedule deterministic;
	REQUIRE(
		CompileEngineGraph(graph, deterministic, offender, EngineExecutionMode::Deterministic) ==
		EngineGraphStatus::Ok
	);
	REQUIRE(deterministic.Waves.size() == 2);
	CHECK_FALSE(deterministic.Waves[0].Concurrent);
	CHECK(deterministic.Waves[0].Nodes.front().Node.Value == 1);
	CHECK(deterministic.Waves[1].Nodes.front().Node.Value == 2);
}

TEST_CASE("engine graph serial order revisits newly ready authored nodes", "[graph][engine]") {
	EngineGraph graph;
	REQUIRE(graph.AddResource({Name("first"), EngineResourceLifetime::Tick}));
	REQUIRE(graph.AddResource({Name("second"), EngineResourceLifetime::Tick}));
	REQUIRE(graph.AddNode(MakeNode("first-reader", EngineStage::World, {Name("first")}, {})).IsValid());
	REQUIRE(graph.AddNode(MakeNode("second-reader", EngineStage::World, {Name("second")}, {})).IsValid());
	REQUIRE(graph.AddNode(MakeNode("independent", EngineStage::World, {}, {})).IsValid());
	REQUIRE(graph.AddNode(MakeNode("first-writer", EngineStage::World, {}, {Name("first")})).IsValid());
	REQUIRE(graph.AddNode(MakeNode("second-writer", EngineStage::World, {}, {Name("second")})).IsValid());

	Name offender;
	EngineSchedule serial;
	REQUIRE(
		CompileEngineGraph(graph, serial, offender, EngineExecutionMode::Deterministic) ==
		EngineGraphStatus::Ok
	);
	REQUIRE(serial.Waves.size() == 5);
	for (const EngineWave &wave : serial.Waves) {
		REQUIRE(wave.Nodes.size() == 1);
		CHECK_FALSE(wave.Concurrent);
	}
	Recorder serialRecorder;
	REQUIRE(ExecuteEngineGraph(graph, serial, serialRecorder));
	CHECK(
		serialRecorder.Ran ==
		std::vector<std::string>{
			"independent", "first-writer", "first-reader", "second-writer", "second-reader"
		}
	);

	EngineSchedule concurrent;
	REQUIRE(
		CompileEngineGraph(graph, concurrent, offender, EngineExecutionMode::Concurrent) ==
		EngineGraphStatus::Ok
	);
	REQUIRE(concurrent.Waves.size() == 2);
	REQUIRE(concurrent.Waves[0].Nodes.size() == 3);
	REQUIRE(concurrent.Waves[1].Nodes.size() == 2);
	CHECK(concurrent.Waves[0].Nodes[0].Node.Value == 3);
	CHECK(concurrent.Waves[0].Nodes[1].Node.Value == 4);
	CHECK(concurrent.Waves[0].Nodes[2].Node.Value == 5);
	CHECK(concurrent.Waves[1].Nodes[0].Node.Value == 1);
	CHECK(concurrent.Waves[1].Nodes[1].Node.Value == 2);
}

TEST_CASE("engine graph cycle preserves a reusable accepted schedule", "[graph][engine]") {
	EngineGraph graph;
	REQUIRE(graph.AddResource({Name("output"), EngineResourceLifetime::Tick}));
	REQUIRE(graph.AddResource({Name("loop-a"), EngineResourceLifetime::Tick}));
	REQUIRE(graph.AddResource({Name("loop-b"), EngineResourceLifetime::Tick}));
	REQUIRE(graph.AddNode(MakeNode("blocked-reader", EngineStage::World, {Name("output")}, {})).IsValid());
	REQUIRE(graph
				.AddNode(MakeNode(
					"first-cycle-node", EngineStage::World, {Name("loop-b")}, {Name("output"), Name("loop-a")}
				))
				.IsValid());
	REQUIRE(
		graph.AddNode(MakeNode("second-cycle-node", EngineStage::World, {Name("loop-a")}, {Name("loop-b")}))
			.IsValid()
	);

	const auto acceptedGraph = AcceptedGraph();
	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(acceptedGraph, schedule, offender) == EngineGraphStatus::Ok);
	REQUIRE_FALSE(schedule.Waves.empty());
	REQUIRE_FALSE(schedule.Barriers.empty());
	REQUIRE_FALSE(schedule.Resources.empty());
	const auto accepted = schedule;
	CHECK(CompileEngineGraph(graph, schedule, offender) == EngineGraphStatus::Cycle);
	CHECK(offender == Name("blocked-reader"));
	CheckRetained(schedule, accepted);
	Recorder recorder;
	REQUIRE(ExecuteEngineGraph(acceptedGraph, schedule, recorder));
	CHECK(recorder.Ran == std::vector<std::string>{"accepted-input", "accepted-compute", "accepted-render"});
}

TEST_CASE("engine graph dependency refusals have distinct descriptions", "[graph][engine]") {
	CHECK(
		std::string(Describe(EngineGraphStatus::LimitExceeded)) ==
		"the engine graph exceeds dependency compilation limits"
	);
	CHECK(
		std::string(Describe(EngineGraphStatus::InvalidDependency)) ==
		"the engine graph has an invalid dependency"
	);
	CHECK(
		std::string(Describe(EngineGraphStatus::AllocationFailed)) ==
		"engine dependency compilation could not allocate storage"
	);
}

TEST_CASE("the authored render graph imports into the unified final stage", "[graph][engine]") {
	RenderGraph render;
	Name offender;
	REQUIRE(Build(DefaultPbrTierBDocument(), render, offender) == PipelineDocumentStatus::Ok);
	EngineGraph engine;
	REQUIRE(AppendRenderStage(render, engine, offender) == EngineGraphStatus::Ok);
	size_t enabled = 0;
	for (uint32_t value = 1; value <= render.Count(); value++) {
		const Node *node = render.Find(NodeId{value});
		REQUIRE(node != nullptr);
		if (!node->Enabled) {
			CHECK(node->Name == Name("depth-peel"));
			continue;
		}
		enabled++;
	}
	CHECK(engine.NodeCount() == enabled);
	for (const EngineNode &node : engine.AllNodes()) {
		CHECK(node.Stage == EngineStage::Render);
		CHECK(node.Name.Text().starts_with("render."));
	}
	CHECK(std::none_of(engine.AllNodes().begin(), engine.AllNodes().end(), [](const EngineNode &node) {
		return node.Name == Name("render.depth-peel");
	}));

	EngineSchedule schedule;
	CHECK(CompileEngineGraph(engine, schedule, offender) == EngineGraphStatus::Ok);
	CHECK_FALSE(schedule.Waves.empty());
}

TEST_CASE("engine graph validation refusals leave every accepted plan row usable", "[graph][engine]") {
	const auto acceptedGraph = AcceptedGraph();
	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(acceptedGraph, schedule, offender) == EngineGraphStatus::Ok);
	const auto accepted = schedule;
	EngineGraph refused;
	EngineGraphStatus expected = EngineGraphStatus::UnknownResource;
	Name expectedOffender("unknown");
	SECTION("unknown resource") {
		REQUIRE(
			refused.AddNode(MakeNode("unknown-reader", EngineStage::World, {Name("unknown")}, {})).IsValid()
		);
	}
	SECTION("invalid resource name") {
		REQUIRE(refused.AddNode(MakeNode("invalid-reader", EngineStage::World, {Name{}}, {})).IsValid());
		expected = EngineGraphStatus::InvalidName;
		expectedOffender = Name("invalid-reader");
	}
	SECTION("missing producer") {
		REQUIRE(refused.AddResource({Name("missing")}));
		REQUIRE(
			refused.AddNode(MakeNode("missing-reader", EngineStage::World, {Name("missing")}, {})).IsValid()
		);
		expected = EngineGraphStatus::MissingProducer;
		expectedOffender = Name("missing");
	}
	SECTION("stage regression") {
		REQUIRE(refused.AddResource({Name("late-output")}));
		REQUIRE(
			refused.AddNode(MakeNode("early-reader", EngineStage::Input, {Name("late-output")}, {})).IsValid()
		);
		REQUIRE(
			refused.AddNode(MakeNode("late-writer", EngineStage::Render, {}, {Name("late-output")})).IsValid()
		);
		expected = EngineGraphStatus::StageRegression;
		expectedOffender = Name("early-reader");
	}
	CHECK(CompileEngineGraph(refused, schedule, offender) == expected);
	CHECK(offender == expectedOffender);
	CheckRetained(schedule, accepted);
	Recorder recorder;
	REQUIRE(ExecuteEngineGraph(acceptedGraph, schedule, recorder));
	CHECK(recorder.Ran == std::vector<std::string>{"accepted-input", "accepted-compute", "accepted-render"});
}

TEST_CASE("engine graph admission bounds nodes resources and each resource-use list", "[graph][engine]") {
	EngineGraph nodes;
	for (size_t index = 0; index < EngineGraph::MAXIMUM_NODES; index++) {
		EngineNode node;
		node.Name = Name("bounded-node-" + std::to_string(index));
		REQUIRE(nodes.AddNode(std::move(node)).IsValid());
	}
	const auto lastNode = nodes.Find(EngineNodeId{EngineGraph::MAXIMUM_NODES})->Name;
	CHECK_FALSE(nodes.AddNode(MakeNode("one-too-many", EngineStage::World, {}, {})).IsValid());
	CHECK(nodes.NodeCount() == EngineGraph::MAXIMUM_NODES);
	CHECK(nodes.Find(EngineNodeId{EngineGraph::MAXIMUM_NODES})->Name == lastNode);
	EngineSchedule schedule;
	Name offender;
	CHECK(CompileEngineGraph(nodes, schedule, offender) == EngineGraphStatus::Ok);
	REQUIRE(schedule.Waves.size() == 1);
	CHECK(schedule.Waves[0].Nodes.size() == EngineGraph::MAXIMUM_NODES);

	EngineGraph resources;
	std::vector<Name> uses;
	for (size_t index = 0; index < EngineGraph::MAXIMUM_RESOURCES; index++) {
		const Name resource("bounded-resource-" + std::to_string(index));
		REQUIRE(resources.AddResource({resource, EngineResourceLifetime::Tick, true}));
		uses.push_back(resource);
	}
	CHECK_FALSE(resources.AddResource({Name("one-too-many-resource")}));
	CHECK(resources.Resources().size() == EngineGraph::MAXIMUM_RESOURCES);
	CHECK(resources.AddNode(MakeNode("at-read-cap", EngineStage::World, uses, {})).IsValid());
	auto duplicated = uses;
	duplicated.push_back(uses.front());
	CHECK_FALSE(resources.AddNode(MakeNode("over-read-cap", EngineStage::World, duplicated, {})).IsValid());
	CHECK_FALSE(resources.AddNode(MakeNode("over-write-cap", EngineStage::World, {}, duplicated)).IsValid());
	CHECK(resources.NodeCount() == 1);
	CHECK(CompileEngineGraph(resources, schedule, offender) == EngineGraphStatus::Ok);
	CHECK(schedule.Resources.size() == EngineGraph::MAXIMUM_RESOURCES);

	EngineGraph duplicates;
	REQUIRE(duplicates.AddResource({Name("repeated"), EngineResourceLifetime::Tick, true}));
	std::vector<Name> repeated(EngineGraph::MAXIMUM_RESOURCE_USES, Name("repeated"));
	REQUIRE(duplicates.AddNode(MakeNode("duplicate-uses-at-cap", EngineStage::World, repeated, repeated))
				.IsValid());
	CHECK(CompileEngineGraph(duplicates, schedule, offender) == EngineGraphStatus::Ok);
	CHECK(schedule.Resources.size() == 1);
}

TEST_CASE(
	"dense queue handovers refuse their bound without replacing an accepted engine plan", "[graph][engine]"
) {
	const auto acceptedGraph = AcceptedGraph();
	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(acceptedGraph, schedule, offender) == EngineGraphStatus::Ok);
	const auto accepted = schedule;
	EngineGraph dense;
	std::vector<Name> resources;
	for (size_t index = 0; index < EngineGraph::MAXIMUM_RESOURCES; index++) {
		const Name name("dense-resource-" + std::to_string(index));
		REQUIRE(dense.AddResource({name}));
		resources.push_back(name);
	}
	// The last of 127 CPU writers supplies 128 complete compute readers:
	// 128 * 512 resource handovers exactly fills the accepted barrier budget.
	constexpr size_t WRITERS = 127;
	for (size_t index = 0; index < EngineGraph::MAXIMUM_NODES - 1; index++) {
		EngineNode node;
		node.Name = Name("dense-node-" + std::to_string(index));
		if (index < WRITERS) {
			node.Writes = resources;
		} else {
			node.Reads = resources;
			node.Queue = ExecutionQueue::Compute;
		}
		REQUIRE(dense.AddNode(std::move(node)).IsValid());
	}
	EngineSchedule atCapacity;
	REQUIRE(CompileEngineGraph(dense, atCapacity, offender) == EngineGraphStatus::Ok);
	CHECK(atCapacity.Barriers.size() == EngineGraph::MAXIMUM_BARRIERS);
	// One final reader adds exactly one handover at the final node slot.
	EngineNode oneOver = MakeNode("dense-one-over", EngineStage::World, {resources.front()}, {});
	oneOver.Queue = ExecutionQueue::Compute;
	REQUIRE(dense.AddNode(std::move(oneOver)).IsValid());
	CHECK(dense.NodeCount() == EngineGraph::MAXIMUM_NODES);
	CHECK(CompileEngineGraph(dense, schedule, offender) == EngineGraphStatus::LimitExceeded);
	CHECK(dense.FindResourceDesc(offender) != nullptr);
	CheckRetained(schedule, accepted);
	Recorder recorder;
	REQUIRE(ExecuteEngineGraph(acceptedGraph, schedule, recorder));
	CHECK(recorder.Ran == std::vector<std::string>{"accepted-input", "accepted-compute", "accepted-render"});
}

TEST_CASE("render import refusals preserve every accepted destination row", "[graph][engine]") {
	auto destination = AcceptedGraph();
	const auto imported = ImportedGraph();
	EngineGraphStatus expected = EngineGraphStatus::InvalidName;
	SECTION("late duplicate node") {
		REQUIRE(destination.AddNode(MakeNode("render.second", EngineStage::Render, {}, {})).IsValid());
	}
	SECTION("late node capacity") {
		while (destination.NodeCount() < EngineGraph::MAXIMUM_NODES - 1) {
			EngineNode node;
			node.Name = Name("held-node-" + std::to_string(destination.NodeCount()));
			node.Stage = EngineStage::Render;
			REQUIRE(destination.AddNode(std::move(node)).IsValid());
		}
		expected = EngineGraphStatus::LimitExceeded;
	}
	SECTION("late resource capacity") {
		while (destination.Resources().size() < EngineGraph::MAXIMUM_RESOURCES - 1) {
			REQUIRE(destination.AddResource(
				{Name("held-resource-" + std::to_string(destination.Resources().size()))}
			));
		}
		expected = EngineGraphStatus::LimitExceeded;
	}
	const auto accepted = destination;
	EngineSchedule schedule;
	Name offender;
	REQUIRE(CompileEngineGraph(destination, schedule, offender) == EngineGraphStatus::Ok);
	Recorder before;
	REQUIRE(ExecuteEngineGraph(destination, schedule, before));
	CHECK(AppendRenderStage(imported, destination, offender) == expected);
	if (expected == EngineGraphStatus::InvalidName) CHECK(offender == Name("render.second"));
	CheckGraphRetained(destination, accepted);
	Recorder after;
	REQUIRE(ExecuteEngineGraph(destination, schedule, after));
	CHECK(after.Ran == before.Ran);
}

TEST_CASE("render import scheduling diagnostics retain their actual cause", "[graph][engine]") {
	auto destination = AcceptedGraph();
	const auto accepted = destination;
	RenderGraph imported;
	const auto storage = imported.AddResource({.Name = Name("storage"), .Kind = ResourceKind::Storage});
	Node invalid;
	invalid.Name = Name("invalid-dispatch");
	invalid.Kind = Name("compute");
	invalid.Writes = {storage};
	invalid.Parameters = {{Name("dispatch.x"), "zero"}};
	REQUIRE(imported.AddNode(std::move(invalid)).IsValid());
	Name offender;
	CHECK(AppendRenderStage(imported, destination, offender) == EngineGraphStatus::InvalidDependency);
	CHECK(offender == Name("invalid-dispatch"));
	CheckGraphRetained(destination, accepted);
}

TEST_CASE(
	"render import admits its qualified identity byte budget and refuses one byte over", "[graph][engine]"
) {
	const auto imported = ImportedGraph();
	size_t authoredBytes = 0;
	for (uint32_t value = 1; value <= imported.ResourceCount(); value++) {
		authoredBytes += imported.FindResourceDesc(ResourceId{value})->Name.Text().size();
	}
	for (uint32_t value = 1; value <= imported.Count(); value++) {
		authoredBytes += imported.Find(NodeId{value})->Name.Text().size();
	}
	const size_t identities = imported.ResourceCount() + imported.Count();
	const size_t prefixBytes = EngineGraph::MAXIMUM_IMPORT_IDENTITY_BYTES - authoredBytes;
	REQUIRE(prefixBytes % identities == 0);
	const std::string prefix(prefixBytes / identities, 'p');
	auto accepted = AcceptedGraph();
	const auto priorNodes = accepted.NodeCount();
	const auto priorResources = accepted.Resources().size();
	Name offender;
	REQUIRE(AppendRenderStage(imported, accepted, offender, prefix) == EngineGraphStatus::Ok);
	CHECK(accepted.NodeCount() == priorNodes + imported.Count());
	CHECK(accepted.Resources().size() == priorResources + imported.ResourceCount());
	EngineSchedule schedule;
	CHECK(CompileEngineGraph(accepted, schedule, offender) == EngineGraphStatus::Ok);

	RenderGraph oneOver;
	const auto first = oneOver.AddResource({.Name = Name("import-first"), .Kind = ResourceKind::Storage});
	const auto second = oneOver.AddResource({.Name = Name("import-second"), .Kind = ResourceKind::Storage});
	REQUIRE(oneOver.AddNode({.Name = Name("first"), .Kind = Name("compute"), .Writes = {first}}).IsValid());
	REQUIRE(
		oneOver
			.AddNode({.Name = Name("secondx"), .Kind = Name("compute"), .Reads = {first}, .Writes = {second}})
			.IsValid()
	);
	auto destination = AcceptedGraph();
	const auto retained = destination;
	CHECK(AppendRenderStage(oneOver, destination, offender, prefix) == EngineGraphStatus::LimitExceeded);
	CHECK(offender == Name("secondx"));
	CheckGraphRetained(destination, retained);
}
