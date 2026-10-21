#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderStage.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.renderstage")
TEST_DEPENDS("engine.graph.enginegraph")
TEST_DEPENDS("engine.graph.rendergraph")
TEST_DEPENDS("engine.graph.schedule")

using engine::core::Name;
using namespace engine::graph;

namespace {
	struct Recorder final : NodeRunner {
		std::vector<std::string> Names;
		Name Refuse;
		bool Run(const RunContext &context) override {
			Names.emplace_back(context.Name.Text());
			return context.Name != Refuse;
		}
	};

	NodeId AddWorldNode(RenderGraph &graph, Node node) {
		node.Scope = NodeScope::World;
		return graph.AddNode(std::move(node));
	}

	NodeId IndependentNode(RenderGraph &graph, const char *name, NodeScope scope) {
		const auto resource =
			graph.AddResource({.Name = Name(std::string(name) + "-value"), .Kind = ResourceKind::Storage});
		REQUIRE(resource.IsValid());
		return graph.AddNode({.Name = Name(name), .Writes = {resource}, .Scope = scope});
	}

	RenderGraph VersionedGraph() {
		RenderGraph graph;
		const auto first = graph.AddResource({.Name = Name("import-first"), .Kind = ResourceKind::Storage});
		const auto second = graph.AddResource({.Name = Name("import-second"), .Kind = ResourceKind::Storage});
		REQUIRE(
			AddWorldNode(graph, {.Name = Name("first"), .Kind = Name("compute"), .Writes = {first}}).IsValid()
		);
		REQUIRE(AddWorldNode(
					graph,
					{.Name = Name("second"), .Kind = Name("compute"), .Reads = {first}, .Writes = {second}}
		).IsValid());
		return graph;
	}

	CompiledGraph Compile(const RenderGraph &graph) {
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		return compiled;
	}

	std::vector<NodeId> OriginalNodes(const RenderStageProjection &projection, const EngineWave &wave) {
		std::vector<NodeId> nodes;
		for (const auto &entry : wave.Nodes)
			nodes.push_back(projection.RenderNodes.at(entry.Node.Value - 1));
		return nodes;
	}
}

TEST_CASE("render projection retains both authored and scheduled ordering contracts", "[graph][engine]") {
	RenderGraph graph;
	const auto a = graph.AddResource({.Name = Name("a"), .Kind = ResourceKind::Storage});
	const auto b = graph.AddResource({.Name = Name("b"), .Kind = ResourceKind::Storage});
	const auto first = AddWorldNode(graph, {.Name = Name("first"), .Kind = Name("compute"), .Writes = {a}});
	const auto dependent =
		AddWorldNode(graph, {.Name = Name("dependent"), .Kind = Name("compute"), .Reads = {a}});
	const auto independent =
		AddWorldNode(graph, {.Name = Name("independent"), .Kind = Name("compute"), .Writes = {b}});
	REQUIRE(first.IsValid());
	REQUIRE(dependent.IsValid());
	REQUIRE(independent.IsValid());
	const auto compiled = Compile(graph);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection authored, scheduled;
	REQUIRE(ProjectRenderStage(graph, compiled, authored, offender) == EngineGraphStatus::Ok);
	REQUIRE(ProjectRenderStage(graph, compiled, schedule, scheduled, offender) == EngineGraphStatus::Ok);
	Recorder original, domain;
	REQUIRE(ExecuteRenderStage(graph, authored, original, size_t{0}));
	REQUIRE(ExecuteRenderStage(graph, scheduled, domain, size_t{0}));
	CHECK(original.Names == std::vector<std::string>{"first", "dependent", "independent"});
	CHECK(domain.Names == std::vector<std::string>{"first", "independent", "dependent"});
	REQUIRE(scheduled.Blocks[0].Waves.size() == schedule.Waves.size());
	for (size_t index = 0; index < schedule.Waves.size(); index++) {
		const auto &wave = scheduled.Blocks[0].Waves[index];
		CHECK(wave.Concurrent == schedule.Waves[index].Concurrent);
		REQUIRE(wave.Nodes.size() == schedule.Waves[index].Nodes.size());
		for (size_t item = 0; item < wave.Nodes.size(); item++) {
			CHECK(
				scheduled.RenderNodes[wave.Nodes[item].Node.Value - 1] ==
				schedule.Waves[index].Nodes[item].Node
			);
			CHECK(wave.Nodes[item].Queue == schedule.Waves[index].Nodes[item].Queue);
			CHECK(wave.Nodes[item].AsyncEligible == schedule.Waves[index].Nodes[item].AsyncEligible);
		}
	}
	for (const auto &wave : authored.Blocks[0].Waves) {
		REQUIRE(wave.Nodes.size() == 1);
		CHECK_FALSE(wave.Concurrent);
		CHECK(wave.Nodes.front().Queue == ExecutionQueue::Cpu);
		CHECK_FALSE(wave.Nodes.front().AsyncEligible);
	}
	CHECK(scheduled.Graph.Resources().size() == 2);
	CHECK(scheduled.Graph.Find(EngineNodeId{2})->Reads == std::vector<Name>{Name("render.a")});
	CHECK(scheduled.Blocks[0].Barriers.empty());
}

TEST_CASE("scheduled projection filters exact domain waves into native scope blocks", "[graph][engine]") {
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(DefaultPbrTierBDocument(), graph, offender) == PipelineDocumentStatus::Ok);
	const auto compiled = Compile(graph);
	ExecutionSchedule schedule;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, schedule, projection, offender) == EngineGraphStatus::Ok);
	const std::array<const std::vector<NodeId> *, 3> scopes{
		&compiled.Shared, &compiled.PerView, &compiled.Final
	};
	for (size_t block = 0; block < scopes.size(); block++) {
		size_t index = 0;
		for (const auto &wave : schedule.Waves) {
			std::vector<NodeId> expected;
			for (const auto &entry : wave.Nodes)
				if (std::find(scopes[block]->begin(), scopes[block]->end(), entry.Node) !=
					scopes[block]->end())
					expected.push_back(entry.Node);
			if (expected.empty()) continue;
			REQUIRE(index < projection.Blocks[block].Waves.size());
			CHECK(OriginalNodes(projection, projection.Blocks[block].Waves[index]) == expected);
			CHECK(projection.Blocks[block].Waves[index].Concurrent == wave.Concurrent);
			const auto &projected = projection.Blocks[block].Waves[index];
			size_t item = 0;
			for (const auto &entry : wave.Nodes) {
				if (std::find(expected.begin(), expected.end(), entry.Node) == expected.end()) continue;
				CHECK(projected.Nodes[item].Queue == entry.Queue);
				CHECK(projected.Nodes[item].AsyncEligible == entry.AsyncEligible);
				const auto *source = graph.Find(entry.Node);
				const auto *profile = source->Parameter(Name("profile"));
				CHECK(projected.Nodes[item].Profile == (profile == nullptr || *profile != "false"));
				item++;
			}
			index++;
		}
		CHECK(index == projection.Blocks[block].Waves.size());
	}
	size_t enabled = 0;
	for (uint32_t value = 1; value <= graph.Count(); value++)
		if (graph.Find(NodeId{value})->Enabled) enabled++;
	CHECK(projection.RenderNodes.size() == enabled);
	CHECK(projection.Graph.NodeCount() == enabled);
}

TEST_CASE(
	"projection refuses incomplete or duplicate mappings without replacing accepted work", "[graph][engine]"
) {
	const auto graph = VersionedGraph();
	const auto accepted = Compile(graph);
	auto compiled = accepted;
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, accepted, schedule, projection, offender) == EngineGraphStatus::Ok);
	SECTION("missing compiled node") {
		compiled.Shared.pop_back();
	}
	SECTION("duplicate compiled node") {
		compiled.Shared.push_back(compiled.Shared.front());
	}
	SECTION("wrong scope") {
		compiled.PerView.push_back(compiled.Shared.back());
		compiled.Shared.pop_back();
	}
	SECTION("unknown compiled node") {
		compiled.Shared.back() = NodeId{999};
	}
	SECTION("missing scheduled node") {
		schedule.Waves.back().Nodes.clear();
	}
	SECTION("duplicate scheduled node") {
		schedule.Waves.back().Nodes.push_back(schedule.Waves.front().Nodes.front());
	}
	SECTION("unknown scheduled node") {
		schedule.Waves.back().Nodes.front().Node = NodeId{999};
	}
	CHECK(
		ProjectRenderStage(graph, compiled, schedule, projection, offender) ==
		EngineGraphStatus::InvalidDependency
	);
	CHECK(projection.RenderNodes == std::vector<NodeId>{NodeId{1}, NodeId{2}});
	Recorder recorder;
	REQUIRE(ExecuteRenderStage(graph, projection, recorder, size_t{0}));
	CHECK(recorder.Names == std::vector<std::string>{"first", "second"});
}

TEST_CASE(
	"projection preserves multiwriter and external history order without dependency reinference",
	"[graph][engine]"
) {
	RenderGraph graph;
	const auto history =
		graph.AddResource({.Name = Name("history"), .Kind = ResourceKind::Storage, .External = true});
	REQUIRE(
		AddWorldNode(graph, {.Name = Name("previous"), .Kind = Name("compute"), .Reads = {history}}).IsValid()
	);
	REQUIRE(AddWorldNode(
				graph, {.Name = Name("first-write"), .Kind = Name("compute"), .Writes = {history}}
	).IsValid());
	REQUIRE(AddWorldNode(
				graph, {.Name = Name("second-write"), .Kind = Name("compute"), .Writes = {history}}
	).IsValid());
	REQUIRE(
		AddWorldNode(graph, {.Name = Name("current"), .Kind = Name("compute"), .Reads = {history}}).IsValid()
	);
	const auto compiled = Compile(graph);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, schedule, projection, offender) == EngineGraphStatus::Ok);
	Recorder recorder;
	REQUIRE(ExecuteRenderStage(graph, projection, recorder, size_t{0}));
	CHECK(recorder.Names == std::vector<std::string>{"previous", "first-write", "second-write", "current"});
	CHECK(projection.Graph.Resources().front().External);
	recorder.Names.clear();
	recorder.Refuse = Name("first-write");
	CHECK_FALSE(ExecuteRenderStage(graph, projection, recorder, size_t{0}));
	CHECK(recorder.Names == std::vector<std::string>{"previous", "first-write"});
}

TEST_CASE(
	"render projection admits its identity budget and preserves the prior plan one byte over",
	"[graph][engine]"
) {
	const auto graph = VersionedGraph();
	const auto compiled = Compile(graph);
	const size_t authoredBytes = std::string_view("import-firstimport-secondfirstsecond").size();
	const size_t identities = 4;
	const size_t prefixBytes = EngineGraph::MAXIMUM_IMPORT_IDENTITY_BYTES - authoredBytes;
	REQUIRE(prefixBytes % identities == 0);
	const std::string prefix(prefixBytes / identities, 'p');
	Name offender;
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, projection, offender, prefix) == EngineGraphStatus::Ok);
	CHECK(projection.Graph.NodeCount() == 2);
	RenderGraph oneOver;
	const auto first = oneOver.AddResource({.Name = Name("import-first"), .Kind = ResourceKind::Storage});
	const auto second = oneOver.AddResource({.Name = Name("import-second"), .Kind = ResourceKind::Storage});
	REQUIRE(
		AddWorldNode(oneOver, {.Name = Name("first"), .Kind = Name("compute"), .Writes = {first}}).IsValid()
	);
	REQUIRE(AddWorldNode(
				oneOver,
				{.Name = Name("secondx"), .Kind = Name("compute"), .Reads = {first}, .Writes = {second}}
	).IsValid());
	CHECK(
		ProjectRenderStage(oneOver, Compile(oneOver), projection, offender, prefix) ==
		EngineGraphStatus::LimitExceeded
	);
	CHECK(offender == Name("secondx"));
	Recorder recorder;
	REQUIRE(ExecuteRenderStage(graph, projection, recorder, size_t{0}));
	CHECK(recorder.Names == std::vector<std::string>{"first", "second"});
}

TEST_CASE(
	"authored final world work retains its contract while scheduled admission requires world setup",
	"[graph][engine]"
) {
	RenderGraph graph;
	REQUIRE(IndependentNode(graph, "setup", NodeScope::World).IsValid());
	REQUIRE(IndependentNode(graph, "view", NodeScope::View).IsValid());
	const auto late = IndependentNode(graph, "late-world", NodeScope::World);
	REQUIRE(late.IsValid());
	const auto compiled = Compile(graph);
	REQUIRE(compiled.Final == std::vector<NodeId>{late});
	Name offender;
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, projection, offender) == EngineGraphStatus::Ok);
	struct ScopeRecorder final : NodeRunner {
		std::vector<std::string> Names;
		std::vector<size_t> Worlds;
		bool Run(const RunContext &context) override {
			Names.emplace_back(context.Name.Text());
			Worlds.push_back(context.World);
			return true;
		}
	};
	const uint64_t worlds[]{7, 9};
	ScopeRecorder before;
	REQUIRE(ExecuteRenderStage(graph, projection, before, worlds));
	CHECK(before.Names == std::vector<std::string>{"setup", "view", "setup", "view", "late-world"});
	CHECK(before.Worlds == std::vector<size_t>{0, 0, 1, 1, RunContext::WHOLE_FRAME});
	ExecutionSchedule schedule;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	CHECK(
		ProjectRenderStage(graph, compiled, schedule, projection, offender) ==
		EngineGraphStatus::InvalidDependency
	);
	CHECK(offender == Name("late-world"));
	ScopeRecorder after;
	REQUIRE(ExecuteRenderStage(graph, projection, after, worlds));
	CHECK(after.Names == before.Names);
	CHECK(after.Worlds == before.Worlds);
}

TEST_CASE("scheduled projection refuses displaced frame setup or final work", "[graph][engine]") {
	RenderGraph graph;
	const auto setup = IndependentNode(graph, "frame-setup", NodeScope::Frame);
	const auto view = IndependentNode(graph, "view", NodeScope::View);
	const auto final = IndependentNode(graph, "frame-final", NodeScope::Frame);
	REQUIRE(setup.IsValid());
	REQUIRE(view.IsValid());
	REQUIRE(final.IsValid());
	auto compiled = Compile(graph);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, schedule, projection, offender) == EngineGraphStatus::Ok);
	Name expected;
	SECTION("setup displaced to final") {
		compiled.Shared.clear();
		compiled.Final.insert(compiled.Final.begin(), setup);
		expected = Name("frame-setup");
	}
	SECTION("final displaced to setup") {
		compiled.Final.clear();
		compiled.Shared.push_back(final);
		expected = Name("frame-final");
	}
	CHECK(
		ProjectRenderStage(graph, compiled, schedule, projection, offender) ==
		EngineGraphStatus::InvalidDependency
	);
	CHECK(offender == expected);
	Recorder recorder;
	REQUIRE(ExecuteRenderStage(graph, projection, recorder, size_t{1}));
	CHECK(recorder.Names == std::vector<std::string>{"frame-setup", "view", "frame-final"});
}

TEST_CASE("scheduled projection keeps previous history ahead of its sole current writer", "[graph][engine]") {
	RenderGraph graph;
	const auto history = graph.AddResource(
		{.Name = Name("history-generation"),
		 .Kind = ResourceKind::Storage,
		 .Lifetime = ResourceLifetime::History}
	);
	const auto previous = graph.AddResource({.Name = Name("previous-result"), .Kind = ResourceKind::Storage});
	const auto current = graph.AddResource({.Name = Name("current-result"), .Kind = ResourceKind::Storage});
	REQUIRE(history.IsValid());
	REQUIRE(previous.IsValid());
	REQUIRE(current.IsValid());
	REQUIRE(
		AddWorldNode(
			graph,
			{.Name = Name("read-previous"), .Kind = Name("compute"), .Reads = {history}, .Writes = {previous}}
		).IsValid()
	);
	REQUIRE(AddWorldNode(
				graph, {.Name = Name("write-current"), .Kind = Name("compute"), .Writes = {history}}
	).IsValid());
	REQUIRE(
		AddWorldNode(
			graph,
			{.Name = Name("read-current"), .Kind = Name("compute"), .Reads = {history}, .Writes = {current}}
		).IsValid()
	);
	const auto compiled = Compile(graph);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	RenderStageProjection projection;
	REQUIRE(ProjectRenderStage(graph, compiled, schedule, projection, offender) == EngineGraphStatus::Ok);
	REQUIRE(projection.Blocks[0].Waves.size() == schedule.Waves.size());
	for (size_t index = 0; index < schedule.Waves.size(); index++) {
		std::vector<NodeId> expected;
		for (const auto &entry : schedule.Waves[index].Nodes)
			expected.push_back(entry.Node);
		CHECK(OriginalNodes(projection, projection.Blocks[0].Waves[index]) == expected);
	}
	Recorder recorder;
	REQUIRE(ExecuteRenderStage(graph, projection, recorder, size_t{0}));
	CHECK(recorder.Names == std::vector<std::string>{"read-previous", "write-current", "read-current"});
	const auto *original = graph.FindResourceDesc(history);
	REQUIRE(original != nullptr);
	CHECK(original->Lifetime == ResourceLifetime::History);
	CHECK(original->External);
	CHECK(
		projection.Graph.FindResourceDesc(Name("render.history-generation"))->Lifetime ==
		EngineResourceLifetime::Persistent
	);
}
