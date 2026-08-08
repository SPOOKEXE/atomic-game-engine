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
