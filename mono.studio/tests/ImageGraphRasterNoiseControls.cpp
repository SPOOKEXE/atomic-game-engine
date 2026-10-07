#include "../src/ImageGraphRasterNoiseControls.hpp"

#include "../src/ImageGraphPorts.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.raster_noise_controls")

TEST_CASE("Raster noise exposes only its result shape as a noise selector", "[studio][imagegraph]") {
	for (const auto type : {"image.noise_simplex", "pc.noise_simplex", "pc.voronoi_extra"}) {
		CHECK(studio::detail::IsImageGraphRasterNoiseSelector(type, "output_type"));
		CHECK_FALSE(studio::detail::IsImageGraphRasterNoiseSelector(type, "dimension"));
		CHECK_FALSE(studio::detail::IsImageGraphRasterNoiseSelector(type, "mode"));
	}
	CHECK_FALSE(studio::detail::IsImageGraphRasterNoiseSelector("value.noise_field", "output_type"));
	CHECK_FALSE(studio::detail::IsImageGraphRasterNoiseSelector("image.unknown", "output_type"));
	engine::imagegraph::EnumValue value{1};
	CHECK_FALSE(studio::detail::DrawImageGraphRasterNoiseChoice("image.unknown", "output_type", value));
	CHECK(value.Value == 1);
}

TEST_CASE(
	"Raster shape edits update instance ports and retain incompatible authored links", "[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"noise", "image.noise_simplex", "", {}, {{"width", int64_t{2}}, {"height", int64_t{2}}}},
		{"sample", "value.sample_noise", "", {}, {}}
	};
	document.Links = {{"noise", "field", "sample", "field"}};
	document.Outputs = {{"value", "sample", "value"}};
	Diagnostic diagnostic;
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(studio::SetImageGraphValue(document, "noise", "output_type", EnumValue{3}, diagnostic));
	const auto ports = studio::detail::ImageGraphOutputPorts(document.Nodes[0], &document);
	REQUIRE(ports.size() == 2);
	CHECK(ports[0].Type == ValueType::Image);
	CHECK(ports[1].Type == ValueType::Noise2DVector3);
	CHECK(document.Links.size() == 1);
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK_FALSE(studio::SetImageGraphValue(document, "noise", "output_type", EnumValue{4}, diagnostic));
	CHECK(diagnostic.Port == "output_type");
}

TEST_CASE("Editing an inherited raster selector explicitly overrides its base", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"base", "pc.noise_simplex", "", {}, {{"output_type", EnumValue{3}}}},
		{"child", "pc.noise_simplex", "", {}, {{"output_type", EnumValue{1}}}}
	};
	document.Nodes[1].InstanceBase = "base";
	CHECK(RasterNoiseComponents(document.Nodes[1], &document) == 3);
	Diagnostic diagnostic;
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(studio::SetImageGraphValue(document, "child", "output_type", EnumValue{2}, diagnostic));
	CHECK(document.Nodes[1].InstanceOverrides == std::vector<std::string>{"output_type"});
	CHECK(RasterNoiseComponents(document.Nodes[1], &document) == 2);
}
