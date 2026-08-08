// The check D00016 was filed for.
//
// `Renderer::Render` submits its passes from a function that knows every one of
// them by name, and `graph::StandardPipeline` is the same list as data. Keeping
// them in step used to be a sentence in `mono.engine/render/AGENTS.md` — rule 6
// out loud, a rule the build did not check. This file is the build checking it.
//
// **The count is deliberately not written down here.** It said "five" until
// v0.7 added `interface` and then said something false, which is the failure
// this file exists to prevent, one level up. The assertions compare the two
// descriptions against each other and neither against a number.
//
// **The second description is gone, and so is most of what this file used to
// check.** `render::Pass` was a hand-written enum saying the same six things in
// the same order; `Renderer::Render` walks `graph::Execute` now and the enum has
// been deleted. What is left is worth keeping: that `PassOrder` really is the
// graph's order, that the graph is runnable, and that a caller can ask which
// stages ran by name.
//
// **No device is created here and none is needed.** `PassOrder` is a list of
// names and the graph is arithmetic over names, so all of this runs anywhere.

#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.passes")

// This suite asserts against another module, so it says so. It was the one
// cross-module assertion in `render` with nothing declared — `DebugPanels.cpp`
// names `engine.core.framegraph` for the same reason.
//
// **And it is what re-runs this file when a stage is added.**
// `StandardPipeline`'s body is in `graph/src/Pipeline.cpp`, which is in no
// suite's *header* closure — a test includes the header and links the object.
// The runner signs each suite over its own module's sources as well as that
// closure, so an edit there moves `engine.graph.pipeline`, and this line is
// what carries that up to here. Without it the check would stay green through
// exactly the change it was written to catch.
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::GraphStatus;
using engine::graph::Node;
using engine::graph::NodeId;
using engine::graph::RenderGraph;
using engine::graph::StandardGraph;
using engine::render::FrameResult;
using engine::render::PassOrder;

namespace {
	// The node names, in declaration order.
	std::vector<Name> NodeNames(const RenderGraph &graph) {
		std::vector<Name> names;
		for (size_t index = 0; index < graph.Count(); index++) {
			names.push_back(graph.Find(NodeId{static_cast<uint32_t>(index + 1)})->Name);
		}
		return names;
	}
}

TEST_CASE("the renderer's passes are the standard graph's nodes", "[render][graph]") {
	// **This no longer compares two descriptions — it checks that the one
	// description arrived intact.** `render::PassOrder` used to be a second
	// hand-written list of these names and this case existed to catch the two
	// drifting apart; `DEFERRED.md` D00016 is that entry. It reads the names out
	// of `StandardGraph` now, so drift is not possible.
	//
	// Kept rather than deleted because `FrameResult::Passes` is a bitmask over
	// these positions: a node inserted in the middle of the graph moves every
	// bit after it, and a caller reading `Ran("overlay")` would get the answer
	// for a different pass. This is where that surfaces.
	const RenderGraph standard = StandardGraph();
	const std::vector<Name> nodes = NodeNames(standard);
	const auto order = PassOrder();

	INFO(
		"Renderer submits " << order.size() << " passes and StandardGraph declares " << nodes.size()
							<< " nodes. A node added to the graph needs an enumerator beside it."
	);
	REQUIRE(order.size() == nodes.size());

	// **In order, not as a set.** A frame holding the right six passes in the
	// wrong order samples a shadow map nothing has rendered, which is not a
	// crash on a GPU — it is a frame lit by whatever was in that memory.
	for (size_t index = 0; index < nodes.size(); index++) {
		INFO("position " << index);
		CHECK(order[index].Text() == nodes[index].Text());
	}
}

TEST_CASE("pass order names the six stages the standard frame has", "[render][graph]") {
	// **The names, not the count.** The count is asserted against the graph
	// above; naming them here is what catches a *rename* — a node called
	// `transparent` becoming `blended` would keep every count right and quietly
	// change what `FrameResult::Ran` answers for.
	//
	// This was the case that used to check the hand-written enum indexed its own
	// names. The enum is gone; the index space it defined is now the graph's, so
	// what is worth pinning is which name sits at which position.
	const auto order = PassOrder();

	REQUIRE(order.size() == 6);
	CHECK(order[0].Text() == "shadow");
	CHECK(order[1].Text() == "surface");
	CHECK(order[2].Text() == "opaque");
	CHECK(order[3].Text() == "transparent");
	CHECK(order[4].Text() == "overlay");
	CHECK(order[5].Text() == "interface");
}

TEST_CASE("the standard graph the renderer follows is runnable", "[render][graph]") {
	// The other half of the pairing. The case above says the renderer's passes
	// are the graph's nodes; this says the graph is worth following — no node
	// reads a resource that no earlier node wrote, and no shared node fights a
	// per-view one over the same write.
	const RenderGraph standard = StandardGraph();

	Name offender;
	INFO("offending node: " << std::string(offender.Text()));
	CHECK(standard.Validate(offender) == GraphStatus::Ok);
}

TEST_CASE("a frame result reports which passes ran", "[render]") {
	// Every pass is conditional, so "ran" is a different question from "exists"
	// and the draw-call count cannot answer it — the shadow pass and the overlay
	// pass are one draw each and are indistinguishable from there.
	FrameResult result;

	for (const Name &kind : PassOrder()) {
		CHECK_FALSE(result.Ran(kind));
	}

	result.Passes = static_cast<uint8_t>((1u << 2) | (1u << 4));

	CHECK(result.Ran(Name("opaque")));
	CHECK(result.Ran(Name("overlay")));

	// The interesting one: a frame that drew but cast no shadows, which is what
	// a scene with no casters looks like and is not a bug.
	CHECK_FALSE(result.Ran(Name("shadow")));
	CHECK_FALSE(result.Ran(Name("surface")));
	CHECK_FALSE(result.Ran(Name("transparent")));

	// **A name the frame does not have is false, not an error.** Asking whether
	// a pass this renderer has never had ran is a fair question, and the honest
	// answer is no rather than a crash or a bit belonging to something else.
	CHECK_FALSE(result.Ran(Name("depth-prepass")));
	CHECK_FALSE(result.Ran(Name{}));
}

// The other half of D00016's neighbourhood: a decision that was recorded and
// not enforced.
//
// v0.7 decided that a studio with several viewports draws them one after
// another — the passes share one command buffer and one device, so parallel
// recording would serialise at submit and would cost the ordering that makes a
// docked viewport show *this* frame. That went into `ROADMAP.md` and nothing
// checked it, which is the shape of every stale claim this repository has found.
//
// **No device is created here.** The owner is claimed by the constructor and
// re-claimed by `Initialise`, precisely so the contract can be exercised on a
// build machine with no GPU. What cannot be asserted is the abort itself —
// `RequireOwningThread` calls `std::abort`, on purpose, and a test that survived
// it would be testing something else.
TEST_CASE("a renderer is owned by one thread", "[render]") {
	engine::render::Renderer renderer;

	CHECK(renderer.IsOnOwningThread());

	bool ownedElsewhere = true;
	std::thread other([&renderer, &ownedElsewhere] { ownedElsewhere = renderer.IsOnOwningThread(); });
	other.join();

	// The assertion the contract is made of: a second thread is not the owner,
	// so `Render` from a worker is refused rather than racing the frame the main
	// thread is recording.
	CHECK_FALSE(ownedElsewhere);

	// And the owner is unchanged by having been asked from elsewhere — the check
	// is a comparison and never a claim.
	CHECK(renderer.IsOnOwningThread());
}
