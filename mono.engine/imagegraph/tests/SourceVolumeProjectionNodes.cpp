#include "NodeHarness.hpp"
#include "nodes/SourceProjectionGradient.hpp"
#include "nodes/SourceProjectionMath.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_volume_projection")
using namespace engine::imagegraph;
namespace {
	Document VolumeGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"volume",
			 "pc.surface_project_volume_3_d",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_color_depth", EnumValue{5}},
			  {"density", .25},
			  {"exponent", 1.},
			  {"view_angle", Vector3{}},
			  {"scale", 2.}}}
		};
		doc.Links = {{"source", "image", "volume", "front"}};
		doc.Outputs = {{"image", "volume", "surface_out"}};
		return doc;
	}
	Image EvaluateVolume(const Document &doc) {
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image image;
		status = Evaluate(doc, plan, "image", image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void ExpectWhiteVolume(const Image &image, double density) {
		// Two occupied axial voxels: the source adds density and attenuated opacity separately.
		const double colour = 1 - std::exp(-2 * density), volume = 2 * density + colour;
		auto pixel = detail::ReadPixel(image, 0, 0);
		for (size_t c = 0; c < 3; c++)
			REQUIRE(pixel[c] == Catch::Approx(colour * volume).margin(2e-6));
		REQUIRE(pixel[3] == Catch::Approx(volume * volume).margin(2e-6));
	}
}
TEST_CASE(
	"Source volume axial density preserves additive density and normal blend through persistence",
	"[source_volume_projection]"
) {
	auto doc = VolumeGraph();
	auto image = EvaluateVolume(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA32Float);
	ExpectWhiteVolume(image, .25);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(EvaluateVolume(restored).Pixels == image.Pixels);
	doc.Nodes.back().Values.push_back({"projection", EnumValue{0}});
	ExpectWhiteVolume(EvaluateVolume(doc), .25);
	doc.Nodes.back().Values.push_back({"position", Vector3{3, 0, 0}});
	REQUIRE(detail::ReadPixel(EvaluateVolume(doc), 0, 0) == detail::Rgba{});
}
TEST_CASE(
	"Source volume original surface fallbacks and permuted face binding retain source colour",
	"[source_volume_projection]"
) {
	auto doc = VolumeGraph();
	doc.Nodes.front().Values.back().Data = Colour{0, 255, 0, 255};
	doc.Nodes.push_back(
		{"top",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	doc.Nodes.push_back(
		{"right",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 0, 255, 255}}}}
	);
	doc.Links.push_back({"top", "image", "volume", "top"});
	doc.Links.push_back({"right", "image", "volume", "right"});
	auto pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == 0);
	REQUIRE(pixel[2] == 0);
	REQUIRE(pixel[1] > 0);
	const double density = .25 / 27, colour = 1 - std::exp(-2 * density), volume = 2 * density + colour;
	REQUIRE(pixel[1] == Catch::Approx(colour * volume).margin(2e-7));
	for (const auto port : {"top", "front", "right"}) {
		doc.Links = {{"source", "image", "volume", port}};
		REQUIRE(detail::ReadPixel(EvaluateVolume(doc), 0, 0) == pixel);
	}
}
TEST_CASE(
	"Source volume selected density array rows replay without dropping surfaces", "[source_volume_projection]"
) {
	auto doc = VolumeGraph();
	doc.Junctions.push_back({"density", "", ValueType::Array, ArrayValue{ValueType::Scalar, {.25, .5}}});
	doc.Links.push_back({"density", "value", "volume", "density"});
	Plan plan;
	Diagnostic diagnostic;
	auto status = Compile(doc, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	ImageArray image;
	status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, image, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(image.Images.size() == 2);
	ExpectWhiteVolume(image.Images[0], .25);
	ExpectWhiteVolume(image.Images[1], .5);
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", EvaluationRequest{}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images.size() == 2);
	for (size_t i = 0; i < 2; i++)
		REQUIRE(replay.Images[i].Pixels == image.Images[i].Pixels);
}
TEST_CASE(
	"Source volume gradient modes level base and side texture follow accumulated impact",
	"[source_volume_projection]"
) {
	auto doc = VolumeGraph();
	Gradient gradient;
	gradient.Keys = {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}};
	doc.Nodes.back().Values.push_back({"density_color", gradient});
	auto pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	const double colour = 1 - std::exp(-.5), volume = .5 + colour;
	REQUIRE(pixel[0] == Catch::Approx(colour * volume * (1 - volume)).margin(2e-6));
	REQUIRE(pixel[1] == 0);
	REQUIRE(pixel[2] == Catch::Approx(colour * volume * volume).margin(2e-6));
	for (int64_t mode = 0; mode <= 6; mode++) {
		gradient.Mode = mode;
		doc.Nodes.back().Values.back().Data = gradient;
		REQUIRE(FiniteSurfaceSamples(EvaluateVolume(doc)));
	}
	gradient.Mode = 1;
	doc.Nodes.back().Values.back().Data = gradient;
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume).margin(2e-6));
	REQUIRE(pixel[2] == 0);
	gradient.Mode = 2;
	doc.Nodes.back().Values.back().Data = gradient;
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume * (2 - 2 * volume)).margin(2e-6));
	REQUIRE(pixel[1] == 0);
	REQUIRE(pixel[2] == Catch::Approx(colour * volume).margin(2e-6));
	gradient.Mode = 5;
	doc.Nodes.back().Values.back().Data = gradient;
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == 0);
	REQUIRE(pixel[1] == Catch::Approx(colour * volume * (4 - 4 * volume)).margin(2e-6));
	REQUIRE(pixel[2] == Catch::Approx(colour * volume).margin(2e-6));
	gradient.Mode = 4;
	doc.Nodes.back().Values.back().Data = gradient;
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume * std::pow(1 - volume, 1 / 2.2)).margin(2e-6));
	REQUIRE(pixel[2] == Catch::Approx(colour * volume * std::pow(volume, 1 / 2.2)).margin(2e-6));
	gradient.Mode = 6;
	doc.Nodes.back().Values.back().Data = gradient;
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume * (1 - volume)).margin(2e-6));
	REQUIRE(pixel[2] == Catch::Approx(colour * volume * volume).margin(2e-6));
	doc.Nodes.back().Values.back().Data = Gradient{0, {{0, {255, 255, 255, 255}}}};
	doc.Nodes.back().Values.push_back({"level", Vector2{0, 2}});
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume / 2).margin(2e-6));
	REQUIRE(pixel[3] == Catch::Approx(volume * volume / 4).margin(2e-6));
	doc.Nodes.back().Values.push_back({"base_color", Colour{128, 255, 255, 128}});
	pixel = detail::ReadPixel(EvaluateVolume(doc), 0, 0);
	REQUIRE(pixel[0] == Catch::Approx(colour * volume / 2 * std::pow(128 / 255., 2)).margin(2e-6));
	Image tex{2, 2, {255, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}};
	const auto normal = imagegraph_test::RunNode(
		"pc.surface_project_volume_3_d",
		{{"front", &tex}, {"texture_side", &tex}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{5}},
		 {"view_angle", Vector3{}},
		 {"density", .25},
		 {"exponent", 1.},
		 {"color_threshold", 1.},
		 {"interpolate", EnumValue{1}}}
	);
	INFO(normal.Message);
	REQUIRE(normal.Ok);
	const auto origin = detail::ReadPixel(normal.Output(), 0, 0);
	REQUIRE(origin[0] > 0);
	REQUIRE(origin[1] == 0);
	REQUIRE(origin[2] == 0);
}
TEST_CASE(
	"Source volume camera uses ordered inverse Euler and distinct projection controls",
	"[source_volume_projection]"
) {
	detail::ProjectionMatrix inverse;
	REQUIRE(detail::ProjectionInverseRotation({30, 45, 0}, inverse));
	detail::ProjectionVector eye, dir;
	REQUIRE(detail::ProjectionRay(inverse, {}, .5, .5, 1, 1, 60, 1, 2, eye, dir));
	REQUIRE(dir[0] == Catch::Approx(-std::sqrt(3. / 8)).margin(2e-6));
	REQUIRE(dir[1] == Catch::Approx(.5).margin(2e-6));
	REQUIRE(dir[2] == Catch::Approx(-std::sqrt(3. / 8)).margin(2e-6));
	auto doc = VolumeGraph();
	doc.Nodes.back().Values[0].Data = Vector2{5, 3};
	const auto orthographic = EvaluateVolume(doc);
	doc.Nodes.back().Values.push_back({"projection", EnumValue{0}});
	doc.Nodes.back().Values.push_back({"fov", 90.});
	const auto perspective = EvaluateVolume(doc);
	REQUIRE(perspective.Pixels != orthographic.Pixels);
	doc.Nodes.back().Values.push_back({"distance", 2.});
	REQUIRE(EvaluateVolume(doc).Pixels != perspective.Pixels);
	doc.Nodes.back().Values[7].Data = EnumValue{1};
	doc.Nodes.back().Values[6].Data = 4.;
	REQUIRE(EvaluateVolume(doc).Pixels != orthographic.Pixels);
}
TEST_CASE(
	"Source volume work and undefined boundaries preserve caller output atomically",
	"[source_volume_projection]"
) {
	const Image sentinel{1, 1, {1, 2, 3, 4}};
	auto doc = VolumeGraph();
	Plan plan;
	Diagnostic diagnostic;
	Image image = sentinel;
	doc.Nodes.back().Values.push_back({"level", Vector2{1, 1}});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "level");
	REQUIRE(image == sentinel);
	doc.Nodes.back().Values.pop_back();
	doc.Links.clear();
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", image, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "front");
	REQUIRE(image == sentinel);
	doc = VolumeGraph();
	doc.Nodes.back().Values[0].Data = Vector2{600, 600};
	doc.Junctions.push_back({"density", "", ValueType::Array, ArrayValue{ValueType::Scalar, {.25, .5}}});
	doc.Links.push_back({"density", "value", "volume", "density"});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray array;
	array.Images.push_back(sentinel);
	REQUIRE(
		EvaluateArray(doc, plan, "image", EvaluationRequest{}, array, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(diagnostic.Port == "dimension");
	REQUIRE(array.Images == std::vector<Image>{sentinel});
	doc = VolumeGraph();
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", EvaluationRequest{}, image, diagnostic, 1) == Status::LimitExceeded);
	REQUIRE(image == sentinel);
}
TEST_CASE(
	"Source volume fixed point clamp precedes blending and shader undefined powers refuse",
	"[source_volume_projection]"
) {
	auto doc = VolumeGraph();
	doc.Nodes.back().Values[3].Data = .5;
	doc.Nodes.back().Values[2].Data = EnumValue{3};
	const auto image = EvaluateVolume(doc);
	REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
	REQUIRE(image.Pixels == std::vector<uint8_t>{161, 161, 161, 255});
	doc.Nodes.front().Values.back().Data = Colour{255, 255, 255, 0};
	doc.Nodes.back().Values[4].Data = 0.;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	const Image sentinel{1, 1, {4, 3, 2, 1}};
	Image output = sentinel;
	REQUIRE(Evaluate(doc, plan, "image", output, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "exponent");
	REQUIRE(output == sentinel);
	doc = VolumeGraph();
	Gradient cmyk;
	cmyk.Mode = 6;
	cmyk.Keys = {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}};
	doc.Nodes.back().Values.push_back({"density_color", cmyk});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", output, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "density_color");
	REQUIRE(output == sentinel);
}
