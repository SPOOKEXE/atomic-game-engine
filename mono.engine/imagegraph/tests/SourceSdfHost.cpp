#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_sdf_host")
TEST_DEPENDS("engine.imagegraph.host_capture")
TEST_CASE("RM recordings feed downstream filters and reject stale project policy", "[imagegraph][sdf-host]") {
	using namespace engine::imagegraph;
	for (const std::string type : {"pc.rm_render", "pc.rm_render_scatter", "pc.rm_cloud", "pc.rm_terrain"}) {
		DYNAMIC_SECTION(type) {
			Document document;
			document.FormatVersion = 9;
			document.Project.emplace();
			document.Project->SurfaceWidth = 8;
			document.Project->SurfaceHeight = 4;
			document.Nodes = {
				{"shape", "pc.rm_primitive", "", {}, {}},
				{"rm", type, "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{1}}}},
				{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
			};
			if (type == "pc.rm_render" || type == "pc.rm_render_scatter")
				document.Links.push_back({"shape", "sdf_object", "rm", "sdf_object"});
			document.Links.push_back({"rm", "surface_out", "invert", "image"});
			document.Outputs = {{"out", "invert", "image"}};
			Plan plan;
			Diagnostic diagnostic;
			EvaluationRequest clock;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			HostNodeCapture receipt;
			REQUIRE(PrepareHostCapture(document, plan, "rm", clock, receipt, diagnostic) == Status::Ok);
			REQUIRE(receipt.CameraPolicy);
			CHECK_FALSE(receipt.CameraRow);
			receipt.Images.push_back({"surface_out", Image{1, 1, {12, 34, 56, 255}}});
			clock.HostCaptures = std::span(&receipt, 1);
			Image result;
			const auto status = Evaluate(document, plan, "out", clock, result, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			CHECK(result.Pixels == std::vector<uint8_t>{243, 221, 199, 255});
			document.Project->SurfaceWidth = 16;
			CHECK(Evaluate(document, plan, "out", clock, result, diagnostic) == Status::InvalidValue);
			CHECK(diagnostic.Message.find("policy is stale") != std::string::npos);
		}
	}
}
