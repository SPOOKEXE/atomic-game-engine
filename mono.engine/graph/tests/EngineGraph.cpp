#include <engine/graph/EngineGraph.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.enginegraph")
TEST_DEPENDS("engine.graph.schedule")

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

TEST_CASE("engine graph cycle refuses and clears a reused schedule", "[graph][engine]") {
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

	EngineSchedule schedule;
	schedule.Waves.push_back({});
	schedule.Barriers.push_back({});
	schedule.Resources.push_back({});
	Name offender;
	CHECK(CompileEngineGraph(graph, schedule, offender) == EngineGraphStatus::Cycle);
	CHECK(offender == Name("blocked-reader"));
	CHECK(schedule.Waves.empty());
	CHECK(schedule.Barriers.empty());
	CHECK(schedule.Resources.empty());
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
