// The seam between a render graph and the code that submits passes.
//
// **The renderer is the one module a suite cannot exercise, so this is written
// to be the part of it that can be.** `PassTable` and `GraphRunner` hold no
// device state and no SDL; every handler here writes its name down and returns.
// What that buys is the assertion the whole of D00002 rests on: that running
// the standard frame through the graph submits **the same passes, in the same
// order, the same number of times** as the loop it is replacing.
//
// If that holds headlessly, the remaining risk in swapping them is the device
// code inside each handler — which is unchanged by the swap, and which
// `just studio-smoke` covers by comparing the image byte for byte.

#include <engine/graph/RenderGraph.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.graphrunner")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::GraphStatus;
using engine::graph::RenderGraph;
using engine::graph::RunContext;
using engine::graph::StandardGraph;
using engine::render::GraphRunner;
using engine::render::PassTable;

namespace {
	// Registers a handler per kind that writes down what it was asked to do.
	//
	// The line is `kind@view` so a per-view pass in a four-view frame reads as
	// four distinct entries rather than as one repeated — which is the whole
	// property being asserted.
	void Record(PassTable &table, const RenderGraph &graph, std::vector<std::string> &into) {
		for (size_t index = 0; index < graph.Count(); index++) {
			const engine::graph::Node *node =
				graph.Find(engine::graph::NodeId{static_cast<uint32_t>(index + 1)});
			if (node == nullptr) {
				continue;
			}
			table.Set(node->Kind, [&into](const RunContext &context) {
				std::string line{context.Name.Text()};
				if (context.View != RunContext::WHOLE_FRAME) {
					line += "@" + std::to_string(context.View);
				}
				into.push_back(line);
				return true;
			});
		}
	}

	CompiledGraph Compiled(const RenderGraph &graph) {
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		return compiled;
	}
}

// --- the equivalence D00002 rests on -----------------------------------------------

TEST_CASE("the standard frame runs its passes in the renderer's order", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	GraphRunner runner(table);
	const uint64_t worlds[] = {1};
	REQUIRE(graph.Execute(Compiled(graph), runner, worlds));

	// **The exact sequence `Renderer::Render` submits today.** Shadow once for
	// the world, three passes for the one view, then the overlay and the chrome
	// once over the lot. If this list ever stops matching what the renderer
	// does, one of the two has drifted and this is where it shows.
	CHECK(
		ran ==
		std::vector<std::string>{"shadow", "surface@0", "opaque@0", "transparent@0", "overlay", "interface"}
	);
	CHECK(runner.Submitted() == 6);
	CHECK_FALSE(runner.Unhandled().IsValid());
}

TEST_CASE("four views of one world share the shadow pass", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	GraphRunner runner(table);
	const uint64_t worlds[] = {1, 1, 1, 1};
	REQUIRE(graph.Execute(Compiled(graph), runner, worlds));

	// **The whole reason the partition exists.** Split-screen pays for one
	// shadow map and four of everything the camera sees — a renderer that
	// submitted the shadow pass per view would cost four times as much and look
	// identical, which is the worst shape a performance bug has.
	CHECK(std::count(ran.begin(), ran.end(), std::string("shadow")) == 1);
	CHECK(std::count(ran.begin(), ran.end(), std::string("opaque@3")) == 1);
	CHECK(std::count(ran.begin(), ran.end(), std::string("interface")) == 1);
	CHECK(runner.Submitted() == 1 + 4 * 3 + 2);
}

TEST_CASE("two worlds each get their own shared work", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	GraphRunner runner(table);
	const uint64_t worlds[] = {1, 2};
	REQUIRE(graph.Execute(Compiled(graph), runner, worlds));

	// A second world's casters are different casters, so it needs its own
	// atlas. The chrome is still drawn once.
	CHECK(std::count(ran.begin(), ran.end(), std::string("shadow")) == 2);
	CHECK(std::count(ran.begin(), ran.end(), std::string("interface")) == 1);
}

// --- what happens when something is missing ------------------------------------------

TEST_CASE("a kind with no handler refuses the frame and names itself", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	// Take one away, as a renderer that had not been taught a new pass would be.
	PassTable partial;
	partial.Set(Name("shadow"), [&ran](const RunContext &) {
		ran.push_back("shadow");
		return true;
	});

	GraphRunner runner(partial);
	const uint64_t worlds[] = {1};
	CHECK_FALSE(graph.Execute(Compiled(graph), runner, worlds));

	// **A refusal, and a named one.** A frame that quietly skipped the pass
	// nobody had registered would render dark and the scene would get the blame;
	// a bare `false` would say something went wrong and not what.
	CHECK(runner.Unhandled() == Name("surface"));
}

TEST_CASE("the missing kinds can be asked for before a frame is started", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	PassTable table;
	CHECK(table.Missing(graph).size() == 6);

	table.Set(Name("shadow"), [](const RunContext &) { return true; });
	table.Set(Name("surface"), [](const RunContext &) { return true; });

	// **Sorted and deduplicated**, so a diagnostic reads the same way twice.
	const std::vector<Name> missing = table.Missing(graph);
	REQUIRE(missing.size() == 4);
	CHECK(missing[0] == Name("interface"));
	CHECK(missing[1] == Name("opaque"));
	CHECK(missing[2] == Name("overlay"));
	CHECK(missing[3] == Name("transparent"));
}

TEST_CASE("a disabled pass is not a missing one", "[render][graphrunner]") {
	RenderGraph graph = StandardGraph();

	PassTable table;
	REQUIRE(table.Missing(graph).size() == 6);

	// Switching a pass off is a decision. A renderer that reported it as one it
	// could not draw would make "what can this build render" depend on what
	// happened to be turned on.
	graph.SetEnabled(engine::graph::NodeId{1}, false);
	CHECK(table.Missing(graph).size() == 5);
}

TEST_CASE("a handler that fails stops the frame", "[render][graphrunner]") {
	const RenderGraph graph = StandardGraph();

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	// The device refusing mid-frame — a swapchain lost, an allocation denied.
	table.Set(Name("opaque"), [&ran](const RunContext &) {
		ran.push_back("opaque");
		return false;
	});

	GraphRunner runner(table);
	const uint64_t worlds[] = {1};
	CHECK_FALSE(graph.Execute(Compiled(graph), runner, worlds));

	// **Told apart from a missing handler**, which is the reason `Unhandled`
	// exists: this one knew how to draw the pass and the pass failed.
	CHECK_FALSE(runner.Unhandled().IsValid());
	CHECK(ran.back() == "opaque");
}

// --- the table itself -------------------------------------------------------------

TEST_CASE("the table refuses a handler it could not call", "[render][graphrunner]") {
	PassTable table;

	CHECK_FALSE(table.Set(Name(), [](const RunContext &) { return true; }));
	CHECK_FALSE(table.Set(Name("opaque"), {}));
	CHECK(table.Count() == 0);

	CHECK(table.Set(Name("opaque"), [](const RunContext &) { return true; }));
	CHECK(table.Has(Name("opaque")));
	CHECK(table.Count() == 1);

	// Registering twice replaces rather than stacking, so a game correcting a
	// built-in gets one handler and not two.
	CHECK(table.Set(Name("opaque"), [](const RunContext &) { return false; }));
	CHECK(table.Count() == 1);

	table.Clear();
	CHECK_FALSE(table.Has(Name("opaque")));
}

TEST_CASE("a viewer node is handed the resource it is wired to", "[render][graph]") {
	// **The whole of what a `viewer` node does.** It draws nothing and produces
	// nothing; its entire job is to name a resource so that somebody downloads
	// it. `Renderer`'s handler turns `RunContext::Reads` into a texture and asks
	// for a copy — so what is worth asserting without a device is that the
	// runner hands the node the resource the graph wired to it, and that a
	// pipeline with one in it still compiles and runs.
	//
	// **`viewer` is not in `StandardGraph`, and should not be.** A frame that
	// always paid for a readback nobody was looking at would be the wrong
	// default. So this builds the smallest pipeline that has one, which is also
	// the first test of `Renderer::SetPipeline`'s shape: a graph the caller
	// authored rather than the standard one.
	RenderGraph graph = StandardGraph();

	engine::graph::ResourceId colour;
	for (size_t index = 0; index < graph.ResourceCount(); index++) {
		const engine::graph::ResourceId id{static_cast<uint32_t>(index + 1)};
		const engine::graph::ResourceDesc *desc = graph.FindResource(id);
		if (desc != nullptr && desc->Name == Name("colour")) {
			colour = id;
		}
	}
	REQUIRE(colour.IsValid());

	engine::graph::Node viewer;
	viewer.Name = Name("viewer");
	viewer.Kind = Name("viewer");
	viewer.Reads = {colour};
	viewer.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(graph.AddNode(viewer).IsValid());

	CompiledGraph compiled;
	Name offender;
	INFO("offending node: " << std::string(offender.Text()));
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	std::vector<std::string> ran;
	std::vector<std::string> wired;

	PassTable table;
	Record(table, graph, ran);
	table.Set(Name("viewer"), [&](const RunContext &context) {
		for (const engine::graph::ResourceId read : context.Reads) {
			const engine::graph::ResourceDesc *desc = graph.FindResource(read);
			if (desc != nullptr) {
				wired.emplace_back(desc->Name.Text());
			}
		}
		ran.emplace_back("viewer");
		return true;
	});

	GraphRunner runner{table};
	REQUIRE(runner.Run(RunContext{}) == false);

	GraphRunner live{table};
	REQUIRE(graph.Execute(compiled, live, size_t{1}));

	// The resource it was wired to, and only that one.
	REQUIRE(wired.size() == 1);
	CHECK(wired[0] == "colour");

	// **And it runs after what it is looking at.** A viewer scheduled before the
	// pass that writes its source would download the previous frame's image
	// while claiming to show this one — which is the staleness `PendingReadback`
	// reports honestly and this would hide.
	const auto viewerAt = std::find(ran.begin(), ran.end(), "viewer");
	const auto opaqueAt = std::find(ran.begin(), ran.end(), "opaque@0");
	REQUIRE(viewerAt != ran.end());
	REQUIRE(opaqueAt != ran.end());
	CHECK(opaqueAt < viewerAt);
}

TEST_CASE("an overdraw pipeline runs the counter and a viewer of it", "[render][graph]") {
	// **The two stage-8 nodes, wired the way somebody inspecting a frame would
	// wire them.** `overdraw` counts into its own target and `viewer` asks for
	// that target to be downloaded — which is the whole loop from "I want to see
	// overdraw" to a picture, minus the device.
	//
	// Neither is in `StandardGraph`, and neither should be: one pays for a
	// second pass over every instance and the other pays for a readback, every
	// frame, for a number nobody asked for. They exist to be added to a pipeline
	// somebody authored.
	//
	// **Built in order rather than appended to the standard frame**, which is
	// the thing worth knowing about authoring one. Appending a `View` node to
	// `StandardGraph` is refused with `SharedBetweenViews`: its `Frame`-scoped
	// tail — `overlay`, `interface` — is already declared, and a per-view node
	// after them would mean "every view, then the panels, then every view
	// again". Declaration order is the order, so an authored pipeline is
	// declared in it.
	RenderGraph graph;

	const engine::graph::ResourceId counts =
		graph.AddResource({.Name = Name("overdraw"), .Kind = engine::graph::ResourceKind::Colour});
	REQUIRE(counts.IsValid());

	engine::graph::Node counter;
	counter.Name = Name("overdraw");
	counter.Kind = Name("overdraw");
	counter.Writes = {counts};
	counter.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(counter).IsValid());

	engine::graph::Node viewer;
	viewer.Name = Name("viewer");
	viewer.Kind = Name("viewer");
	viewer.Reads = {counts};
	viewer.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(graph.AddNode(viewer).IsValid());

	CompiledGraph compiled;
	Name offender;
	INFO("offending node: " << std::string(offender.Text()));
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	std::vector<std::string> ran;
	PassTable table;
	Record(table, graph, ran);

	GraphRunner runner{table};
	REQUIRE(graph.Execute(compiled, runner, size_t{2}));

	// **The counter is per view and the viewer is not.** Two views count twice;
	// the download happens once, at the end, because there is one panel.
	CHECK(std::count(ran.begin(), ran.end(), "overdraw@0") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "overdraw@1") == 1);
	CHECK(std::count(ran.begin(), ran.end(), "viewer") == 1);

	// **And the viewer runs after the thing it is looking at**, which is what
	// makes the picture this frame's rather than the last one's.
	const auto viewerAt = std::find(ran.begin(), ran.end(), "viewer");
	const auto lastCount = std::find(ran.begin(), ran.end(), "overdraw@1");
	REQUIRE(viewerAt != ran.end());
	REQUIRE(lastCount != ran.end());
	CHECK(lastCount < viewerAt);
}
