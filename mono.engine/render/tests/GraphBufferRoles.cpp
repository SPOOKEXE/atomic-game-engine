#include "GraphBufferRoles.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.graphbufferroles")
TEST_DEPENDS("engine.graph.rendergraph")

using namespace engine;

TEST_CASE(
	"tessellation buffer usage follows consuming ports after resource renames", "[render][buffer-roles]"
) {
	graph::RenderGraph graph;
	const auto vertices =
		graph.AddResource({.Name = core::Name("hard-vertices"), .Kind = graph::ResourceKind::Buffer});
	const auto indices =
		graph.AddResource({.Name = core::Name("hard-indices"), .Kind = graph::ResourceKind::Buffer});
	const auto commands =
		graph.AddResource({.Name = core::Name("hard-commands"), .Kind = graph::ResourceKind::Buffer});
	graph::Node draw;
	draw.Name = core::Name("draw");
	draw.Kind = core::Name("tessellated-draw");
	draw.Reads = {commands, vertices, indices};
	draw.ReadPorts = {core::Name("commands"), core::Name("vertices"), core::Name("indices")};
	REQUIRE(graph.AddNode(std::move(draw)).IsValid());
	CHECK(render::detail::GraphBufferRoles(graph, vertices) == render::detail::VertexBuffer);
	CHECK(render::detail::GraphBufferRoles(graph, indices) == render::detail::IndexBuffer);
	CHECK(render::detail::GraphBufferRoles(graph, commands) == render::detail::IndirectBuffer);
}

TEST_CASE("programmatic tessellation inputs keep the draw's positional roles", "[render][buffer-roles]") {
	graph::RenderGraph graph;
	const auto shared =
		graph.AddResource({.Name = core::Name("shared"), .Kind = graph::ResourceKind::Buffer});
	graph::Node draw;
	draw.Name = core::Name("draw");
	draw.Kind = core::Name("tessellated-draw");
	draw.Reads = {shared, shared, shared};
	REQUIRE(graph.AddNode(std::move(draw)).IsValid());
	CHECK(
		render::detail::GraphBufferRoles(graph, shared) ==
		(render::detail::VertexBuffer | render::detail::IndexBuffer | render::detail::IndirectBuffer)
	);
	CHECK(render::detail::GraphBufferRoles(graph, {}) == 0);
}
