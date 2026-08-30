#include <engine/graph/EngineGraph.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("the authored render graph imports into the unified final stage", "[graph][engine]") {
	RenderGraph render;
	Name offender;
	REQUIRE(Build(DefaultPbrTierBDocument(), render, offender) == PipelineDocumentStatus::Ok);
	EngineGraph engine;
	REQUIRE(AppendRenderStage(render, engine, offender) == EngineGraphStatus::Ok);
	CHECK(engine.NodeCount() == render.Count());
	for (const EngineNode &node : engine.AllNodes()) {
		CHECK(node.Stage == EngineStage::Render);
		CHECK(node.Name.Text().starts_with("render."));
	}

	EngineSchedule schedule;
	CHECK(CompileEngineGraph(engine, schedule, offender) == EngineGraphStatus::Ok);
	CHECK_FALSE(schedule.Waves.empty());
}
