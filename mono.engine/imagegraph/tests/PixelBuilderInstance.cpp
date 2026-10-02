#include "../src/PixelBuilderPayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.pixel_builder_instance")
using namespace engine::imagegraph;

TEST_CASE(
	"Pixel Builder replay dimensions override only its instance getter binding", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"prototype",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}},
		{"builder", "pc.pixel_builder", "", {}, {}}
	};
	document.Nodes.back().InstanceBase = "prototype";
	document.Outputs = {{"recipe", "builder", "dynamic_builder"}};
	Diagnostic diagnostic;
	GroupReplayState empty, local, bound;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"builder", "prototype", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "dimension"}
	};
	REQUIRE(BindGroupReplay(document, bindings, local, 1, bound, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.GroupReplay = &bound;
	request.GroupAuthoringRevision = 1;
	EvaluatedValue result;
	const auto initial = EvaluateValue(document, plan, "recipe", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(initial == Status::Ok);
	const auto &recipe = std::get<DynamicSurfaceValue>(result.Data);
	REQUIRE(recipe.Data);
	REQUIRE(recipe.Data->Groups);
	Image image;
	const auto resized = engine::imagegraph::detail::RasterizePixelBuilder(recipe, {2, 3}, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(resized == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(image.Height == 3);
	REQUIRE(recipe.Data->Groups->Replay.Binding("builder", "dimension"));
	CHECK(recipe.Data->Groups->Replay.Binding("builder", "dimension")->OwnerId == "prototype");
}
