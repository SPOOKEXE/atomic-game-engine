#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PipelineAdmission.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/RenderPipelineGraph.hpp>

TEST_SUITE_ID("studio.renderpipelineadmission")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")

namespace {
	engine::graph::PipelineDocument InvalidDispatchDocument() {
		using namespace engine::graph;
		using engine::core::Name;

		PipelineDocument document;
		document.Record(
			Edit{
				.Kind = EditKind::AddResource,
				.Name = Name("storage"),
				.Resource = ResourceKind::Storage,
			}
		);
		document.Record(
			Edit{
				.Kind = EditKind::AddNode,
				.Name = Name("bad-dispatch"),
				.NodeKind = Name("compute"),
			}
		);
		document.Record(Edit{.Kind = EditKind::Writes, .Target = Name("storage")});
		document.Record(
			Edit{
				.Kind = EditKind::Set,
				.Key = Name("dispatch.x"),
				.Value = "zero",
			}
		);
		return document;
	}
}

TEST_CASE("Studio exposes the renderer's full pipeline refusal", "[studio][pipeline]") {
	engine::render::Renderer renderer;
	std::string error;

	CHECK_FALSE(studio::ValidateRenderPipelineAdmission(InvalidDispatchDocument(), renderer, error));
	CHECK(error == "refused during schedule: a scheduling hint is not valid at 'bad-dispatch'");
}
