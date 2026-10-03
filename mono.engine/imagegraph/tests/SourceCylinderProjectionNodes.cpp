#include "NodeHarness.hpp"
#include "nodes/Sampler.hpp"
#include "nodes/SourceProjectionMath.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_cylinder_projection")
using namespace engine::imagegraph;
namespace {
	Document CylinderGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"profile",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 128, 64, 255}}}},
			{"cylinder",
			 "pc.surface_project_cylinder_3_d",
			 "",
			 {},
			 {{"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"view_angle", Vector3{}},
			  {"scale", 2.},
			  {"attribute_color_depth", EnumValue{5}},
			  {"interpolate", EnumValue{1}}}}
		};
		doc.Links = {{"profile", "image", "cylinder", "cylinder"}};
		doc.Outputs = {
			{"colour", "cylinder", "surface_out"},
			{"depth", "cylinder", "depth_pass"},
			{"normal", "cylinder", "normal_pass"}
		};
		return doc;
	}
	void CylinderSet(Document &doc, std::string_view id, Value value) {
		for (auto &node : doc.Nodes)
			if (node.Id == "cylinder") {
				for (auto &item : node.Values)
					if (item.Port == id) {
						item.Data = std::move(value);
						return;
					}
				node.Values.push_back({std::string(id), std::move(value)});
				return;
			}
	}
	Image CylinderEvaluate(
		const Document &doc, std::string_view output = "colour", const EvaluationRequest &request = {}
	) {
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image image;
		status = Evaluate(doc, plan, std::string(output), request, image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	ImageArray CylinderArray(const Document &doc, std::string_view output = "colour") {
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		ImageArray result;
		status = EvaluateArray(doc, plan, std::string(output), EvaluationRequest{}, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
} // namespace
TEST_CASE(
	"Cylinder source voxel entry produces profile depth and step-mask "
	"passes through save",
	"[source_cylinder_projection]"
) {
	const auto doc = CylinderGraph();
	const auto colour = CylinderEvaluate(doc), depth = CylinderEvaluate(doc, "depth"),
			   normal = CylinderEvaluate(doc, "normal");
	const auto p = detail::ReadPixel(colour, 1, 1);
	REQUIRE(p[0] == 1);
	REQUIRE(p[1] == Catch::Approx(128 / 255.).margin(1e-7));
	REQUIRE(p[2] == Catch::Approx(64 / 255.).margin(1e-7));
	REQUIRE(p[3] == 1);
	// Output voxels are .5 wide. This ray first hits the z=1 entrance plane;
	// the source replaces both zero direction components with positive .001.
	const double axial = (std::sqrt(3.) - 1) * std::sqrt(1 + 2e-6) / 2;
	const auto z = detail::ReadPixel(depth, 1, 1);
	for (size_t c = 0; c < 3; ++c)
		REQUIRE(z[c] == Catch::Approx(axial).margin(2e-6));
	REQUIRE(z[3] == 1);
	REQUIRE(detail::ReadPixel(normal, 1, 1) == detail::Rgba{0, 0, 1, 1});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	for (const auto id : {"colour", "depth", "normal"})
		REQUIRE(CylinderEvaluate(restored, id).Pixels == CylinderEvaluate(doc, id).Pixels);
}
TEST_CASE(
	"Cylinder Top alpha gates occupancy and its sampled colour "
	"multiplies the profile",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Nodes.push_back(
		{"top",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{128, 255, 255, 128}}}}
	);
	doc.Links.push_back({"top", "image", "cylinder", "top"});
	auto p = detail::ReadPixel(CylinderEvaluate(doc), 1, 1);
	const double a = 128 / 255.;
	REQUIRE(p[0] == Catch::Approx(a * a).margin(2e-7));
	REQUIRE(p[1] == Catch::Approx(a * a).margin(2e-7));
	REQUIRE(p[2] == Catch::Approx(64 / 255. * a).margin(2e-7));
	REQUIRE(p[3] == Catch::Approx(a * a).margin(2e-7));
	doc.Nodes.back().Values.back().Data = Colour{255, 255, 255, 0};
	for (const auto id : {"colour", "depth", "normal"})
		REQUIRE(detail::ReadPixel(CylinderEvaluate(doc, id), 1, 1) == detail::Rgba{});
	doc.Nodes.back().Values.back().Data = Colour{0, 255, 0, 255};
	for (const auto port : {"right", "bottom", "back", "left"})
		doc.Links.push_back({"top", "image", "cylinder", port});
	doc.Links.erase(doc.Links.begin() + 1);
	REQUIRE(CylinderEvaluate(doc).Pixels == CylinderEvaluate(CylinderGraph()).Pixels);
	CylinderSet(doc, "voxel_color", EnumValue{2});
	REQUIRE(CylinderEvaluate(doc).Pixels == CylinderEvaluate(CylinderGraph()).Pixels);
}
TEST_CASE(
	"Cylinder angular clipping preserves rear entry and source "
	"depth-range remapping",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "angle_range", Vector2{0, 30});
	auto depth = detail::ReadPixel(CylinderEvaluate(doc, "depth"), 1, 1);
	// Front voxel centres have angles above 30 degrees. First accepted rear
	// voxel enters at z=-.5, so the ray travels sqrt(3)+.5 world units.
	const double rear = (std::sqrt(3.) + .5) * std::sqrt(1 + 2e-6) / 2;
	REQUIRE(depth[0] == Catch::Approx(rear).margin(2e-6));
	CylinderSet(doc, "depth_range", Vector2{.25, 2.25});
	REQUIRE(
		detail::ReadPixel(CylinderEvaluate(doc, "depth"), 1, 1)[0] ==
		Catch::Approx((rear - .25) / 2).margin(2e-6)
	);
	CylinderSet(doc, "angle_range", Vector2{180, 360});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc), 1, 1) == detail::Rgba{});
	CylinderSet(doc, "angle_range", Vector2{360, 0});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc), 1, 1) == detail::Rgba{});
}
TEST_CASE("Cylinder radial profile coordinates retain From Center offset", "[source_cylinder_projection]") {
	Image profile{4, 1, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
	auto run = imagegraph_test::RunNode(
		"pc.surface_project_cylinder_3_d",
		{{"cylinder", &profile}},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"view_angle", Vector3{}},
		 {"scale", 2.},
		 {"interpolate", EnumValue{1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	// First profile radius is sqrt(.125^2+.375^2)*2, in the last texel.
	REQUIRE(detail::ReadPixel(run.Output(), 1, 1) == detail::Rgba{1, 1, 1, 1});
	// A six-texel profile separates .790569 from .895285 without reconstruction.
	profile = {6, 1, {255, 0, 0, 255, 255, 0,	0, 255, 255, 0, 0,	 255,
					  255, 0, 0, 255, 0,   255, 0, 255, 0,	 0, 255, 255}};
	for (bool fromCenter : {false, true}) {
		run = imagegraph_test::RunNode(
			"pc.surface_project_cylinder_3_d",
			{{"cylinder", &profile}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"from_center", fromCenter},
			 {"interpolate", EnumValue{1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(
			detail::ReadPixel(run.Output(), 1, 1) ==
			(fromCenter ? detail::Rgba{0, 0, 1, 1} : detail::Rgba{0, 1, 0, 1})
		);
	}
}
TEST_CASE(
	"Cylinder camera rotation changes the DDA axis and translation can miss", "[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "view_angle", Vector3{0, 90, 0});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc, "normal"), 1, 1) == detail::Rgba{1, 0, 0, 1});
	CylinderSet(doc, "position", Vector3{20, 0, 0});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc), 1, 1) == detail::Rgba{});
	CylinderSet(doc, "view_angle", Vector3{});
	CylinderSet(doc, "position", Vector3{});
	CylinderSet(doc, "projection", EnumValue{0});
	CylinderSet(doc, "distance", 1.);
	auto a = CylinderEvaluate(doc, "depth");
	CylinderSet(doc, "distance", 2.);
	auto b = CylinderEvaluate(doc, "depth");
	REQUIRE(detail::ReadPixel(b, 1, 1)[0] > detail::ReadPixel(a, 1, 1)[0]);
	CylinderSet(doc, "fov", 30.);
	REQUIRE(CylinderEvaluate(doc, "depth").Pixels != b.Pixels);
	CylinderSet(doc, "projection", EnumValue{1});
	CylinderSet(doc, "dimension", Vector2{8, 4});
	auto wide = CylinderEvaluate(doc);
	REQUIRE(wide.Width == 8);
	REQUIRE(wide.Height == 4);
	CylinderSet(doc, "scale", 4.);
	REQUIRE(CylinderEvaluate(doc).Pixels != wide.Pixels);
}
TEST_CASE(
	"Cylinder all seven surface formats preserve alpha and finite MRT output", "[source_cylinder_projection]"
) {
	constexpr SurfaceFormat formats[] = {
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	auto doc = CylinderGraph();
	for (int64_t choice = 2; choice <= 8; ++choice) {
		CylinderSet(doc, "attribute_color_depth", EnumValue{choice});
		for (const auto port : {"colour", "depth", "normal"}) {
			const auto image = CylinderEvaluate(doc, port);
			REQUIRE(image.Format == formats[choice - 2]);
			REQUIRE(FiniteSurfaceSamples(image));
			REQUIRE(detail::ReadPixel(image, 1, 1)[3] == 1);
		}
	}
}
TEST_CASE(
	"Cylinder selected coordinate and depth rows survive array "
	"evaluation and persistence",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Junctions.push_back(
		{"ranges", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{0, 1}, Vector2{0, 2}}}}
	);
	doc.Links.push_back({"ranges", "value", "cylinder", "depth_range"});
	auto result = CylinderArray(doc, "depth");
	REQUIRE(result.Images.size() == 2);
	const auto a = detail::ReadPixel(result.Images[0], 1, 1)[0],
			   b = detail::ReadPixel(result.Images[1], 1, 1)[0];
	REQUIRE(b == Catch::Approx(a / 2).margin(1e-7));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	auto replay = CylinderArray(restored, "depth");
	for (size_t i = 0; i < 2; ++i)
		REQUIRE(replay.Images[i].Pixels == result.Images[i].Pixels);
	doc.Junctions.clear();
	doc.Links.pop_back();
	doc.Junctions.push_back(
		{"angles", "", ValueType::Array, ArrayValue{ValueType::Scalar, {}, {{0., 0., 0.}, {0., 90., 0.}}}}
	);
	doc.Links.push_back({"angles", "value", "cylinder", "view_angle"});
	result = CylinderArray(doc, "normal");
	REQUIRE(result.Images.size() == 2);
	REQUIRE(detail::ReadPixel(result.Images[0], 1, 1) == detail::Rgba{0, 0, 1, 1});
	REQUIRE(detail::ReadPixel(result.Images[1], 1, 1) == detail::Rgba{1, 0, 0, 1});
}
TEST_CASE(
	"Cylinder a defined miss permits unused zero depth denominator but "
	"hits refuse atomically",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "depth_range", Vector2{1, 1});
	CylinderSet(doc, "position", Vector3{20, 0, 0});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc), 1, 1) == detail::Rgba{});
	CylinderSet(doc, "position", Vector3{});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "depth_range");
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Cylinder whole original array admits work and three attachment "
	"bytes before publication",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Junctions.push_back(
		{"dimensions",
		 "",
		 ValueType::Array,
		 ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{512, 512}}}}
	);
	doc.Links.push_back({"dimensions", "value", "cylinder", "dimension"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back(Image{1, 1, {9, 8, 7, 6}});
	REQUIRE(
		EvaluateArray(doc, plan, "colour", EvaluationRequest{}, sentinel, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(sentinel.Images.size() == 1);
	REQUIRE(sentinel.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	doc.Junctions[0].Default = ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{4, 4}}};
	doc.Junctions.push_back({"distances", "", ValueType::Array, ArrayValue{ValueType::Scalar, {1., 1e7}}});
	doc.Links.push_back({"distances", "value", "cylinder", "distance"});
	CylinderSet(doc, "projection", EnumValue{0});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(
		EvaluateArray(doc, plan, "colour", EvaluationRequest{}, sentinel, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(sentinel.Images.size() == 1);
}

TEST_CASE(
	"Cylinder timeline camera scale and frame replay use the captured "
	"authored controls",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Keyframes = {{"cylinder", "scale", 0, 2., "linear"}, {"cylinder", "scale", 2, 4.}};
	EvaluationRequest request;
	request.Tick = 1;
	auto middle = CylinderEvaluate(doc, "colour", request);
	auto expected = doc;
	expected.Keyframes.clear();
	CylinderSet(expected, "scale", 3.);
	REQUIRE(middle.Pixels == CylinderEvaluate(expected).Pixels);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(CylinderEvaluate(restored, "colour", request).Pixels == middle.Pixels);
}
TEST_CASE(
	"Cylinder ray singularity and three-attachment request byte caps "
	"retain caller pixels",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "projection", EnumValue{0});
	CylinderSet(doc, "fov", 0.);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	doc = CylinderGraph();
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;

	REQUIRE(Evaluate(doc, plan, "colour", request, sentinel, diagnostic, 1024) == Status::LimitExceeded);
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Cylinder private row-zero admission observes later dimensions "
	"before any output allocation",
	"[source_cylinder_projection]"
) {
	const auto *entry = FindCatalogueEntry("pc.surface_project_cylinder_3_d");
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(entry->Type);
	REQUIRE(executor);
	Node authored{"cylinder", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 2;
	Image profile{1, 1, {255, 255, 255, 255}};
	context.Images = {{"cylinder", &profile}};
	context.Values = {
		{"dimension", Vector2{4, 4}},
		{"dimension_unit", EnumValue{0}},
		{"projection", EnumValue{1}},
		{"distance", 2.}
	};
	Value original = ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{512, 512}}};
	std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"dimension", &original}}};
	context.ProcessorOriginalValues = originals;
	CHECK_FALSE(executor(context));
	INFO(context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputCharge.Bytes() == 0);
}
TEST_CASE(
	"Cylinder flat numeric getter tuples supply dimensions ranges and "
	"resized camera vectors",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Junctions = {
		{"size", "", ValueType::Array, ArrayValue{ValueType::Scalar, {4., 4.}}},
		{"range", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 2.}}},
		{"angle", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 90.}}}
	};
	doc.Links.push_back({"size", "value", "cylinder", "dimension"});
	doc.Links.push_back({"range", "value", "cylinder", "depth_range"});
	doc.Links.push_back({"angle", "value", "cylinder", "view_angle"});
	auto expected = CylinderGraph();
	CylinderSet(expected, "depth_range", Vector2{0, 2});
	CylinderSet(expected, "view_angle", Vector3{0, 90, 0});
	for (const auto port : {"colour", "depth", "normal"})
		REQUIRE(CylinderEvaluate(doc, port).Pixels == CylinderEvaluate(expected, port).Pixels);
	doc.Links.pop_back();
	doc.Junctions.back().Type = ValueType::Scalar;
	doc.Junctions.back().Default = 0.;
	doc.Links.push_back({"angle", "value", "cylinder", "view_angle"});
	CylinderSet(expected, "view_angle", Vector3{});
	REQUIRE(CylinderEvaluate(doc).Pixels == CylinderEvaluate(expected).Pixels);
}
TEST_CASE(
	"Cylinder plain texture reads distinguish Pixel from bilinear and "
	"retain source interpolation aliases",
	"[source_cylinder_projection]"
) {
	Image profile{6, 1, {255, 0, 0, 255, 255, 0,   0, 255, 255, 0, 0,	255,
						 255, 0, 0, 255, 0,	  255, 0, 255, 0,	0, 255, 255}};
	// radius = 2*sqrt(.125^2+.375^2); bilinear sample index is radius*6-.5.
	const double fraction = 2 * std::sqrt(.125 * .125 + .375 * .375) * 6 - .5 - 4;
	for (int64_t interpolation : {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{4}}) {
		auto run = imagegraph_test::RunNode(
			"pc.surface_project_cylinder_3_d",
			{{"cylinder", &profile}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"interpolate", EnumValue{interpolation}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto p = detail::ReadPixel(run.Output(), 1, 1);
		REQUIRE(p[0] == 0);
		REQUIRE(p[1] == Catch::Approx(interpolation == 1 ? 1 : 1 - fraction).margin(2e-6));
		REQUIRE(p[2] == Catch::Approx(interpolation == 1 ? 0 : fraction).margin(2e-6));
		REQUIRE(p[3] == 1);
	}
}
TEST_CASE(
	"Cylinder first-voxel hit retains the source zero step mask and "
	"signed entry distance",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "position", Vector3{0, 0, std::sqrt(3.)});
	REQUIRE(detail::ReadPixel(CylinderEvaluate(doc, "normal"), 1, 1) == detail::Rgba{0, 0, 0, 1});
	// The source entry plane lies behind this already-inside eye. It takes a
	// Euclidean distance to that point rather than clamping the ray parameter.
	// The half-unit entry distance is divided by the authored Scale of two.
	REQUIRE(
		detail::ReadPixel(CylinderEvaluate(doc, "depth"), 1, 1)[0] ==
		Catch::Approx(std::sqrt(1 + 2e-6) / 4).margin(2e-6)
	);
}
TEST_CASE(
	"Cylinder native fixed point fragment clamp precedes blending while "
	"floating output retains HDR",
	"[source_cylinder_projection]"
) {
	Image profile;
	profile.Width = 2;
	profile.Height = 2;
	profile.Format = SurfaceFormat::RGBA32Float;
	profile.Pixels.resize(64);
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 2; ++x)
			REQUIRE(detail::WritePixel(profile, x, y, {2, -1, .5, .5}));
	for (int64_t depth : {int64_t{3}, int64_t{5}}) {
		auto run = imagegraph_test::RunNode(
			"pc.surface_project_cylinder_3_d",
			{{"cylinder", &profile}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"interpolate", EnumValue{1}},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		auto p = detail::ReadPixel(run.Output(), 1, 1);
		if (depth == 5)
			REQUIRE(p == detail::Rgba{1, -.5, .25, .25});
		else {
			REQUIRE(p[0] == Catch::Approx(128 / 255.));
			REQUIRE(p[1] == 0);
			REQUIRE(p[2] == Catch::Approx(64 / 255.));
			REQUIRE(p[3] == Catch::Approx(64 / 255.));
		}
	}
}
TEST_CASE(
	"Cylinder all seven owned input formats sample the actual profile surface", "[source_cylinder_projection]"
) {
	constexpr SurfaceFormat formats[] = {
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (const auto format : formats) {
		Image profile;
		profile.Width = 2;
		profile.Height = 2;
		profile.Format = format;
		const auto layout = CheckedSurfaceLayout(2, 2, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		profile.Pixels.resize(layout->Bytes);
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 2; ++x)
				REQUIRE(detail::WritePixel(profile, x, y, {1, 1, 1, 1}));
		auto run = imagegraph_test::RunNode(
			"pc.surface_project_cylinder_3_d",
			{{"cylinder", &profile}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"interpolate", EnumValue{1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		INFO(DescribeSurfaceFormat(format)->Name);
		const bool redOnly = DescribeSurfaceFormat(format)->Channels == 1;
		REQUIRE(
			detail::ReadPixel(run.Output(), 1, 1) ==
			(redOnly ? detail::Rgba{1, 0, 0, 1} : detail::Rgba{1, 1, 1, 1})
		);
	}
}
TEST_CASE(
	"Cylinder Input depth follows numeric Dimension slot zero rather "
	"than the HDR profile",
	"[source_cylinder_projection]"
) {
	Image profile;
	profile.Width = 2;
	profile.Height = 2;
	profile.Format = SurfaceFormat::RGBA32Float;
	profile.Pixels.resize(64);
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 2; ++x)
			REQUIRE(detail::WritePixel(profile, x, y, {2, -1, .5, .5}));
	auto run = imagegraph_test::RunNode(
		"pc.surface_project_cylinder_3_d",
		{{"cylinder", &profile}},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"view_angle", Vector3{}},
		 {"scale", 2.},
		 {"attribute_color_depth", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	REQUIRE(run.Output().Format == SurfaceFormat::RGBA8Unorm);
	REQUIRE(detail::ReadPixel(run.Output(), 1, 1)[0] == Catch::Approx(128 / 255.));
	Image malformed = profile;
	malformed.Pixels.pop_back();
	run = imagegraph_test::RunNode(
		"pc.surface_project_cylinder_3_d",
		{{"cylinder", &malformed}},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE_FALSE(run.Ok);
	REQUIRE(run.Code == Status::InvalidValue);
	REQUIRE(run.Images.empty());
}
TEST_CASE(
	"Cylinder source Surface getters project four uniform ports before "
	"row processing",
	"[source_cylinder_projection]"
) {
	for (const auto port : {"view_angle", "position", "angle_range", "depth_range"}) {
		auto doc = CylinderGraph();
		doc.Nodes.push_back(
			{"getter",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
		);
		doc.Links.push_back({"getter", "image", "cylinder", port});
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationSnapshot snapshot;
		status = EvaluateNodeInputs(doc, plan, "cylinder", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *resolved = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) resolved = &input.Data;
		REQUIRE(resolved);
		const bool vector = std::string_view(port) == "view_angle" || std::string_view(port) == "position";
		REQUIRE(*resolved == (vector ? Value{Vector3{1, 2, 0}} : Value{Vector2{1, 2}}));
		auto expected = CylinderGraph();
		CylinderSet(expected, port, vector ? Value{Vector3{1, 2, 0}} : Value{Vector2{1, 2}});
		for (const auto output : {"colour", "depth", "normal"})
			REQUIRE(CylinderEvaluate(doc, output).Pixels == CylinderEvaluate(expected, output).Pixels);
	}
}
TEST_CASE(
	"Cylinder source whole Surface-array uniform getter uses nonsurface "
	"one-by-one dimensions",
	"[source_cylinder_projection]"
) {
	for (const auto port : {"view_angle", "position", "angle_range", "depth_range"}) {
		auto doc = CylinderGraph();
		doc.Nodes.push_back(
			{"getter", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
		);
		doc.Junctions.push_back(
			{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 3}, Vector2{4, 3}}}}
		);
		doc.Links.push_back({"sizes", "value", "getter", "dimension"});
		doc.Links.push_back({"getter", "surface_out", "cylinder", port});
		doc.Outputs.push_back({"surfaces", "getter", "surface_out"});
		auto images = CylinderArray(doc, "surfaces");
		REQUIRE(images.Images.size() == 2);
		REQUIRE(images.Images[0].Width == 2);
		REQUIRE(images.Images[1].Width == 4);
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		EvaluationSnapshot snapshot;
		auto status = EvaluateNodeInputs(doc, plan, "cylinder", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *resolved = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) resolved = &input.Data;
		REQUIRE(resolved);
		const bool vector = std::string_view(port) == "view_angle" || std::string_view(port) == "position";
		REQUIRE(*resolved == (vector ? Value{Vector3{1, 1, 0}} : Value{Vector2{1, 1}}));
		// Equal range endpoints still define a miss with this angular interval.
		if (std::string_view(port) == "depth_range") CylinderSet(doc, "angle_range", Vector2{0, 0});
		auto expected = CylinderGraph();
		CylinderSet(expected, port, vector ? Value{Vector3{1, 1, 0}} : Value{Vector2{1, 1}});
		if (std::string_view(port) == "depth_range") CylinderSet(expected, "angle_range", Vector2{0, 0});
		for (const auto output : {"colour", "depth", "normal"})
			REQUIRE(CylinderEvaluate(doc, output).Pixels == CylinderEvaluate(expected, output).Pixels);
	}
}
TEST_CASE(
	"Cylinder generic tuples resize before uniforms and ignored extra "
	"components do not consume work",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	doc.Nodes.push_back(
		{"json", "pc.struct_json_parse", "", {}, {{"json_string", std::string{R"([4,4,"unused"])"}}}}
	);
	doc.Nodes.push_back({"array", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}});
	doc.Nodes.back().DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
	doc.Links.push_back({"json", "struct", "array", "input_0"});
	doc.Links.push_back({"array", "array", "cylinder", "dimension"});
	REQUIRE(CylinderEvaluate(doc).Pixels == CylinderEvaluate(CylinderGraph()).Pixels);
	doc.Nodes[2].Values[0].Data = std::string{R"([0,2,"unused"])"};
	doc.Links.back().ToPort = "depth_range";
	auto expected = CylinderGraph();
	CylinderSet(expected, "depth_range", Vector2{0, 2});
	REQUIRE(CylinderEvaluate(doc, "depth").Pixels == CylinderEvaluate(expected, "depth").Pixels);
	doc.Nodes[2].Values[0].Data = std::string{R"([0,90,0,"unused"])"};
	doc.Links.back().ToPort = "view_angle";
	CylinderSet(expected, "depth_range", Vector2{0, 1});
	CylinderSet(expected, "view_angle", Vector3{0, 90, 0});
	REQUIRE(CylinderEvaluate(doc, "normal").Pixels == CylinderEvaluate(expected, "normal").Pixels);
	doc.Nodes[2].Values[0].Data = std::string{R"([0,"invalid",0])"};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::TypeMismatch);
	REQUIRE(diagnostic.Port == "view_angle");
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Cylinder generic Atlas sockets remain distinct from declared "
	"Surface getters",
	"[source_cylinder_projection]"
) {
	for (const auto kind : {AtlasKind::Atlas, AtlasKind::SurfaceAtlas})
		for (const auto port :
			 {"view_angle", "position", "angle_range", "depth_range", "fov", "distance", "scale"}) {
			auto doc = CylinderGraph();
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
			const auto sentinel = plan;
			AtlasValue atlas;
			atlas.Data.emplace();
			atlas.Data->Kind = kind;
			atlas.Data->Surface.Data = {1, 2, {255, 255, 255, 255, 255, 255, 255, 255}};
			atlas.Data->Dimension = {1, 2};
			doc.Junctions.push_back({"atlas", "", ValueType::Atlas, atlas});
			doc.Links.push_back({"atlas", "value", "cylinder", port});
			const auto status = Compile(doc, plan, diagnostic);
			INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
			REQUIRE(status == Status::TypeMismatch);
			REQUIRE(diagnostic.NodeId == "cylinder");
			REQUIRE(diagnostic.Port == port);
			REQUIRE(plan == sentinel);
		}
}
TEST_CASE(
	"Cylinder scalar Surface getters produce width and height processor "
	"rows before camera uniforms",
	"[source_cylinder_projection]"
) {
	for (const auto port : {"fov", "distance", "scale"}) {
		auto doc = CylinderGraph();
		CylinderSet(doc, "projection", EnumValue{0});
		CylinderSet(doc, "distance", 1.);
		doc.Nodes.push_back(
			{"getter",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
		);
		doc.Links.push_back({"getter", "image", "cylinder", port});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, "cylinder", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *resolved = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) resolved = &input.Data;
		REQUIRE(resolved);
		const auto *pair = std::get_if<ArrayValue>(resolved);
		REQUIRE(pair);
		REQUIRE(pair->Elements == std::vector<ElementValue>{1., 2.});
		for (const auto output : {"colour", "depth", "normal"}) {
			auto result = CylinderArray(doc, output);
			REQUIRE(result.Images.size() == 2);
			for (size_t i = 0; i < 2; ++i) {
				auto expected = CylinderGraph();
				CylinderSet(expected, "projection", EnumValue{0});
				CylinderSet(expected, "distance", 1.);
				CylinderSet(expected, port, double(i + 1));
				REQUIRE(result.Images[i].Pixels == CylinderEvaluate(expected, output).Pixels);
			}
		}
	}
}
TEST_CASE(
	"Cylinder scalar whole Surface-array getter keeps the source "
	"nonsurface pair and two rows",
	"[source_cylinder_projection]"
) {
	for (const auto port : {"fov", "distance", "scale"}) {
		auto doc = CylinderGraph();
		CylinderSet(doc, "projection", EnumValue{0});
		CylinderSet(doc, "distance", 1.);
		doc.Nodes.push_back(
			{"getter", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
		);
		doc.Junctions.push_back(
			{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 3}, Vector2{4, 3}}}}
		);
		doc.Links.push_back({"sizes", "value", "getter", "dimension"});
		doc.Links.push_back({"getter", "surface_out", "cylinder", port});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, "cylinder", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *resolved = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) resolved = &input.Data;
		REQUIRE(resolved);
		const auto *pair = std::get_if<ArrayValue>(resolved);
		REQUIRE(pair);
		REQUIRE(pair->Elements == std::vector<ElementValue>{1., 1.});
		auto result = CylinderArray(doc, "depth");
		REQUIRE(result.Images.size() == 2);
		auto expected = CylinderGraph();
		CylinderSet(expected, "projection", EnumValue{0});
		CylinderSet(expected, "distance", 1.);
		CylinderSet(expected, port, 1.);
		for (const auto &image : result.Images)
			REQUIRE(image.Pixels == CylinderEvaluate(expected, "depth").Pixels);
	}
}
TEST_CASE(
	"Cylinder Surface-derived later Distance row is admitted before the "
	"first small row allocates",
	"[source_cylinder_projection]"
) {
	auto doc = CylinderGraph();
	CylinderSet(doc, "dimension", Vector2{8, 8});
	CylinderSet(doc, "projection", EnumValue{0});
	doc.Nodes.push_back(
		{"getter",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{4096}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links.push_back({"getter", "image", "cylinder", "distance"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back(Image{1, 1, {9, 8, 7, 6}});
	const auto status = EvaluateArray(doc, plan, "depth", {}, sentinel, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	REQUIRE(diagnostic.NodeId == "cylinder");
	REQUIRE(sentinel.Images.size() == 1);
	REQUIRE(sentinel.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
