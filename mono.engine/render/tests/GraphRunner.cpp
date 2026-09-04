#include "BackendNodes.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.graphrunner")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::RenderGraph;
using engine::graph::RunContext;
using engine::render::GraphRunner;
using engine::render::NodeTable;

namespace {
	RenderGraph DefaultGraph() {
		RenderGraph graph;
		Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	CompiledGraph Compile(const RenderGraph &graph) {
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok);
		return compiled;
	}

	void Record(const RenderGraph &graph, NodeTable &table, std::vector<std::string> &ran) {
		for (uint32_t value = 1; value <= graph.Count(); value++) {
			const engine::graph::Node *node = graph.Find(engine::graph::NodeId{value});
			REQUIRE(node != nullptr);
			table.Set(node->Kind, [&ran](const RunContext &context) {
				std::string entry(context.Name.Text());
				if (context.View != RunContext::WHOLE_FRAME) {
					entry += "@" + std::to_string(context.View);
				}
				ran.push_back(std::move(entry));
				return true;
			});
		}
	}
}

TEST_CASE("the default graph is dispatched in authored order", "[render][graph]") {
	const RenderGraph graph = DefaultGraph();
	std::vector<std::string> ran;
	NodeTable table;
	Record(graph, table, ran);

	GraphRunner runner(table);
	const uint64_t worlds[] = {7};
	REQUIRE(graph.Execute(Compile(graph), runner, worlds));

	CHECK(
		ran == std::vector<std::string>{
				   "world",
				   "shadow",
				   "camera@0",
				   "last-frame@0",
				   "entities@0",
				   "cull-frustum@0",
				   "order-draw@0",
				   "upload-instances@0",
				   "mirror-capture@0",
				   "portal-capture@0",
				   "portal-tonemap@0",
				   "gbuffer@0",
				   "depth-linearise@0",
				   "ssao@0",
				   "deferred-lighting@0",
				   "sky@0",
				   "volumetrics@0",
				   "tonemap@0",
				   "portal-overlay@0",
				   "mirror-overlay@0",
				   "transparent@0",
				   "present",
				   "interface",
				   "overlay",
				   "output-image",
			   }
	);
	CHECK(runner.Submitted() == 25);
	CHECK_FALSE(runner.Unhandled().IsValid());
}

TEST_CASE("world work is shared while view work is repeated", "[render][graph]") {
	const RenderGraph graph = DefaultGraph();
	std::vector<std::string> ran;
	NodeTable table;
	Record(graph, table, ran);

	GraphRunner runner(table);
	const uint64_t worlds[] = {3, 3, 9};
	REQUIRE(graph.Execute(Compile(graph), runner, worlds));

	CHECK(std::count(ran.begin(), ran.end(), "shadow") == 2);
	CHECK(std::count(ran.begin(), ran.end(), "world") == 2);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@0") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@1") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@2") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "present") == 1);
}

TEST_CASE("missing backend kinds are reported before execution", "[render][graph]") {
	RenderGraph graph;
	const auto target = graph.AddResource({.Name = Name("target")});
	REQUIRE(target.IsValid());

	engine::graph::Node first;
	first.Name = Name("zeta");
	first.Kind = Name("missing-zeta");
	first.Writes = {target};
	first.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(first).IsValid());

	engine::graph::Node second = first;
	second.Name = Name("alpha");
	second.Kind = Name("missing-alpha");
	REQUIRE(graph.AddNode(second).IsValid());

	NodeTable table;
	const std::vector<Name> missing = table.Missing(graph);
	REQUIRE(missing.size() == 2);
	CHECK(missing[0] == Name("missing-alpha"));
	CHECK(missing[1] == Name("missing-zeta"));

	table.Set(Name("missing-zeta"), [](const RunContext &) { return true; });
	GraphRunner runner(table);
	CHECK_FALSE(graph.Execute(Compile(graph), runner, size_t{1}));
	CHECK(runner.Unhandled() == Name("missing-alpha"));
}

TEST_CASE("invalid node registrations are refused", "[render][graph]") {
	NodeTable table;
	CHECK_FALSE(table.Set({}, [](const RunContext &) { return true; }));
	CHECK_FALSE(table.Set(Name("valid"), {}));
	CHECK(table.Set(Name("valid"), [](const RunContext &) { return true; }));
	CHECK(table.Has(Name("valid")));
	CHECK(table.Count() == 1);
	table.Clear();
	CHECK(table.Count() == 0);
}

TEST_CASE("backend metadata is derived from the node catalogue", "[render][graph]") {
	engine::graph::RegisterRenderNodeKinds();
	const std::vector<engine::render::BackendNode> backends = engine::render::BackendNodes();

	for (const engine::graph::NodeKindSpec &spec : engine::graph::NodeCatalogue::All()) {
		const auto backend = std::find_if(
			backends.begin(), backends.end(), [&spec](const engine::render::BackendNode &candidate) {
				return candidate.Kind == spec.Kind;
			}
		);
		INFO("kind: " << spec.Kind.Text());
		CHECK((backend != backends.end()) == spec.BuiltInBackend);
		if (backend != backends.end()) {
			CHECK(backend->Scope == spec.Scope);
			CHECK(backend->Queue == spec.Queue);
		}
	}
}

TEST_CASE("GraphRunner owns profiling tiers and dropped mark accounting", "[render][profile]") {
	const RenderGraph graph = DefaultGraph();
	std::vector<std::string> ran;
	NodeTable table;
	Record(graph, table, ran);

	size_t opened = 0;
	size_t closed = 0;
	engine::render::NodeProfileHooks profile;
	profile.Enabled = [](const RunContext &context) { return context.Kind != Name("shadow"); };
	profile.Begin = [&opened](const RunContext &) { opened++; };
	profile.End = [&closed](const RunContext &context) {
		closed++;
		return context.Kind == Name("gbuffer") ? size_t{2} : size_t{0};
	};
	GraphRunner full(table, engine::render::ProfilingTier::Full, std::move(profile));
	const uint64_t worlds[] = {7};
	REQUIRE(graph.Execute(Compile(graph), full, worlds));
	CHECK(opened == 24);
	CHECK(closed == opened);
	CHECK(full.DroppedProfileMarks() == 2);

	opened = 0;
	closed = 0;
	engine::render::NodeProfileHooks disabled;
	disabled.Begin = [&opened](const RunContext &) { opened++; };
	disabled.End = [&closed](const RunContext &) {
		closed++;
		return size_t{4};
	};
	GraphRunner off(table, engine::render::ProfilingTier::Off, std::move(disabled));
	REQUIRE(graph.Execute(Compile(graph), off, worlds));
	CHECK(opened == 0);
	CHECK(closed == 0);
	CHECK(off.DroppedProfileMarks() == 0);
}
