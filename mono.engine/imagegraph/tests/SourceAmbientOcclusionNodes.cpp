#include "NodeHarness.hpp"
#include "nodes/Processor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_ambient_occlusion")
using namespace engine::imagegraph;
namespace {
	Document OcclusionGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"source",
			 "image.checker",
			 "",
			 {},
			 {{"width", int64_t{3}},
			  {"height", int64_t{3}},
			  {"size", 1. / 3},
			  {"position", Vector2{0, 0}},
			  {"color_1", Colour{255, 255, 255, 255}},
			  {"color_2", Colour{0, 0, 0, 255}}}},
			{"ao",
			 "pc.ambient_occlusion",
			 "",
			 {},
			 {{"height", 2.},
			  {"height_unit", EnumValue{0}},
			  {"intensity", 1.},
			  {"interpolate", EnumValue{2}},
			  {"oversample", EnumValue{3}},
			  {"attribute_color_depth", EnumValue{5}}}}
		};
		doc.Links = {{"source", "image", "ao", "height_map"}};
		doc.Outputs = {{"image", "ao", "surface_out"}, {"source", "source", "image"}};
		return doc;
	}
	Image OcclusionImage(const Document &doc, const std::string &output = "image") {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto status = Evaluate(doc, plan, output, image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	constexpr double PIXEL_SWEEP_AMBIENT = .8361882729570022;
	constexpr double UNIFORM_SWEEP_AMBIENT = .8354046165539246;
	void SetOcclusion(Node &node, std::string port, Value value) {
		for (auto &existing : node.Values)
			if (existing.Port == port) {
				existing.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::move(port), std::move(value)});
	}
} // namespace
TEST_CASE(
	"Source AO angular orders match an independent bilinear checker integral", "[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	const auto source = OcclusionImage(doc, "source");
	REQUIRE(detail::ReadPixel(source, 1, 1) == detail::Rgba{0, 0, 0, 1});
	REQUIRE(detail::ReadPixel(source, 0, 1) == detail::Rgba{1, 1, 1, 1});
	const auto image = OcclusionImage(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA32Float);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6));
	REQUIRE(detail::ReadPixel(image, 1, 1)[3] == 1);
	SetOcclusion(doc.Nodes[1], "pixel_sweep", false);
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] == Catch::Approx(UNIFORM_SWEEP_AMBIENT).margin(2e-6)
	);
	// The 65th direction is new in Pixel Sweep and repeats zero in Uniform Sweep;
	// both divide by64.
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(OcclusionImage(restored).Pixels == OcclusionImage(doc).Pixels);
}
TEST_CASE(
	"Source AO flat and zero-height reductions retain alpha and inactive "
	"format",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	doc.Nodes[0] = {
		"source",
		"image.solid",
		"",
		{},
		{{"width", int64_t{3}}, {"height", int64_t{3}}, {"colour", Colour{40, 80, 120, 128}}}
	};
	auto image = OcclusionImage(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == 1);
	REQUIRE(detail::ReadPixel(image, 1, 1)[3] == Catch::Approx(128. / 255).margin(1e-7));
	SetOcclusion(doc.Nodes[1], "height", 0.);
	REQUIRE(OcclusionImage(doc).Pixels == image.Pixels);
	SetOcclusion(doc.Nodes[1], "height", -3.);
	REQUIRE(OcclusionImage(doc).Pixels == image.Pixels);
	SetOcclusion(doc.Nodes[1], "active", false);
	image = OcclusionImage(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
	REQUIRE(image.Pixels == OcclusionImage(doc, "source").Pixels);
}
TEST_CASE("Source AO multiply and subtract preserve the source alpha and HDR", "[source_ambient_occlusion]") {
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "blend_original", true);
	SetOcclusion(doc.Nodes[1], "blend_strength", .25);
	SetOcclusion(doc.Nodes[1], "blendmode", EnumValue{1});
	auto image = OcclusionImage(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(-PIXEL_SWEEP_AMBIENT * .25).margin(2e-6));
	REQUIRE(detail::ReadPixel(image, 1, 1)[3] == 1);
	SetOcclusion(doc.Nodes[1], "blendmode", EnumValue{0});
	REQUIRE(detail::ReadPixel(OcclusionImage(doc), 1, 1) == detail::Rgba{0, 0, 0, 1});
	SetOcclusion(doc.Nodes[1], "blend_original", false);
	SetOcclusion(doc.Nodes[1], "attribute_color_depth", EnumValue{3});
	image = OcclusionImage(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
	REQUIRE(image.Pixels[4 * (1 + image.Width)] == 213);
}
TEST_CASE("Source AO mapped controls use mean RGB without map alpha", "[source_ambient_occlusion]") {
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "height_mapped", true);
	SetOcclusion(doc.Nodes[1], "height_map_range", Vector2{2, 4});
	SetOcclusion(doc.Nodes[1], "intensity_mapped", true);
	SetOcclusion(doc.Nodes[1], "intensity_map_range", Vector2{0, 2});
	doc.Nodes.push_back(
		{"height_control",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}}
	);
	doc.Nodes.push_back(
		{"intensity_control",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{128, 128, 128, 0}}}}
	);
	doc.Links.push_back({"height_control", "image", "ao", "height_map_2"});
	doc.Links.push_back({"intensity_control", "image", "ao", "intensity_map"});
	auto image = OcclusionImage(doc);
	const double expected = 1 - (1 - PIXEL_SWEEP_AMBIENT) * (256. / 255);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(expected).margin(2e-6));
	// Mapped ranges stay physical endpoint pairs even without an attached control
	// surface.
	doc.Links.resize(2);
	REQUIRE(detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] == 1);
}
TEST_CASE(
	"Source AO radial intensity curve uses the source shader curve evaluator", "[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	Curve curve;
	curve.Header = {0, 1, 0, 0, 1};
	curve.Anchors = {{0, 0, 0, .5, 0, 0}, {0, 0, 1, .5, 0, 0}};
	SetOcclusion(doc.Nodes[1], "intensity_curved", true);
	SetOcclusion(doc.Nodes[1], "intensity_curve", curve);
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] ==
		Catch::Approx((1 + PIXEL_SWEEP_AMBIENT) / 2).margin(2e-6)
	);
}
TEST_CASE(
	"Source AO mapped endpoint rows survive actual graph evaluation and "
	"persistence",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "intensity_mapped", true);
	doc.Nodes.push_back(
		{"ranges",
		 "pc.array",
		 "",
		 {},
		 {},
		 {{"input_0", ValueType::Vector2, Vector2{0, 0}}, {"input_1", ValueType::Vector2, Vector2{1, 1}}}}
	);
	doc.Links.push_back({"ranges", "array", "ao", "intensity"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	REQUIRE(detail::ReadPixel(images.Images[0], 1, 1)[0] == 1);
	REQUIRE(detail::ReadPixel(images.Images[1], 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6));
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", EvaluationRequest{}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images.size() == 2);
	for (size_t row = 0; row < 2; row++)
		REQUIRE(replay.Images[row].Pixels == images.Images[row].Pixels);
}
TEST_CASE(
	"Source AO Reference height observes the original first surface "
	"array width",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	ArrayValue sizes;
	sizes.ElementType = ValueType::Scalar;
	sizes.Nested = {{3., 3.}, {7., 7.}};
	doc.Junctions = {{"sizes", "", ValueType::Array, sizes}};
	doc.Nodes[0] = {
		"source",
		"pc.shape_ellipse",
		"",
		{},
		{{"attribute_process", true},
		 {"dimension_unit", EnumValue{0}},
		 {"center", Vector2{1.5, 1.5}},
		 {"center_unit", EnumValue{0}},
		 {"half_size", Vector2{.49, .49}},
		 {"half_size_unit", EnumValue{0}}}
	};
	doc.Links.push_back({"sizes", "value", "source", "dimension"});
	doc.Links[0].FromPort = "surface_out";
	doc.Outputs[1].Port = "surface_out";
	SetOcclusion(doc.Nodes[1], "height", 2. / 3);
	SetOcclusion(doc.Nodes[1], "height_unit", EnumValue{1});
	SetOcclusion(doc.Nodes[1], "interpolate", EnumValue{1});
	SetOcclusion(doc.Nodes[1], "oversample", EnumValue{1});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	REQUIRE(images.Images[0].Width == 3);
	REQUIRE(images.Images[1].Width == 7);
	for (const auto &image : images.Images) {
		REQUIRE(detail::ReadPixel(image, 0, 1)[0] == Catch::Approx(.90625).margin(1e-7));
		REQUIRE(detail::ReadPixel(image, 0, 1)[3] == 0);
	}
}
TEST_CASE(
	"Source AO aggregate work and shader curve refusals preserve caller "
	"storage",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	const Image sentinel{1, 1, {5, 6, 7, 8}};
	Image image = sentinel;
	Plan plan;
	Diagnostic diagnostic;
	Curve curve;
	curve.Header = {0, 0, 0, 0, 1};
	curve.Anchors = {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0}};
	SetOcclusion(doc.Nodes[1], "intensity_curved", true);
	SetOcclusion(doc.Nodes[1], "intensity_curve", curve);
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "intensity_curve");
	REQUIRE(image == sentinel);
	doc = OcclusionGraph();
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", EvaluationRequest{}, image, diagnostic, 1) == Status::LimitExceeded);
	REQUIRE(image == sentinel);
	doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[0], "width", int64_t{512});
	SetOcclusion(doc.Nodes[0], "height", int64_t{512});
	SetOcclusion(doc.Nodes[1], "height", 1.);
	SetOcclusion(doc.Nodes[1], "intensity", ArrayValue{ValueType::Scalar, {0., 1.}});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray result;
	result.Images = {sentinel};
	result.Items = {ImageArrayItem{size_t{0}}};
	REQUIRE(
		EvaluateArray(doc, plan, "image", EvaluationRequest{}, result, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(diagnostic.Port == "height");
	REQUIRE(result.Images.size() == 1);
	REQUIRE(result.Images[0] == sentinel);
}
TEST_CASE(
	"Source AO scalar surface getters bypass units and obey mapped row depth", "[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	doc.Nodes.push_back(
		{"dimensions",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{4}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links.push_back({"dimensions", "image", "ao", "height"});
	SetOcclusion(doc.Nodes[1], "height_unit", EnumValue{1});
	const auto evaluate = [&]() {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		return images;
	};
	auto images = evaluate();
	REQUIRE(detail::ReadPixel(images.Images[0], 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6));
	REQUIRE(detail::ReadPixel(images.Images[1], 1, 1)[0] == Catch::Approx(.6014213290669324).margin(2e-6));
	SetOcclusion(doc.Nodes[1], "height_mapped", true);
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6)
	);
	SetOcclusion(doc.Nodes[1], "height_mapped", false);
	SetOcclusion(doc.Nodes[1], "height_unit", EnumValue{0});
	doc.Links.back().ToPort = "intensity";
	images = evaluate();
	REQUIRE(
		detail::ReadPixel(images.Images[0], 1, 1)[0] ==
		Catch::Approx(1 - (1 - PIXEL_SWEEP_AMBIENT) * 2).margin(2e-6)
	);
	REQUIRE(
		detail::ReadPixel(images.Images[1], 1, 1)[0] ==
		Catch::Approx(1 - (1 - PIXEL_SWEEP_AMBIENT) * 4).margin(2e-6)
	);
	SetOcclusion(doc.Nodes[1], "intensity_mapped", true);
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] ==
		Catch::Approx(1 - (1 - PIXEL_SWEEP_AMBIENT) * 2).margin(2e-6)
	);
	SetOcclusion(doc.Nodes[1], "intensity_mapped", false);
	ArrayValue sizes;
	sizes.ElementType = ValueType::Scalar;
	sizes.Nested = {{3., 3.}, {7., 7.}};
	doc.Junctions = {{"sizes", "", ValueType::Array, sizes}};
	doc.Nodes.back() = {
		"dimensions",
		"pc.shape_ellipse",
		"",
		{},
		{{"attribute_process", true}, {"dimension_unit", EnumValue{0}}}
	};
	doc.Links.back().FromPort = "surface_out";
	doc.Links.push_back({"sizes", "value", "dimensions", "dimension"});
	images = evaluate();
	// Float.getValue passes the whole image array to surface_get_dimension,
	// producing scalar rows1,1.
	for (const auto &image : images.Images)
		REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6));
}
TEST_CASE(
	"Source AO disabled processing uploads numeric pairs once and uses "
	"the first unmapped component",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "attribute_process", false);
	SetOcclusion(doc.Nodes[1], "height_unit", EnumValue{1});
	SetOcclusion(doc.Nodes[1], "height", ArrayValue{ValueType::Scalar, {2. / 3, 4. / 3}});
	SetOcclusion(doc.Nodes[1], "intensity", ArrayValue{ValueType::Scalar, {1., 2.}});
	const auto image = OcclusionImage(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(OcclusionImage(restored) == image);
	// Mapping changes the effective intensity only when its surface exists. Alpha
	// is ignored.
	SetOcclusion(doc.Nodes[1], "intensity_mapped", true);
	doc.Nodes.push_back(
		{"map",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 0}}}}
	);
	doc.Links.push_back({"map", "image", "ao", "intensity_map"});
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] ==
		Catch::Approx(1 - (1 - PIXEL_SWEEP_AMBIENT) * 2).margin(3e-6)
	);
	SetOcclusion(doc.Nodes[1], "height_mapped", true);
	doc.Links.push_back({"map", "image", "ao", "height_map_2"});
	REQUIRE(
		detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] ==
		Catch::Approx(1 - (1 - .6014213290669324) * 2).margin(5e-6)
	);
	SetOcclusion(doc.Nodes[1], "intensity", ArrayValue{ValueType::Scalar, {1., 7.}});
	// sh_sao clamps final ambient to zero before optional Blend Original.
	REQUIRE(detail::ReadPixel(OcclusionImage(doc), 1, 1)[0] == 0);
}
TEST_CASE(
	"Source AO rejects unobserved disabled uniform shapes before active "
	"or inactive publication",
	"[source_ambient_occlusion]"
) {
	for (const bool active : {false, true}) {
		for (const std::string port : {"height", "intensity"}) {
			auto doc = OcclusionGraph();
			SetOcclusion(doc.Nodes[1], "attribute_process", false);
			SetOcclusion(doc.Nodes[1], "active", active);
			SetOcclusion(doc.Nodes[1], port, ArrayValue{ValueType::Scalar, {1., 2., 3.}});
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
			const Image sentinel{1, 1, {4, 5, 6, 7}};
			Image result = sentinel;
			REQUIRE(Evaluate(doc, plan, "image", result, diagnostic) == Status::UnsupportedExecution);
			REQUIRE(diagnostic.Port == port);
			REQUIRE(result == sentinel);
		}
	}
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "attribute_process", false);
	SetOcclusion(doc.Nodes[1], "active", false);
	SetOcclusion(doc.Nodes[1], "height", ArrayValue{ValueType::Scalar, {2., 4.}});
	SetOcclusion(doc.Nodes[1], "intensity", ArrayValue{ValueType::Scalar, {1., 2.}});
	REQUIRE(OcclusionImage(doc) == OcclusionImage(doc, "source"));
}
TEST_CASE(
	"Source AO admits original later-row height and surface work before publication",
	"[source_ambient_occlusion]"
) {
	auto doc = OcclusionGraph();
	SetOcclusion(doc.Nodes[1], "active", true);
	SECTION("Later height endpoint dominates the initial zero-height row") {
		SetOcclusion(doc.Nodes[0], "width", int64_t{512});
		SetOcclusion(doc.Nodes[0], "height", int64_t{512});
		SetOcclusion(doc.Nodes[1], "height", ArrayValue{ValueType::Scalar, {0., 2.}});
	}
	SECTION("Later height-map dimensions dominate the initial three-pixel surface") {
		ArrayValue sizes;
		sizes.ElementType = ValueType::Scalar;
		sizes.Nested = {{3., 3.}, {512., 512.}};
		doc.Junctions = {{"sizes", "", ValueType::Array, sizes}};
		doc.Nodes[0] = {
			"source",
			"pc.shape_ellipse",
			"",
			{},
			{{"attribute_process", true}, {"dimension_unit", EnumValue{0}}}
		};
		doc.Links[0].FromPort = "surface_out";
		doc.Outputs[1].Port = "surface_out";
		doc.Links.push_back({"sizes", "value", "source", "dimension"});
		SetOcclusion(doc.Nodes[1], "height", 1.);
	}
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const Image sentinel{1, 1, {4, 5, 6, 7}};
	ImageArray result;
	result.Images = {sentinel};
	result.Items = {ImageArrayItem{size_t{0}}};
	REQUIRE(
		EvaluateArray(doc, plan, "image", EvaluationRequest{}, result, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(diagnostic.NodeId == "ao");
	REQUIRE(diagnostic.Port == "height");
	REQUIRE(diagnostic.Message == "AO original batch exceeds aggregate sample work");
	REQUIRE(result.Images.size() == 1);
	REQUIRE(result.Images[0] == sentinel);
}
TEST_CASE("Source AO private row admission precedes the inactive copy", "[source_ambient_occlusion]") {
	const auto *entry = FindCatalogueEntry("pc.ambient_occlusion");
	const auto executor = detail::FindExecutor("pc.ambient_occlusion");
	REQUIRE(entry);
	REQUIRE(executor);
	const Node node{"ao", "pc.ambient_occlusion", "", {}, {}};
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	const Image source{512, 512, std::vector<uint8_t>(512 * 512 * 4)};
	context.Images = {{"height_map", &source}};
	context.Values = {
		{"active", false},
		{"height_unit", EnumValue{0}},
		{"height_mapped", false},
		{"intensity_curved", false}
	};
	const Value originalHeight = ArrayValue{ValueType::Scalar, {0., 2.}};
	const Value selectedHeight = 0.;
	const std::array<std::pair<std::string_view, const Value *>, 1> original = {
		{{"height", &originalHeight}}
	};
	context.ProcessorOriginalValues = original;
	context.ValueViews = {{"height", &selectedHeight}};
	context.ProcessorCount = 2;
	const auto available = context.AvailableBytes();
	// This bypasses the public scalar Active getter only to check kernel admission ordering.
	// It does not claim source Active supports arrays or a disabled-first public graph.
	REQUIRE_FALSE(executor(context));
	REQUIRE(context.FailureCode == Status::LimitExceeded);
	REQUIRE(context.FailurePort == "height");
	REQUIRE(context.FailureMessage == "AO original batch exceeds aggregate sample work");
	REQUIRE(context.OutputImages.empty());
	REQUIRE(context.AvailableBytes() == available);
}
TEST_CASE(
	"Source AO nested first reference uses project width independently of selected surface width",
	"[source_ambient_occlusion]"
) {
	ImageArray originals;
	originals.Images.push_back(OcclusionImage(OcclusionGraph(), "source"));
	Image large{7, 7, std::vector<uint8_t>(7 * 7 * 4)};
	for (uint32_t y = 0; y < 7; ++y)
		for (uint32_t x = 0; x < 7; ++x) {
			const size_t offset = (y * 7 + x) * 4;
			for (size_t channel = 0; channel < 3; ++channel)
				large.Pixels[offset + channel] = (x + y) % 2 ? 255 : 0;
			large.Pixels[offset + 3] = 255;
		}
	originals.Images.push_back(std::move(large));
	originals.Items = {
		ImageArrayItem{std::vector<ImageArrayItem>{ImageArrayItem{size_t{0}}}}, ImageArrayItem{size_t{1}}
	};
	const auto *entry = FindCatalogueEntry("pc.ambient_occlusion");
	const auto executor = detail::FindExecutor("pc.ambient_occlusion");
	REQUIRE(entry);
	REQUIRE(executor);
	const Node node{"ao", "pc.ambient_occlusion", "", {}, {}};
	const EvaluationRequest request;
	for (size_t row = 0; row < originals.Images.size(); ++row) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ProcessorRow = row;
		context.ProcessorCount = 2;
		context.Project.SurfaceWidth = 9;
		context.Images = {{"height_map", &originals.Images[row]}};
		context.ImageArrays = {{"height_map", &originals}};
		context.Values = {
			{"height", 2. / 9},
			{"height_unit", EnumValue{1}},
			{"intensity", 1.},
			{"interpolate", EnumValue{2}},
			{"oversample", EnumValue{3}},
			{"attribute_color_depth", EnumValue{5}}
		};
		// The first nested item is a nonsurface: source getDimension(0) returns PROJ_SURF.
		// No public nested processor admission is inferred from this private getter-profile test.
		const bool executed = executor(context);
		INFO(context.FailureNodeId << ":" << context.FailurePort << ":" << context.FailureMessage);
		REQUIRE(executed);
		REQUIRE(context.OutputImages.size() == 1);
		const auto &image = context.OutputImages[0].second;
		REQUIRE(image.Width == originals.Images[row].Width);
		REQUIRE(
			detail::ReadPixel(image, image.Width / 2, image.Height / 2)[0] ==
			Catch::Approx(PIXEL_SWEEP_AMBIENT).margin(2e-6)
		);
	}
}
