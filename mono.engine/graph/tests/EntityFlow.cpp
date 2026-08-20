// Which instances a pass draws, as a value that flows between nodes.
//
// **This is the half of a frame that was never in the graph**, and the whole
// point of putting it there is that it becomes arithmetic somebody can check.
// Culling and ordering used to be a fixed sequence inside the largest function
// in the engine; a suite could reach neither. Here every one of them is a
// function over spans.
//
// **What each case is defending is composition.** A filter that only worked on
// the whole world, or that quietly renumbered its output, would look correct in
// isolation and fall apart the moment two of them were wired in a row - which is
// exactly what a node editor invites somebody to do.

#include <engine/graph/EntityFlow.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.entityflow")

using engine::core::Name;
using engine::core::Vector3;
using engine::graph::AllEntities;
using engine::graph::EntityFlow;
using engine::graph::FilterByDistance;
using engine::graph::FilterByTag;
using engine::graph::Node;
using engine::graph::NodeParameter;
using engine::graph::OrderEntities;
using engine::graph::RenderGraph;
using engine::graph::ResourceKind;
using engine::graph::RunEntityNode;
using engine::graph::Viewpoint;
using engine::graph::Viewpoints;
using engine::scene::DrawInstance;

namespace {
	DrawInstance At(float x, uint32_t tags = 0, float transparency = 0.0f) {
		DrawInstance instance;
		instance.Frame.Position = Vector3{x, 0.0f, 0.0f};
		instance.TagMask = tags;
		instance.Transparency = transparency;
		return instance;
	}

	std::vector<uint32_t> Vec(std::span<const uint32_t> from) {
		return {from.begin(), from.end()};
	}
}

TEST_CASE("a flow carries named lists between nodes", "[graph]") {
	EntityFlow flow;
	CHECK(flow.Count() == 0);
	CHECK_FALSE(flow.Has(Name("visible")));

	const std::vector<uint32_t> some{3, 1, 2};
	flow.Set(Name("visible"), some);

	CHECK(flow.Has(Name("visible")));
	CHECK(Vec(flow.Get(Name("visible"))) == some);
	CHECK(flow.Count() == 1);

	// **Writing a name again replaces it rather than appending.** A node run
	// twice - which is every per-view node in a two-view frame - must not leave
	// the first run's contents in front of the second's.
	flow.Set(Name("visible"), std::vector<uint32_t>{9});
	CHECK(Vec(flow.Get(Name("visible"))) == std::vector<uint32_t>{9});
	CHECK(flow.Count() == 1);
}

TEST_CASE("a list nothing wrote is empty rather than an error", "[graph]") {
	// **The behaviour a mis-wired pipeline gets.** A pass reading a list nothing
	// filled draws nothing, which reads as a black frame - visibly wrong and
	// obviously the pipeline's fault. The alternative, treating absent as "draw
	// everything", is a frame that looks right and is not.
	const EntityFlow flow;

	CHECK(flow.Get(Name("nobody")).empty());
	CHECK_FALSE(flow.Has(Name("nobody")));
}

TEST_CASE("a flow tells absent apart from empty", "[graph]") {
	// Same picture, different fault: "nothing filled this" is a wiring mistake
	// and "the filter rejected everything" is a scene. A diagnostic that could
	// not tell them apart would send somebody to the wrong place.
	EntityFlow flow;
	flow.Set(Name("visible"), std::vector<uint32_t>{});

	CHECK(flow.Has(Name("visible")));
	CHECK(flow.Get(Name("visible")).empty());
	CHECK_FALSE(flow.Has(Name("other")));
}

TEST_CASE("clearing a flow forgets a view's lists and keeps its storage", "[graph]") {
	EntityFlow flow;
	flow.Set(Name("a"), std::vector<uint32_t>{1, 2, 3});
	flow.Set(Name("b"), std::vector<uint32_t>{4});
	REQUIRE(flow.Count() == 2);

	flow.Clear();

	// **Cleared between views, because an index means nothing without the draw
	// list it indexes.** Two views of one world have the same geometry and
	// different frustums, so view two must not inherit view one's survivors.
	CHECK(flow.Count() == 0);
	CHECK_FALSE(flow.Has(Name("a")));
	CHECK(flow.Get(Name("a")).empty());

	// And it is usable again immediately.
	flow.Set(Name("a"), std::vector<uint32_t>{7});
	CHECK(Vec(flow.Get(Name("a"))) == std::vector<uint32_t>{7});
}

TEST_CASE("the source node lists every instance in order", "[graph]") {
	std::vector<uint32_t> all;
	AllEntities(4, all);
	CHECK(all == std::vector<uint32_t>{0, 1, 2, 3});

	AllEntities(0, all);
	CHECK(all.empty());
}

TEST_CASE("a tag filter keeps what matches and everything when unset", "[graph]") {
	const std::vector<DrawInstance> instances{At(0.0f, 0b001), At(1.0f, 0b010), At(2.0f, 0b011)};
	std::vector<uint32_t> all;
	AllEntities(instances.size(), all);

	std::vector<uint32_t> matched;
	CHECK(FilterByTag(instances, all, 0b010, matched) == 2);
	CHECK(matched == std::vector<uint32_t>{1, 2});

	// **Zero keeps everything**, because an unset filter is a node somebody
	// dropped in and has not configured. Reading it as "match nothing" turns
	// that into a black frame with no explanation.
	CHECK(FilterByTag(instances, all, 0, matched) == 3);
	CHECK(matched == all);
}

TEST_CASE("filters compose, and each sees only what the last one left", "[graph]") {
	// **The property the whole design turns on.** A node editor invites somebody
	// to wire two filters in a row; if either renumbered its output or looked at
	// the world rather than at its input, the second would silently undo the
	// first.
	const std::vector<DrawInstance> instances{At(0.0f, 0b1), At(10.0f, 0b1), At(1.0f, 0b10), At(2.0f, 0b1)};

	std::vector<uint32_t> all;
	AllEntities(instances.size(), all);

	std::vector<uint32_t> tagged;
	REQUIRE(FilterByTag(instances, all, 0b1, tagged) == 3);
	CHECK(tagged == std::vector<uint32_t>{0, 1, 3});

	// Now distance, over the survivors only. Index 2 is within the radius and
	// must not come back - the tag filter already rejected it.
	std::vector<uint32_t> near;
	CHECK(FilterByDistance(instances, tagged, Vector3{0.0f, 0.0f, 0.0f}, 5.0f, near) == 2);
	CHECK(near == std::vector<uint32_t>{0, 3});
}

TEST_CASE("every filter reads its input and not the world", "[graph]") {
	// **Found by mutation.** The composition case above wires tag-then-distance,
	// and the tag filter's input there is *everything* - so a version of it that
	// ignored its input and walked the world gave the same answer and the case
	// stayed green. Every filter needs one of these with a genuine subset going
	// in, or "reads its input" is only checked for whichever runs second.
	const std::vector<DrawInstance> instances{At(0.0f, 0b1), At(1.0f, 0b1), At(2.0f, 0b1), At(3.0f, 0b1)};

	// A subset that is not the whole world and not a prefix of it.
	const std::vector<uint32_t> some{1, 3};

	std::vector<uint32_t> out;

	CHECK(FilterByTag(instances, some, 0b1, out) == 2);
	CHECK(out == some);

	CHECK(FilterByDistance(instances, some, Vector3{}, 100.0f, out) == 2);
	CHECK(out == some);

	// And the unconfigured shortcuts must copy the input rather than the world,
	// which is the other half of the same mistake.
	CHECK(FilterByTag(instances, some, 0, out) == 2);
	CHECK(out == some);

	CHECK(FilterByDistance(instances, some, Vector3{}, 0.0f, out) == 2);
	CHECK(out == some);
}

TEST_CASE("a distance filter keeps everything when it is not configured", "[graph]") {
	const std::vector<DrawInstance> instances{At(0.0f), At(100.0f)};
	std::vector<uint32_t> all;
	AllEntities(instances.size(), all);

	std::vector<uint32_t> near;
	CHECK(FilterByDistance(instances, all, Vector3{}, 0.0f, near) == 2);
	CHECK(near == all);

	CHECK(FilterByDistance(instances, all, Vector3{}, -1.0f, near) == 2);
	CHECK(near == all);
}

TEST_CASE("ordering a list puts the opaque first and the blended back to front", "[graph]") {
	// Two opaque and two blended, the blended ones deliberately given in the
	// wrong order so the sort has something to do.
	const std::vector<DrawInstance> instances{
		At(0.0f, 0, 0.0f),
		At(1.0f, 0, 0.5f),
		At(2.0f, 0, 0.0f),
		At(9.0f, 0, 0.5f),
	};

	std::vector<uint32_t> all;
	AllEntities(instances.size(), all);

	std::vector<uint32_t> ordered;
	const size_t opaque = OrderEntities(instances, all, Vector3{}, ordered);

	REQUIRE(opaque == 2);
	REQUIRE(ordered.size() == 4);

	// Opaque head, in the order the world gave it - which is what makes an
	// opaque scene replay.
	CHECK(ordered[0] == 0);
	CHECK(ordered[1] == 2);

	// Blended tail, farthest first.
	CHECK(ordered[2] == 3);
	CHECK(ordered[3] == 1);
}

TEST_CASE("ordering a filtered list orders only what it was given", "[graph]") {
	// The composition case again, for the node that is not a filter: the sort
	// must not reach past its input and re-add what a filter removed.
	const std::vector<DrawInstance> instances{At(0.0f, 0b1, 0.0f), At(1.0f, 0b10, 0.5f), At(2.0f, 0b1, 0.5f)};

	const std::vector<uint32_t> tagged{0, 2};

	std::vector<uint32_t> ordered;
	const size_t opaque = OrderEntities(instances, tagged, Vector3{}, ordered);

	CHECK(opaque == 1);
	CHECK(ordered == std::vector<uint32_t>{0, 2});
}

TEST_CASE("a list naming an instance that is gone loses an object, not the process", "[graph]") {
	// **A list is whatever a chain of filter nodes produced.** One of them naming
	// an index past the end is a mis-wired pipeline, and the honest failure is a
	// missing object rather than a read off the end of the draw list.
	const std::vector<DrawInstance> instances{At(0.0f, 0b1)};
	const std::vector<uint32_t> stale{0, 99};

	std::vector<uint32_t> matched;
	CHECK(FilterByTag(instances, stale, 0b1, matched) == 1);
	CHECK(matched == std::vector<uint32_t>{0});

	std::vector<uint32_t> near;
	CHECK(FilterByDistance(instances, stale, Vector3{}, 100.0f, near) == 1);

	// The sort keeps it rather than dropping it, and puts it last: dropping
	// silently would make a count disagree with a draw.
	std::vector<uint32_t> ordered;
	OrderEntities(instances, stale, Vector3{}, ordered);
	CHECK(ordered.size() == 2);
	CHECK(ordered[0] == 0);
}

TEST_CASE("what a filter leaves is what a pass would draw", "[graph]") {
	// **The end of the chain, as arithmetic.** The renderer builds its camera
	// range from whatever list the geometry pass is wired to: the survivors in
	// the order they were left, `DrawOrder` the identity, and the opaque count
	// taken by walking until the first blended instance. That last step is the
	// one worth pinning - it replaces a sort that already ran, and it is only
	// correct because `order-draw` puts the opaque head first.
	const std::vector<DrawInstance> instances{
		At(0.0f, 0b1, 0.0f),
		At(5.0f, 0b10, 0.0f),
		At(1.0f, 0b1, 0.5f),
		At(2.0f, 0b1, 0.0f),
	};

	std::vector<uint32_t> all;
	AllEntities(instances.size(), all);

	std::vector<uint32_t> tagged;
	REQUIRE(FilterByTag(instances, all, 0b1, tagged) == 3);

	std::vector<uint32_t> ordered;
	const size_t opaque = OrderEntities(instances, tagged, Vector3{}, ordered);

	// Instance 1 is gone: the tag filter rejected it, and nothing downstream
	// brings it back. That is the whole point - a filter changes the frame.
	CHECK(ordered.size() == 3);
	CHECK(std::find(ordered.begin(), ordered.end(), 1u) == ordered.end());

	// Opaque head first, so counting up to the first blended instance gives the
	// same answer the sort did.
	size_t counted = 0;
	for (const uint32_t index : ordered) {
		if (engine::scene::IsTransparent(instances[index])) {
			break;
		}
		counted++;
	}
	CHECK(counted == opaque);
	CHECK(counted == 2);
}

TEST_CASE("authored entity nodes execute as one composable flow", "[graph][entity-flow]") {
	RenderGraph graph;
	const auto resource = [&graph](const char *name, ResourceKind kind) {
		return graph.AddResource({.Name = Name(name), .Kind = kind});
	};
	const auto camera = resource("camera", ResourceKind::Camera);
	const auto all = resource("all", ResourceKind::Entities);
	const auto tagged = resource("tagged", ResourceKind::Entities);
	const auto near = resource("near", ResourceKind::Entities);
	const auto ordered = resource("ordered", ResourceKind::Entities);

	const std::vector<DrawInstance> instances{
		At(1.0f, 0b01, 0.0f),
		At(10.0f, 0b01, 0.0f),
		At(2.0f, 0b10, 0.0f),
		At(2.0f, 0b01, 0.5f),
	};
	EntityFlow entities;
	Viewpoints viewpoints;
	Viewpoint fallback;

	Node cameraNode{.Name = Name("camera"), .Kind = Name("camera"), .Writes = {camera}};
	CHECK(RunEntityNode(graph, cameraNode, instances, fallback, 1.0f, entities, viewpoints).Handled);

	Node source{.Name = Name("entities"), .Kind = Name("entities"), .Writes = {all}};
	CHECK(RunEntityNode(graph, source, instances, fallback, 1.0f, entities, viewpoints).Count == 4);

	Node tag{
		.Name = Name("characters"),
		.Kind = Name("filter-tag"),
		.Reads = {all},
		.Writes = {tagged},
		.Parameters = {NodeParameter{Name("mask"), "0x1"}},
	};
	CHECK(RunEntityNode(graph, tag, instances, fallback, 1.0f, entities, viewpoints).Count == 3);
	CHECK(Vec(entities.Get(Name("tagged"))) == std::vector<uint32_t>{0, 1, 3});

	// Reading and writing one list is legal and must not clear the source before
	// the filter sees it.
	tag.Name = Name("characters-again");
	tag.Reads = {tagged};
	tag.Writes = {tagged};
	CHECK(RunEntityNode(graph, tag, instances, fallback, 1.0f, entities, viewpoints).Count == 3);

	Node distance{
		.Name = Name("nearby"),
		.Kind = Name("cull-distance"),
		.Reads = {tagged, camera},
		.Writes = {near},
		.Parameters = {NodeParameter{Name("radius"), "5"}},
	};
	CHECK(RunEntityNode(graph, distance, instances, fallback, 1.0f, entities, viewpoints).Count == 2);
	CHECK(Vec(entities.Get(Name("near"))) == std::vector<uint32_t>{0, 3});

	Node order{
		.Name = Name("order"),
		.Kind = Name("order-draw"),
		.Reads = {near, camera},
		.Writes = {ordered},
	};
	const engine::graph::EntityNodeRun result =
		RunEntityNode(graph, order, instances, fallback, 1.0f, entities, viewpoints);
	CHECK(result.Handled);
	CHECK(result.Ordered);
	CHECK(result.Opaque == 1);
	CHECK(result.Count == 2);
	CHECK(Vec(entities.Get(Name("ordered"))) == std::vector<uint32_t>{0, 3});
}
