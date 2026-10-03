#include "ImageGraphComposerAdapter.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.transformgraphhost")

TEST_CASE(
	"live Transform graph provider reaches both profiles through "
	"downstream surface and mesh routes",
	"[client][transform-graph-host]"
) {
	using namespace engine::imagegraph;
	for (const auto nodeType : {"pc.3_d_transform_image", "image.transform_3d"}) {
		DYNAMIC_SECTION(nodeType) {
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"image",
				 "image.solid",
				 "",
				 {},
				 {{"width", int64_t(3)}, {"height", int64_t(2)}, {"colour", Colour{255, 255, 255, 255}}}},
				{"transform", nodeType, "", {}, {}},
				{"downstream", "pc.invert", "", {}, {}}
			};
			document.Links = {
				{"image", "image", "transform", "surface"},
				{"transform", "rendered", "downstream", "surface_in"}
			};
			document.Outputs = {{"surface", "downstream", "surface_out"}, {"mesh", "transform", "mesh"}};
			Plan plan;
			Diagnostic diagnostic;
			const auto compiled = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(compiled == Status::Ok);
			engine::render::Renderer renderer;
			client::detail::ComposerProvider provider(
				renderer,
				engine::core::Name("transform.owner"),
				nullptr,
				engine::core::Name("transform.binding")
			);
			EvaluationRequest clock{.HostProvider = &provider};
			Image image;
			CHECK(
				Evaluate(document, plan, "surface", clock, image, diagnostic) == Status::UnsupportedExecution
			);
			INFO(diagnostic.Message);
			CHECK(diagnostic.NodeId == "transform");
			CHECK(diagnostic.Message.find("renderer device") != std::string::npos);
			CHECK_FALSE(provider.Pending);
			EvaluatedValue mesh;
			CHECK(
				EvaluateValue(document, plan, "mesh", clock, mesh, diagnostic) == Status::UnsupportedExecution
			);
			CHECK(diagnostic.NodeId == "transform");
			CHECK(diagnostic.Message.find("renderer device") != std::string::npos);
		}
	}
}
