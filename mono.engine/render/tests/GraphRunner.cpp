#include "RenderFixture.hpp"
#include "RenderNodeExecutor.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
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
				   "mesh-residency",
				   "shadow",
				   "skybox-compute",
				   "clouds-compute",
				   "camera@0",
				   "last-frame@0",
				   "entities@0",
				   "cull-frustum@0",
				   "order-draw@0",
				   "delta-upload@0",
				   "select-lod@0",
				   "surface-capture@0",
				   "gbuffer@0",
				   "depth-linearise@0",
				   "ssao@0",
				   "deferred-lighting@0",
				   "sky@0",
				   "fog@0",
				   "portal-overlay@0",
				   "mirror-overlay@0",
				   "transparent@0",
				   "shader-lenses@0",
				   "dof@0",
				   "god-rays@0",
				   "bloom@0",
				   "tonemap@0",
				   "present",
				   "interface",
				   "overlay",
				   "output-image",
			   }
	);
	CHECK(runner.Submitted() == 31);
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
	CHECK(std::count(ran.begin(), ran.end(), "skybox-compute") == 2);
	CHECK(std::count(ran.begin(), ran.end(), "clouds-compute") == 2);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@0") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@1") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "gbuffer@2") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "present") == 1);
}

TEST_CASE("graph runner readers observe authored resource versions", "[render][graph]") {
	for (const bool secondEnabled : {true, false}) {
		CAPTURE(secondEnabled);
		RenderGraph graph;
		const auto colour =
			graph.AddResource({.Name = Name("authored-colour"), .Kind = engine::graph::ResourceKind::Colour});
		const auto add = [&](const char *name, const char *kind, bool writes, bool enabled = true) {
			engine::graph::Node node;
			node.Name = Name(name);
			node.Kind = Name(kind);
			node.Scope = engine::graph::NodeScope::View;
			node.Enabled = enabled;
			if (writes)
				node.Writes = {colour};
			else
				node.Reads = {colour};
			graph.AddNode(node);
		};
		add("first", "write-authored-colour", true);
		add("before-overwrite", "read-authored-colour", false);
		add("second", "write-authored-colour", true, secondEnabled);
		add("after-overwrite", "read-authored-colour", false);

		std::array<std::string, 2> current;
		std::vector<std::string> observed;
		std::vector<std::string> order;
		NodeTable table;
		REQUIRE(table.Set(Name("write-authored-colour"), [&](const RunContext &context) {
			REQUIRE(context.View < current.size());
			current[context.View] = std::string(context.Name.Text());
			order.push_back(current[context.View] + "@" + std::to_string(context.View));
			return true;
		}));
		REQUIRE(table.Set(Name("read-authored-colour"), [&](const RunContext &context) {
			REQUIRE(context.View < current.size());
			const std::string label = std::string(context.Name.Text()) + "@" + std::to_string(context.View);
			order.push_back(label);
			observed.push_back(label + "=" + current[context.View]);
			return true;
		}));
		GraphRunner runner(table);
		const uint64_t worlds[] = {7, 8};
		REQUIRE(graph.Execute(Compile(graph), runner, worlds));
		CHECK(runner.Submitted() == (secondEnabled ? 8 : 6));
		std::vector<std::string> expectedOrder;
		std::vector<std::string> expectedObserved;
		for (size_t view = 0; view < 2; ++view) {
			const std::string suffix = "@" + std::to_string(view);
			expectedOrder.push_back("first" + suffix);
			expectedOrder.push_back("before-overwrite" + suffix);
			if (secondEnabled) expectedOrder.push_back("second" + suffix);
			expectedOrder.push_back("after-overwrite" + suffix);
			expectedObserved.push_back("before-overwrite" + suffix + "=first");
			expectedObserved.push_back("after-overwrite" + suffix + (secondEnabled ? "=second" : "=first"));
		}
		CHECK(order == expectedOrder);
		CHECK(observed == expectedObserved);
	}
}

TEST_CASE("graph runner dispatches history reads before same-frame writes", "[render][graph]") {
	RenderGraph graph;
	const auto history = graph.AddResource(
		{.Name = Name("history-colour"),
		 .Kind = engine::graph::ResourceKind::Colour,
		 .External = true,
		 .Lifetime = engine::graph::ResourceLifetime::History}
	);
	const auto add = [&](const char *name, const char *kind, bool writes) {
		engine::graph::Node node;
		node.Name = Name(name);
		node.Kind = Name(kind);
		node.Scope = engine::graph::NodeScope::View;
		if (writes)
			node.Writes = {history};
		else
			node.Reads = {history};
		graph.AddNode(node);
	};
	add("read-previous", "read-history", false);
	add("first-write", "write-history", true);
	add("second-write", "write-history", true);
	engine::graph::ExecutionSchedule schedule;
	Name offender;
	REQUIRE(engine::graph::CompileSchedule(graph, schedule, offender) == engine::graph::ScheduleStatus::Ok);
	const auto compiled = Compile(graph);

	// This small store models generations to check dispatch. It does not exercise GPU aliasing.
	std::string previous = "prior";
	std::string next;
	std::vector<std::string> observed;
	std::vector<std::string> order;
	NodeTable table;
	REQUIRE(table.Set(Name("read-history"), [&](const RunContext &context) {
		order.emplace_back(context.Name.Text());
		observed.push_back(previous);
		return true;
	}));
	REQUIRE(table.Set(Name("write-history"), [&](const RunContext &context) {
		order.emplace_back(context.Name.Text());
		next = std::string(context.Name.Text());
		return true;
	}));
	const uint64_t worlds[] = {7};
	for (size_t frame = 0; frame < 2; ++frame) {
		GraphRunner runner(table);
		REQUIRE(graph.Execute(compiled, runner, worlds));
		CHECK(runner.Submitted() == 3);
		CHECK(next == "second-write");
		previous = next;
		next.clear();
	}
	CHECK(observed == std::vector<std::string>{"prior", "second-write"});
	CHECK(
		order ==
		std::vector<std::string>{
			"read-previous", "first-write", "second-write", "read-previous", "first-write", "second-write"
		}
	);
}

TEST_CASE("graph runner carries world values through views before frame work", "[render][graph]") {
	RenderGraph graph;
	const auto worldColour =
		graph.AddResource({.Name = Name("world-colour"), .Kind = engine::graph::ResourceKind::Colour});
	const auto viewColour =
		graph.AddResource({.Name = Name("view-colour"), .Kind = engine::graph::ResourceKind::Colour});
	engine::graph::Node world;
	world.Name = Name("world-write");
	world.Kind = Name("write-world-colour");
	world.Scope = engine::graph::NodeScope::World;
	world.Writes = {worldColour};
	graph.AddNode(world);
	engine::graph::Node view;
	view.Name = Name("view-read");
	view.Kind = Name("read-world-colour");
	view.Scope = engine::graph::NodeScope::View;
	view.Reads = {worldColour};
	view.Writes = {viewColour};
	graph.AddNode(view);
	engine::graph::Node frame;
	frame.Name = Name("frame-read");
	frame.Kind = Name("read-view-colour");
	frame.Scope = engine::graph::NodeScope::Frame;
	frame.Reads = {viewColour};
	graph.AddNode(frame);
	engine::graph::ExecutionSchedule schedule;
	Name offender;
	REQUIRE(engine::graph::CompileSchedule(graph, schedule, offender) == engine::graph::ScheduleStatus::Ok);

	std::array<std::string, 2> worldValues;
	std::array<std::string, 3> viewValues;
	std::vector<std::string> order;
	std::string frameValue;
	NodeTable table;
	REQUIRE(table.Set(Name("write-world-colour"), [&](const RunContext &context) {
		REQUIRE(context.View == RunContext::WHOLE_FRAME);
		REQUIRE(context.World < worldValues.size());
		worldValues[context.World] = "world" + std::to_string(context.World);
		order.push_back("W" + std::to_string(context.World));
		return true;
	}));
	REQUIRE(table.Set(Name("read-world-colour"), [&](const RunContext &context) {
		REQUIRE(context.View < viewValues.size());
		REQUIRE(context.World < worldValues.size());
		viewValues[context.View] = worldValues[context.World];
		order.push_back("V" + std::to_string(context.View));
		return true;
	}));
	REQUIRE(table.Set(Name("read-view-colour"), [&](const RunContext &context) {
		REQUIRE(context.View == RunContext::WHOLE_FRAME);
		REQUIRE(context.World == RunContext::WHOLE_FRAME);
		frameValue = viewValues[0] + "," + viewValues[1] + "," + viewValues[2];
		order.push_back("F");
		return true;
	}));
	GraphRunner runner(table);
	const uint64_t worlds[] = {7, 7, 9};
	REQUIRE(graph.Execute(Compile(graph), runner, worlds));
	CHECK(runner.Submitted() == 6);
	CHECK(order == std::vector<std::string>{"W0", "V0", "V1", "W1", "V2", "F"});
	CHECK(viewValues == std::array<std::string, 3>{"world0", "world0", "world1"});
	CHECK(frameValue == "world0,world0,world1");
}

TEST_CASE("clouds consume the skybox producer in the same world invocation", "[render][graph]") {
	const RenderGraph graph = DefaultGraph();
	const engine::graph::Node *skybox = nullptr;
	const engine::graph::Node *clouds = nullptr;
	for (uint32_t value = 1; value <= graph.Count(); ++value) {
		const auto *node = graph.Find(engine::graph::NodeId{value});
		if (node == nullptr) continue;
		if (node->Kind == Name("skybox-compute")) skybox = node;
		if (node->Kind == Name("clouds-compute")) clouds = node;
	}
	REQUIRE(skybox != nullptr);
	REQUIRE(clouds != nullptr);
	REQUIRE(skybox->Writes.size() == 1);
	REQUIRE(clouds->Reads.size() == 1);
	CHECK(skybox->Scope == engine::graph::NodeScope::World);
	CHECK(clouds->Scope == engine::graph::NodeScope::World);
	CHECK(clouds->Reads.front() == skybox->Writes.front());

	const CompiledGraph compiled = Compile(graph);
	const auto position = [&](Name kind) {
		for (size_t index = 0; index < compiled.Shared.size(); ++index) {
			const auto *node = graph.Find(compiled.Shared[index]);
			if (node != nullptr && node->Kind == kind) return index;
		}
		return compiled.Shared.size();
	};
	CHECK(position(Name("skybox-compute")) < position(Name("clouds-compute")));
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

TEST_CASE("registered handler refusal reports the authored node", "[render][graph]") {
	RenderGraph graph;
	const auto target = graph.AddResource({.Name = Name("target")});
	REQUIRE(target.IsValid());
	engine::graph::Node node;
	node.Name = Name("deferred-output");
	node.Kind = Name("deferred-lighting");
	node.Writes = {target};
	node.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(node).IsValid());

	NodeTable table;
	REQUIRE(table.Set(Name("deferred-lighting"), [](const RunContext &) { return false; }));
	for (const auto tier :
		 {engine::render::ProfilingTier::Off,
		  engine::render::ProfilingTier::Cpu,
		  engine::render::ProfilingTier::Full}) {
		GraphRunner runner(table, tier);
		CHECK_FALSE(graph.Execute(Compile(graph), runner, size_t{1}));
		CHECK(runner.Rejected() == Name("deferred-output"));
		CHECK_FALSE(runner.Unhandled().IsValid());
	}
}

TEST_CASE("deferred baseline port extent follows its port in either order", "[render][graph]") {
	using engine::graph::Edit;
	using engine::graph::EditKind;
	using engine::graph::PipelineDocument;
	using engine::graph::PipelineDocumentStatus;
	const bool baselineFirst = GENERATE(false, true);
	const bool matchingExtent = GENERATE(false, true);
	CAPTURE(baselineFirst, matchingExtent);

	PipelineDocument document;
	document.Record(
		{.Kind = EditKind::AddResource,
		 .Name = Name("lighting-baseline"),
		 .Resource = engine::graph::ResourceKind::Colour,
		 .Format = engine::graph::ResourceFormat::RGBA32F,
		 .Width = matchingExtent ? 65u : 33u,
		 .Height = matchingExtent ? 37u : 19u}
	);
	const Edit baselineWrite{
		.Kind = EditKind::Writes, .Target = Name("lighting-baseline"), .Key = Name("lighting-baseline")
	};
	const engine::graph::PipelineDocument base = engine::graph::DefaultPbrDocument();
	for (auto edit : base.Edits()) {
		if (edit.Kind == EditKind::AddResource && edit.Name == Name("lit")) {
			edit.Width = 65;
			edit.Height = 37;
		}
		const bool colourWrite = edit.Kind == EditKind::Writes && edit.Target == Name("lit");
		if (colourWrite && baselineFirst) document.Record(baselineWrite);
		document.Record(edit);
		if (colourWrite && !baselineFirst) document.Record(baselineWrite);
	}

	RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	const engine::graph::Node *deferred = nullptr;
	for (uint32_t value = 1; value <= graph.Count(); ++value) {
		const auto *node = graph.Find(engine::graph::NodeId{value});
		if (node != nullptr && node->Kind == Name("deferred-lighting")) {
			deferred = node;
			break;
		}
	}
	REQUIRE(deferred != nullptr);
	const auto colourPort =
		std::find(deferred->WritePorts.begin(), deferred->WritePorts.end(), Name("colour"));
	const auto baselinePort =
		std::find(deferred->WritePorts.begin(), deferred->WritePorts.end(), Name("lighting-baseline"));
	REQUIRE(colourPort != deferred->WritePorts.end());
	REQUIRE(baselinePort != deferred->WritePorts.end());
	const auto *colour = graph.FindResource(deferred->Writes[colourPort - deferred->WritePorts.begin()]);
	const auto *baseline = graph.FindResource(deferred->Writes[baselinePort - deferred->WritePorts.begin()]);
	REQUIRE(colour != nullptr);
	REQUIRE(baseline != nullptr);
	uint32_t colourWidth = 0, colourHeight = 0, baselineWidth = 0, baselineHeight = 0;
	colour->Resolve(1, 1, colourWidth, colourHeight);
	baseline->Resolve(1, 1, baselineWidth, baselineHeight);
	CHECK(colourWidth == 65);
	CHECK(colourHeight == 37);
	CHECK(baseline->Format == engine::graph::ResourceFormat::RGBA32F);
	CHECK((baselineWidth == colourWidth && baselineHeight == colourHeight) == matchingExtent);
}

TEST_CASE(
	"deferred baseline extent validation is independent of output port order",
	"[render][gpu][graph][lighting-baseline-extent][.]"
) {
	using namespace engine;
	const bool baselineFirst = GENERATE(false, true);
	const bool matchingExtent = GENERATE(false, true);
	CAPTURE(baselineFirst, matchingExtent);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::PipelineDocument document;
	document.Record(
		{.Kind = graph::EditKind::AddResource,
		 .Name = Name("lighting-baseline"),
		 .Resource = graph::ResourceKind::Colour,
		 .Format = graph::ResourceFormat::RGBA32F,
		 .Width = matchingExtent ? 65u : 33u,
		 .Height = matchingExtent ? 37u : 19u}
	);
	const graph::Edit baselineWrite{
		.Kind = graph::EditKind::Writes, .Target = Name("lighting-baseline"), .Key = Name("lighting-baseline")
	};
	const auto base = graph::DefaultPbrDocument();
	for (auto edit : base.Edits()) {
		if (edit.Kind == graph::EditKind::AddResource && edit.Name == Name("lit")) {
			edit.Width = 65;
			edit.Height = 37;
		}
		const bool colourWrite = edit.Kind == graph::EditKind::Writes && edit.Target == Name("lit");
		if (colourWrite && baselineFirst) document.Record(baselineWrite);
		document.Record(edit);
		if (colourWrite && !baselineFirst) document.Record(baselineWrite);
	}
	RenderGraph pipeline;
	Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const Name pipelineName("baseline-extent");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	render::OverlayImage overlay;
	const auto report = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(report.Ran(Name("deferred-lighting")) == matchingExtent);
}

TEST_CASE("backend support and catalogue declarations stay in lockstep", "[render][graph]") {
	engine::graph::RegisterRenderNodeKinds();
	const std::vector<engine::render::BackendNode> backends = engine::render::BackendNodes();
	for (const std::string_view kind : engine::render::BuiltInBackendKinds()) {
		const auto *spec = engine::graph::NodeCatalogue::Find(Name(kind));
		INFO("backend kind: " << kind);
		REQUIRE(spec != nullptr);
		CHECK(spec->BuiltInBackend);
	}

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
	// Bloom is a separate HDR graph pass and is profiled with the rest of the
	// view chain, even when its authored intensity leaves it pass-through.
	CHECK(opened == 30);
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
