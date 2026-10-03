#include "NodeHarness.hpp"
#include "nodes/Sampler.hpp"
#include "nodes/SourceProjectionMath.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_heightmap_projection")
using namespace engine::imagegraph;
namespace {
	Document HeightmapGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"profile",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"heightmap",
			 "pc.heightmap_project_3_d",
			 "",
			 {},
			 {{"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"view_angle", Vector3{}},
			  {"scale", 2.},
			  {"attribute_color_depth", EnumValue{5}},
			  {"interpolate", EnumValue{1}}}}
		};
		doc.Links = {{"profile", "image", "heightmap", "heightmap"}};
		doc.Outputs = {
			{"colour", "heightmap", "surface_out"},
			{"depth", "heightmap", "depth_pass"},
			{"normal", "heightmap", "normal_pass"}
		};
		return doc;
	}
	void HeightmapSet(Document &doc, std::string_view id, Value value) {
		for (auto &node : doc.Nodes)
			if (node.Id == "heightmap") {
				for (auto &item : node.Values)
					if (item.Port == id) {
						item.Data = std::move(value);
						return;
					}
				node.Values.push_back({std::string(id), std::move(value)});
				return;
			}
	}
	Image HeightmapEvaluate(
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
	ImageArray HeightmapArray(const Document &doc, std::string_view output = "colour") {
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
	"Heightmap source voxel red occupancy publishes three passes and saves", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{0, 0, 1, 1});
	const double axial = (std::sqrt(3.) - 1) * std::sqrt(1 + 2e-6) / 2;
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "depth"), 1, 1)[0] == Catch::Approx(axial).margin(2e-6));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	for (auto output : {"colour", "depth", "normal"})
		REQUIRE(HeightmapEvaluate(restored, output).Pixels == HeightmapEvaluate(doc, output).Pixels);
}
TEST_CASE(
	"Heightmap occupancy ignores alpha and zero red has no occupied voxel", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Nodes[0].Values[2].Data = Colour{255, 255, 255, 0};
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{0, 0, 1, 1});
	doc.Nodes[0].Values[2].Data = Colour{0, 255, 255, 255};
	for (auto output : {"colour", "depth", "normal"})
		REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, output), 1, 1) == detail::Rgba{});
}
TEST_CASE("Heightmap side and front samplers obey source face precedence", "[source_heightmap_projection]") {
	auto doc = HeightmapGraph();
	doc.Nodes.push_back(
		{"side",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	doc.Nodes.push_back(
		{"front",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 0, 255, 255}}}}
	);
	doc.Links.push_back({"side", "image", "heightmap", "texture_side"});
	doc.Links.push_back({"front", "image", "heightmap", "texture_front"});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{1, 0, 0, 1});
	HeightmapSet(doc, "view_angle", Vector3{0, 90, 0});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{0, 0, 1, 1});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{1, 0, 0, 1});
}
TEST_CASE(
	"Heightmap texture override and gradient alpha multiply before native blending",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Nodes.push_back(
		{"texture",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{128, 64, 32, 128}}}}
	);
	doc.Links.push_back({"texture", "image", "heightmap", "texture"});
	HeightmapSet(doc, "height_color", Gradient{0, {{0, {255, 255, 255, 128}}}});
	const auto p = detail::ReadPixel(HeightmapEvaluate(doc), 1, 1);
	const double a = 128 / 255. * 128 / 255.;
	REQUIRE(p[0] == Catch::Approx(128 / 255. * a).margin(1e-6));
	REQUIRE(p[1] == Catch::Approx(64 / 255. * a).margin(1e-6));
	REQUIRE(p[3] == Catch::Approx(a * a).margin(1e-6));
}
TEST_CASE(
	"Heightmap normalize height shift and gradient use entry coordinates", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Nodes[0].Values[2].Data = Colour{128, 128, 128, 255};
	HeightmapSet(doc, "height_color", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}});
	const double raw = 1 - (.625 + .0005 * (std::sqrt(3.) - 1));
	REQUIRE(
		detail::ReadPixel(HeightmapEvaluate(doc), 1, 2)[0] == Catch::Approx(128 / 255. * raw).margin(2e-6)
	);
	HeightmapSet(doc, "normalize_height", true);
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 2)[0] == Catch::Approx(raw).margin(2e-6));
	HeightmapSet(doc, "normalize_height", false);
	HeightmapSet(doc, "shift", .5);
	REQUIRE(
		detail::ReadPixel(HeightmapEvaluate(doc), 1, 2)[0] ==
		Catch::Approx(128 / 255. * (raw + .5)).margin(2e-6)
	);
	HeightmapSet(doc, "shift", -1.);
	REQUIRE(
		detail::ReadPixel(HeightmapEvaluate(doc), 1, 2)[0] == Catch::Approx(128 / 255. * raw).margin(2e-6)
	);
}
TEST_CASE(
	"Heightmap tiled occupancy wraps x and z without moving final texture coordinates",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	HeightmapSet(doc, "position", Vector3{20, 0, 0});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{});
	HeightmapSet(doc, "tiled", true);
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{0, 0, 0, 1});
	HeightmapSet(doc, "oversample", EnumValue{1});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{});
	HeightmapSet(doc, "oversample", EnumValue{4});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{1, 1, 1, 1});
}
TEST_CASE(
	"Heightmap selected height and depth tuples preserve source array rows", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Junctions.push_back(
		{"ranges", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{0, 1}, Vector2{0, 2}}}}
	);
	doc.Links.push_back({"ranges", "value", "heightmap", "depth_range"});
	auto rows = HeightmapArray(doc, "depth");
	REQUIRE(rows.Images.size() == 2);
	REQUIRE(
		detail::ReadPixel(rows.Images[1], 1, 1)[0] ==
		Catch::Approx(detail::ReadPixel(rows.Images[0], 1, 1)[0] / 2)
	);
	doc.Links.back().ToPort = "height_range";
	doc.Junctions[0].Default = ArrayValue{ValueType::Vector2, {Vector2{0, 1}, Vector2{1, 0}}};
	rows = HeightmapArray(doc, "normal");
	REQUIRE(detail::ReadPixel(rows.Images[0], 1, 1) == detail::Rgba{0, 0, 1, 1});
	REQUIRE(detail::ReadPixel(rows.Images[1], 1, 1) == detail::Rgba{});
}
TEST_CASE(
	"Heightmap first voxel retains zero step mask and negative entry plane distance",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	HeightmapSet(doc, "position", Vector3{0, 0, std::sqrt(3.)});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc, "normal"), 1, 1) == detail::Rgba{0, 0, 0, 1});
	REQUIRE(
		detail::ReadPixel(HeightmapEvaluate(doc, "depth"), 1, 1)[0] ==
		Catch::Approx(std::sqrt(1 + 2e-6) / 4).margin(2e-6)
	);
}
TEST_CASE(
	"Heightmap seven native input and output formats keep channel storage contracts",
	"[source_heightmap_projection]"
) {
	for (int64_t choice = 2; choice <= 8; ++choice) {
		auto doc = HeightmapGraph();
		HeightmapSet(doc, "attribute_color_depth", EnumValue{choice});
		auto p = HeightmapEvaluate(doc);
		REQUIRE(p.Format == *SourceSurfaceFormat(choice));
		REQUIRE(ValidSurfaceLayout(p, Limits::MaximumDimension, Limits::MaximumOutputBytes));
		const bool redOnly = DescribeSurfaceFormat(p.Format)->Channels == 1;
		REQUIRE(
			detail::ReadPixel(p, 1, 1) == (redOnly ? detail::Rgba{1, 0, 0, 1} : detail::Rgba{1, 1, 1, 1})
		);
		const auto normal = HeightmapEvaluate(doc, "normal");
		REQUIRE(normal.Format == p.Format);
		REQUIRE(
			detail::ReadPixel(normal, 1, 1) == (redOnly ? detail::Rgba{0, 0, 0, 1} : detail::Rgba{0, 0, 1, 1})
		);
		const auto depth = HeightmapEvaluate(doc, "depth");
		REQUIRE(depth.Format == p.Format);
		REQUIRE(FiniteSurfaceSamples(depth));
	}
	for (auto format :
		 {SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		Image surface;
		surface.Width = 2;
		surface.Height = 2;
		surface.Format = format;
		surface.Pixels.resize(CheckedSurfaceLayout(2, 2, format, Limits::MaximumOutputBytes)->Bytes);
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 2; ++x)
				REQUIRE(detail::WritePixel(surface, x, y, {1, 1, 1, 1}));
		auto run = imagegraph_test::RunNode(
			"pc.heightmap_project_3_d",
			{{"heightmap", &surface}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"interpolate", EnumValue{1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(
			detail::ReadPixel(run.Output(), 1, 1) == (DescribeSurfaceFormat(format)->Channels == 1
														  ? detail::Rgba{1, 0, 0, 1}
														  : detail::Rgba{1, 1, 1, 1})
		);
	}
}
TEST_CASE(
	"Heightmap original later Distance row refuses before output allocation", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	HeightmapSet(doc, "projection", EnumValue{0});
	doc.Junctions.push_back({"distance", "", ValueType::Array, ArrayValue{ValueType::Scalar, {1., 1e7}}});
	doc.Links.push_back({"distance", "value", "heightmap", "distance"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {9, 8, 7, 6}});
	auto status = EvaluateArray(doc, plan, "colour", {}, sentinel, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	REQUIRE(sentinel.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Heightmap undefined divisions and byte limits preserve caller image", "[source_heightmap_projection]"
) {
	for (auto port : {"height_range", "depth_range"}) {
		auto doc = HeightmapGraph();
		HeightmapSet(doc, port, Vector2{0, 0});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		Image sentinel{1, 1, {9, 8, 7, 6}};
		REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
		REQUIRE(diagnostic.Port == port);
		REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	}
	auto doc = HeightmapGraph();
	EvaluationRequest request;

	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", request, sentinel, diagnostic, 1024) == Status::LimitExceeded);
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Heightmap interpolated texture reads use Heightmap dimensions across differently sized textures",
	"[source_heightmap_projection]"
) {
	Image heightmap{2, 2, std::vector<uint8_t>(16, 255)},
		texture{4, 1, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
	const double u = .375 + .0005 * (std::sqrt(3.) - 1), linear = 4 * u - .5, f = 2 * u + .5 - 1,
				 bicubic = (1 + f * f * (3 - 2 * f) - .5) / 2, bx = 4 * bicubic - .5;
	for (int64_t interpolation : {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{4}, int64_t{6}}) {
		auto run = imagegraph_test::RunNode(
			"pc.heightmap_project_3_d",
			{{"heightmap", &heightmap}, {"texture", &texture}},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"view_angle", Vector3{}},
			 {"scale", 2.},
			 {"interpolate", EnumValue{interpolation}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		auto p = detail::ReadPixel(run.Output(), 1, 1);
		if (interpolation == 1 || interpolation == 6)
			REQUIRE(p == detail::Rgba{0, 1, 0, 1});
		else if (interpolation == 2) {
			REQUIRE(p[1] == Catch::Approx(2 - linear).margin(2e-6));
			REQUIRE(p[2] == Catch::Approx(linear - 1).margin(2e-6));
		} else if (interpolation == 3) {
			REQUIRE(p[0] == Catch::Approx(1 - bx).margin(2e-6));
			REQUIRE(p[1] == Catch::Approx(bx).margin(2e-6));
		} else
			REQUIRE(FiniteSurfaceSamples(run.Output()));
	}
}
TEST_CASE(
	"Heightmap source camera controls and animated Scale survive save", "[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	HeightmapSet(doc, "projection", EnumValue{0});
	HeightmapSet(doc, "distance", 2.);
	HeightmapSet(doc, "fov", 0.);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	doc = HeightmapGraph();
	doc.Keyframes = {{"heightmap", "scale", 0, 2., "linear"}, {"heightmap", "scale", 2, 4.}};
	EvaluationRequest request;
	request.Tick = 1;
	auto output = HeightmapEvaluate(doc, "depth", request);
	auto expected = HeightmapGraph();
	HeightmapSet(expected, "scale", 3.);
	REQUIRE(output.Pixels == HeightmapEvaluate(expected, "depth").Pixels);
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(HeightmapEvaluate(restored, "depth", request).Pixels == output.Pixels);
}
TEST_CASE(
	"Heightmap heterogeneous original Dimension rows refuse before first small image",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Junctions.push_back(
		{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{4, 4}, Vector2{512, 512}}}}
	);
	doc.Links.push_back({"sizes", "value", "heightmap", "dimension"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {9, 8, 7, 6}});
	auto status = EvaluateArray(doc, plan, "colour", {}, sentinel, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	REQUIRE(sentinel.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Heightmap exact Surface getters retain tuple and scalar row shapes", "[source_heightmap_projection]"
) {
	for (auto port :
		 {"view_angle", "position", "height_range", "depth_range", "fov", "distance", "scale", "shift"}) {
		auto doc = HeightmapGraph();
		doc.Nodes.push_back(
			{"getter",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
		);
		doc.Links.push_back({"getter", "image", "heightmap", port});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		EvaluationSnapshot snapshot;
		auto status = EvaluateNodeInputs(doc, plan, "heightmap", {}, snapshot, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *value = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) value = &input.Data;
		REQUIRE(value);
		const bool vector = std::string_view(port) == "view_angle" || std::string_view(port) == "position",
				   range =
					   std::string_view(port) == "height_range" || std::string_view(port) == "depth_range";
		if (vector)
			REQUIRE(*value == Value{Vector3{1, 2, 0}});
		else if (range)
			REQUIRE(*value == Value{Vector2{1, 2}});
		else {
			const auto *pair = std::get_if<ArrayValue>(value);
			REQUIRE(pair);
			REQUIRE(pair->Elements == std::vector<ElementValue>{1., 2.});
		}
		if (!range && !vector) {
			auto actual = HeightmapArray(doc, "normal");
			REQUIRE(actual.Images.size() == 2);
			for (size_t i = 0; i < 2; ++i) {
				auto expected = HeightmapGraph();
				HeightmapSet(expected, port, double(i + 1));
				REQUIRE(actual.Images[i].Pixels == HeightmapEvaluate(expected, "normal").Pixels);
			}
		}
	}
}
TEST_CASE(
	"Heightmap scalar whole Surface array remains a nonsurface width-height pair",
	"[source_heightmap_projection]"
) {
	for (auto port : {"fov", "distance", "scale", "shift"}) {
		auto doc = HeightmapGraph();
		doc.Nodes.push_back(
			{"getter", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
		);
		doc.Junctions.push_back(
			{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 3}, Vector2{4, 3}}}}
		);
		doc.Links.push_back({"sizes", "value", "getter", "dimension"});
		doc.Links.push_back({"getter", "surface_out", "heightmap", port});
		auto rows = HeightmapArray(doc, "normal");
		REQUIRE(rows.Images.size() == 2);
		auto expected = HeightmapGraph();
		HeightmapSet(expected, port, 1.);
		for (const auto &image : rows.Images)
			REQUIRE(image.Pixels == HeightmapEvaluate(expected, "normal").Pixels);
	}
}
TEST_CASE(
	"Heightmap private row-zero admission charges original late rows before allocation",
	"[source_heightmap_projection]"
) {
	ArrayValue dimensions{ValueType::Vector2, {Vector2{4, 4}, Vector2{512, 512}}};
	Image heightmap{2, 2, std::vector<uint8_t>(16, 255)};
	const auto *entry = FindCatalogueEntry("pc.heightmap_project_3_d");
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(entry->Type);
	REQUIRE(executor);
	Node authored{"heightmap", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"heightmap", &heightmap}};
	context.Values = {{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}};
	context.ProcessorCount = 2;
	Value original{dimensions};
	std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"dimension", &original}}};
	context.ProcessorOriginalValues = originals;
	REQUIRE_FALSE(executor(context));
	INFO(context.FailureMessage);
	REQUIRE(context.FailureCode == Status::LimitExceeded);
	REQUIRE(context.OutputImages.empty());
	REQUIRE(context.OutputCharge.Bytes() == 0);
}
TEST_CASE(
	"Heightmap admitted general tuples ignore unused coordinates and refuse malformed selected rows "
	"atomically",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Nodes.push_back(
		{"json", "pc.struct_json_parse", "", {}, {{"json_string", std::string{R"([0,90,0,"unused"])"}}}}
	);
	doc.Nodes.push_back({"array", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}});
	doc.Nodes.back().DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
	doc.Links.push_back({"json", "struct", "array", "input_0"});
	doc.Links.push_back({"array", "array", "heightmap", "view_angle"});
	auto expected = HeightmapGraph();
	HeightmapSet(expected, "view_angle", Vector3{0, 90, 0});
	REQUIRE(HeightmapEvaluate(doc, "normal").Pixels == HeightmapEvaluate(expected, "normal").Pixels);
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
	"Heightmap non-gradient color and malformed image payloads have named native boundaries",
	"[source_heightmap_projection]"
) {
	Image heightmap{1, 1, {255, 255, 255, 255}}, invalid{1, 1, {255}};
	auto bad = imagegraph_test::RunNode(
		"pc.heightmap_project_3_d",
		{{"heightmap", &invalid}},
		{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE_FALSE(bad.Ok);
	auto color = imagegraph_test::RunNode(
		"pc.heightmap_project_3_d",
		{{"heightmap", &heightmap}},
		{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}, {"height_color", Vector2{0, 1}}}
	);
	REQUIRE_FALSE(color.Ok);
	REQUIRE(color.Code == Status::UnsupportedExecution);
	auto surfaceColor = imagegraph_test::RunNode(
		"pc.heightmap_project_3_d",
		{{"heightmap", &heightmap}, {"height_color", &heightmap}},
		{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE_FALSE(surfaceColor.Ok);
	REQUIRE(surfaceColor.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Heightmap declared Colour getter builds one constant gradient before processing",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Junctions.push_back({"colour", "", ValueType::Colour, Colour{255, 0, 0, 255}});
	doc.Links.push_back({"colour", "value", "heightmap", "height_color"});
	REQUIRE(detail::ReadPixel(HeightmapEvaluate(doc), 1, 1) == detail::Rgba{1, 0, 0, 1});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(doc, plan, "heightmap", {}, snapshot, diagnostic) == Status::Ok);
	const Gradient *gradient = nullptr;
	for (const auto &input : snapshot.Values())
		if (input.Port == "height_color") gradient = std::get_if<Gradient>(&input.Data);
	REQUIRE(gradient);
	REQUIRE(gradient->Keys == std::vector<GradientKey>{{0, {255, 0, 0, 255}}});
}
TEST_CASE(
	"Heightmap declared Colour-array getter uses i over count key times and one image",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Nodes.push_back({"colors", "pc.gradient_sample", "", {}, {{"step", int64_t{2}}}});
	doc.Links.push_back({"colors", "colors", "heightmap", "height_color"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	auto status = EvaluateNodeInputs(doc, plan, "heightmap", {}, snapshot, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const Gradient *gradient = nullptr;
	for (const auto &input : snapshot.Values())
		if (input.Port == "height_color") gradient = std::get_if<Gradient>(&input.Data);
	REQUIRE(gradient);
	REQUIRE(gradient->Keys.size() == 2);
	REQUIRE(gradient->Keys[0].Time == 0);
	REQUIRE(gradient->Keys[1].Time == .5);
	REQUIRE(gradient->Keys[1].Color == Colour{128, 128, 128, 255});
	const auto pixel = detail::ReadPixel(HeightmapEvaluate(doc), 1, 1);
	for (size_t c = 0; c < 3; ++c)
		REQUIRE(pixel[c] == Catch::Approx(128 / 255.).margin(1e-7));
	REQUIRE(pixel[3] == 1);
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(HeightmapEvaluate(restored).Pixels == HeightmapEvaluate(doc).Pixels);
	doc.Nodes[2].Values[0].Data = int64_t{65};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "height_color");
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Heightmap Input depth reads numeric Dimension and HDR texture retains native float blending",
	"[source_heightmap_projection]"
) {
	Image heightmap;
	heightmap.Width = 2;
	heightmap.Height = 2;
	heightmap.Format = SurfaceFormat::RGBA32Float;
	heightmap.Pixels.resize(64);
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 2; ++x)
			REQUIRE(detail::WritePixel(heightmap, x, y, {2, -1, .5, .5}));
	for (int64_t depth : {int64_t{0}, int64_t{3}, int64_t{5}}) {
		auto run = imagegraph_test::RunNode(
			"pc.heightmap_project_3_d",
			{{"heightmap", &heightmap}},
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
			REQUIRE(run.Output().Format == SurfaceFormat::RGBA8Unorm);
			REQUIRE(p[0] == Catch::Approx(128 / 255.));
			REQUIRE(p[1] == 0);
			REQUIRE(p[2] == Catch::Approx(64 / 255.));
			REQUIRE(p[3] == Catch::Approx(64 / 255.));
		}
	}
}
TEST_CASE(
	"Heightmap Surface-derived later Distance row participates in whole-batch work admission",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	HeightmapSet(doc, "dimension", Vector2{8, 8});
	HeightmapSet(doc, "projection", EnumValue{0});
	doc.Nodes.push_back(
		{"getter",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{4096}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links.push_back({"getter", "image", "heightmap", "distance"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {9, 8, 7, 6}});
	auto status = EvaluateArray(doc, plan, "depth", {}, sentinel, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	REQUIRE(diagnostic.NodeId == "heightmap");
	REQUIRE(sentinel.Images.size() == 1);
	REQUIRE(sentinel.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Heightmap Dimension units apply only to self and Mask has no configured source input",
	"[source_heightmap_projection]"
) {
	auto doc = HeightmapGraph();
	doc.Project.emplace();
	doc.Project->SurfaceWidth = 4;
	doc.Project->SurfaceHeight = 6;
	HeightmapSet(doc, "dimension", Vector2{1, 1});
	HeightmapSet(doc, "dimension_unit", EnumValue{1});
	auto image = HeightmapEvaluate(doc);
	REQUIRE(image.Width == 4);
	REQUIRE(image.Height == 6);
	HeightmapSet(doc, "dimension_unit", EnumValue{2});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {9, 8, 7, 6}};
	REQUIRE(Evaluate(doc, plan, "colour", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(diagnostic.Port == "dimension_unit");
	REQUIRE(sentinel.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	doc.Junctions.push_back({"size", "", ValueType::Vector2, Vector2{3, 5}});
	doc.Links.push_back({"size", "value", "heightmap", "dimension"});
	image = HeightmapEvaluate(doc);
	REQUIRE(image.Width == 3);
	REQUIRE(image.Height == 5);
}
