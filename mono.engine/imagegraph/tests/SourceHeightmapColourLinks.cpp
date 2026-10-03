#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_heightmap_colour_links")
using namespace engine::imagegraph;

TEST_CASE(
	"Heightmap Gradient admits source Colour palettes without admitting "
	"unrelated arrays",
	"[source_heightmap_colour_links]"
) {
	for (const auto type : {"pc.gradient_sample", "pc.gradient_extract"}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"palette", type, "", {}, {}}, {"heightmap", "pc.heightmap_project_3_d", "", {}, {}}
		};
		document.Links = {{"palette", "colors", "heightmap", "height_color"}};
		document.Outputs = {{"out", "heightmap", "surface_out"}};
		Plan plan;
		Diagnostic diagnostic;
		INFO(type);
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	}
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"heightmap", "pc.heightmap_project_3_d", "", {}, {}}};
	document.Junctions = {
		{"unrelated", "", ValueType::Array, ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}}}}
	};
	document.Links = {{"unrelated", "value", "heightmap", "height_color"}};
	document.Outputs = {{"out", "heightmap", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "heightmap");
	CHECK(diagnostic.Port == "height_color");
}
