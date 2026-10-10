#include "../src/PipelineNodeLookup.hpp"

#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <utility>

TEST_SUITE_ID("engine.render.pipeline_node_lookup")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::graph;
	using engine::render::detail::EnabledNodeIndex;
	using engine::render::detail::FindEnabledNodeByScan;
	using engine::render::detail::FindFirstNodeByScan;

	void AddNode(RenderGraph &graph, const char *name, const char *kind, bool enabled = true) {
		graph.AddNode(Node{.Name = core::Name(name), .Kind = core::Name(kind), .Enabled = enabled});
	}

	void CheckParity(const RenderGraph &graph, std::span<const core::Name> kinds) {
		EnabledNodeIndex index;
		index.Rebuild(graph);
		for (const core::Name kind : kinds) {
			CHECK(index.Enabled(graph, kind) == FindEnabledNodeByScan(graph, kind));
			CHECK(index.First(graph, kind) == FindFirstNodeByScan(graph, kind));
		}
	}
}

TEST_CASE("enabled node index remains valid after moving its graph", "[render][pipeline-node-lookup]") {
	RenderGraph graph;
	const NodeId surfaceCapture = graph.AddNode(
		Node{.Name = core::Name("surface-capture-node"), .Kind = core::Name("surface-capture")}
	);
	graph.AddNode(
		Node{.Name = core::Name("disabled-overlay"), .Kind = core::Name("overlay"), .Enabled = false}
	);
	EnabledNodeIndex index;
	index.Rebuild(graph);

	RenderGraph moved = std::move(graph);
	CHECK(index.Enabled(moved, core::Name("surface-capture")) == moved.Find(surfaceCapture));
	CHECK(index.Enabled(moved, core::Name("mirror-capture")) == moved.Find(surfaceCapture));
	CHECK(index.First(moved, core::Name("overlay")) == FindFirstNodeByScan(moved, core::Name("overlay")));
}

TEST_CASE("enabled node index preserves scan order and aliases", "[render][pipeline-node-lookup]") {
	RenderGraph graph;
	const NodeId disabledTransparent = graph.AddNode(
		Node{.Name = core::Name("disabled-transparent"), .Kind = core::Name("transparent"), .Enabled = false}
	);
	const NodeId firstEnabled =
		graph.AddNode(Node{.Name = core::Name("first-transparent"), .Kind = core::Name("transparent")});
	AddNode(graph, "second-transparent", "transparent");
	const NodeId surfaceCapture = graph.AddNode(
		Node{.Name = core::Name("surface-capture-node"), .Kind = core::Name("surface-capture")}
	);
	AddNode(graph, "disabled-shadow", "shadow", false);

	EnabledNodeIndex index;
	index.Rebuild(graph);
	CHECK(index.Enabled(graph, core::Name("transparent")) == graph.Find(firstEnabled));
	CHECK(index.First(graph, core::Name("transparent")) == graph.Find(disabledTransparent));
	CHECK(index.Enabled(graph, core::Name("surface-capture")) == graph.Find(surfaceCapture));
	CHECK(index.Enabled(graph, core::Name("mirror-capture")) == graph.Find(surfaceCapture));
	CHECK(index.Enabled(graph, core::Name("portal-capture")) == graph.Find(surfaceCapture));
	CHECK(index.Enabled(graph, core::Name("shadow")) == nullptr);
	CHECK(index.Enabled(graph, core::Name("missing")) == nullptr);
}

TEST_CASE("enabled node index matches default PBR graph lookups", "[render][pipeline-node-lookup]") {
	RenderGraph graph;
	core::Name offender;
	REQUIRE(Build(DefaultPbrDocument(), graph, offender) == PipelineDocumentStatus::Ok);
	const std::array kinds{
		core::Name("surface-capture"),
		core::Name("mirror-capture"),
		core::Name("portal-capture"),
		core::Name("overlay"),
		core::Name("transparent"),
		core::Name("transparent-layer"),
		core::Name("shadow"),
		core::Name("interface"),
		core::Name("spatial-overlay"),
		core::Name("gbuffer"),
		core::Name("depth-peel"),
		core::Name("depth-linearise"),
		core::Name("depth-validity"),
		core::Name("ssao"),
		core::Name("deferred-lighting"),
		core::Name("fog"),
		core::Name("tonemap"),
		core::Name("camera-motion"),
		core::Name("world"),
		core::Name("cull-frustum"),
		core::Name("not-installed"),
	};
	CheckParity(graph, kinds);
}

TEST_CASE("enabled node index can be rebuilt after graph replacement", "[render][pipeline-node-lookup]") {
	RenderGraph graph;
	const NodeId original =
		graph.AddNode(Node{.Name = core::Name("original"), .Kind = core::Name("custom-kind")});
	EnabledNodeIndex before;
	before.Rebuild(graph);
	CHECK(before.Enabled(graph, core::Name("custom-kind")) == graph.Find(original));

	graph.SetEnabled(original, false);
	const NodeId replacement =
		graph.AddNode(Node{.Name = core::Name("replacement"), .Kind = core::Name("custom-kind")});
	EnabledNodeIndex after;
	after.Rebuild(graph);
	CHECK(after.Enabled(graph, core::Name("custom-kind")) == graph.Find(replacement));
	CHECK(
		after.Enabled(graph, core::Name("custom-kind")) ==
		FindEnabledNodeByScan(graph, core::Name("custom-kind"))
	);
	CHECK(after.First(graph, core::Name("custom-kind")) == graph.Find(original));
}
