#include <engine/imagegraph/SourceSdf.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_sdf")
using namespace engine::imagegraph;
namespace {
	SdfValue Sphere(double radius = 1) {
		SdfValue sdf;
		auto &data = sdf.Data.emplace();
		SourceSdfShape shape;
		shape.Identity = "sphere";
		shape.Shape = 200;
		shape.Radius = radius;
		data.Shapes.push_back(shape);
		data.Operations.push_back({0, 0});
		return sdf;
	}
	double Sample(const SdfValue &sdf, Vector3 point) {
		double distance = 0;
		Diagnostic diagnostic;
		const auto status = SampleSourceSdf(sdf, point, distance, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return distance;
	}
}
TEST_CASE("Source SDF analytic surfaces and sphere tracing", "[imagegraph][source_sdf]") {
	auto sdf = Sphere();
	CHECK(Sample(sdf, {0, 0, 0}) == Catch::Approx(-1));
	CHECK(Sample(sdf, {0, 0, 3}) == Catch::Approx(2));
	SourceSdfMarchResult result;
	Diagnostic diagnostic;
	REQUIRE(MarchSourceSdf(sdf, {0, 0, 5}, {0, 0, -2}, {0, 10}, result, diagnostic) == Status::Ok);
	CHECK(result.Hit);
	CHECK(result.Depth == Catch::Approx(4));
	CHECK(result.Steps == 2);
	CHECK(result.Normal.Z == Catch::Approx(1).margin(1e-5));
	REQUIRE(MarchSourceSdf(sdf, {0, 0, 5}, {0, 0, 1}, {0, 10}, result, diagnostic) == Status::Ok);
	CHECK_FALSE(result.Hit);
	CHECK(result.Depth == 10);
	sdf.Data->Shapes[0].Shape = 101;
	sdf.Data->Shapes[0].Size = {2, 4, 6};
	CHECK(Sample(sdf, {0, 0, 0}) == -1);
	CHECK(Sample(sdf, {3, 0, 0}) == 2);
	sdf.Data->Shapes[0].Rotation = {0, 0, 90};
	CHECK(Sample(sdf, {0, 3, 0}) == Catch::Approx(2).margin(1e-6));
}
TEST_CASE("Source SDF deformation order and owned payload copies", "[imagegraph][source_sdf]") {
	auto sdf = Sphere();
	sdf.Data->Shapes[0].Scale = 2;
	sdf.Data->Shapes[0].Position = {3, 0, 0};
	CHECK(Sample(sdf, {6, 0, 0}) == 1);
	auto copied = sdf;
	copied.Data->Shapes[0].Radius = 2;
	CHECK(Sample(sdf, {6, 0, 0}) == 1);
	CHECK(Sample(copied, {6, 0, 0}) == -1);
	sdf = Sphere();
	sdf.Data->Shapes[0].Elongate = {2, 0, 0};
	CHECK(Sample(sdf, {2, 0, 0}) == -1);
	CHECK(Sample(sdf, {4, 0, 0}) == 1);
	sdf = Sphere();
	sdf.Data->Shapes[0].Tile = true;
	sdf.Data->Shapes[0].TileDistance = {4, 4, 4};
	sdf.Data->Shapes[0].TileAmount = {0, 0, 0};
	CHECK(Sample(sdf, {4, 0, 0}) == -1);
	CHECK(Sample(sdf, {-2, 0, 0}) == 1);
	sdf = Sphere();
	sdf.Data->Shapes[0].WaveAmplitude = {1, 1, 1};
	sdf.Data->Shapes[0].WaveIntensity = {1, 1, 0};
	const float x = std::sin(1.f), y = 1.f + std::sin(x);
	CHECK(Sample(sdf, {0, 1, 0}) == Catch::Approx(std::sqrt(x * x + y * y) - 1).margin(1e-6));
}
TEST_CASE(
	"Source SDF postfix operations retain operand order and reject invalid resources",
	"[imagegraph][source_sdf]"
) {
	auto sdf = Sphere(2);
	auto second = sdf.Data->Shapes[0];
	second.Identity = "cut";
	second.Radius = 1;
	sdf.Data->Shapes.push_back(second);
	sdf.Data->Operations = {{0, 0}, {1, 0}, {102, .1}};
	CHECK(Sample(sdf, {0, 0, 0}) == Catch::Approx(1));
	CHECK(Sample(sdf, {1.5, 0, 0}) == Catch::Approx(-.475).margin(1e-6));
	sdf.Data->Operations.back() = {101, .1};
	CHECK(Sample(sdf, {0, 0, 0}) == Catch::Approx(-2));
	sdf.Data->Shapes[1].Radius = 2;
	CHECK(Sample(sdf, {0, 0, 0}) == Catch::Approx(-2.1).margin(1e-6));
	Diagnostic diagnostic;
	double distance = 123;
	sdf.Data->Operations = {{102, .1}};
	CHECK(SampleSourceSdf(sdf, {0, 0, 0}, distance, diagnostic) == Status::InvalidValue);
	CHECK(distance == 123);
	auto valid = Sphere();
	CHECK(
		SampleSourceSdf(valid, {std::numeric_limits<double>::infinity(), 0, 0}, distance, diagnostic) ==
		Status::InvalidValue
	);
}
TEST_CASE(
	"Source SDF graph producers carry actual typed scenes into a combined resource",
	"[imagegraph][source_sdf]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"outer", "pc.rm_primitive", "", {}, {{"shape", EnumValue{5}}, {"radius", 2.0}}},
		{"cut", "pc.rm_primitive", "", {}, {{"shape", EnumValue{5}}, {"radius", 1.0}}},
		{"combine", "pc.rm_combine", "", {}, {{"type", EnumValue{2}}, {"merge", .1}}}
	};
	document.Links = {
		{"outer", "sdf_object", "combine", "shape_1"}, {"cut", "sdf_object", "combine", "shape_2"}
	};
	document.Outputs = {{"result", "combine", "sdf_object"}};
	Plan plan;
	Diagnostic diagnostic;
	auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	EvaluatedValue result;
	status = EvaluateValue(document, plan, "result", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &sdf = std::get<SdfValue>(result.Data);
	REQUIRE(sdf.Data);
	CHECK(sdf.Data->Shapes.size() == 2);
	CHECK(sdf.Data->Operations.size() == 3);
	CHECK(Sample(sdf, {0, 0, 0}) == Catch::Approx(1));

	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "combine", {}, snapshot, diagnostic) == Status::Ok);
	SdfValue captured;
	REQUIRE(BuildSourceSdfObject(document.Nodes[2], snapshot, captured, diagnostic) == Status::Ok);
	CHECK(captured == sdf);
	const auto preserved = captured;
	CHECK(
		BuildSourceSdfObject(document.Nodes[2], snapshot, captured, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(captured == preserved);
	document.Nodes[0].Values[1].Data = 9.0;
	REQUIRE(BuildSourceSdfObject(document.Nodes[2], snapshot, captured, diagnostic) == Status::Ok);
	CHECK(captured == preserved);
	document.Links[1].FromNode = "outer";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "result", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<SdfValue>(result.Data).Data->Shapes.size() == 1);
}
TEST_CASE("Source SDF cross section writes actual distances to a float surface", "[imagegraph][source_sdf]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"sphere", "pc.rm_primitive", "", {}, {{"shape", EnumValue{5}}, {"radius", 1.0}}},
		{"cross",
		 "pc.rm_render_cross",
		 "",
		 {},
		 {{"dimension", Vector2{3, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{5}},
		  {"span", Vector2{3, 1}},
		  {"axis", EnumValue{2}}}}
	};
	document.Links = {{"sphere", "sdf_object", "cross", "sdf_object"}};
	document.Outputs = {{"cross", "cross", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	Image image;
	const auto evaluated = Evaluate(document, plan, "cross", image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	CHECK(image.Width == 3);
	CHECK(image.Height == 1);
	// The centre is inside the unit sphere; both side samples lie outside it.
	SurfacePixel left{}, centre{}, right{};
	REQUIRE(LoadSurfacePixel(image, 0, 0, left));
	REQUIRE(LoadSurfacePixel(image, 1, 0, centre));
	REQUIRE(LoadSurfacePixel(image, 2, 0, right));
	CHECK(centre[0] > left[0]);
	CHECK(left == right);
}

TEST_CASE(
	"Source SDF structural admission does not sample singular ellipsoid coordinates",
	"[imagegraph][source_sdf]"
) {
	auto sdf = Sphere();
	sdf.Data->Shapes[0].Shape = 201;
	REQUIRE(ValidateSourceSdfValue(sdf));
	Diagnostic diagnostic;
	double distance = 321;
	CHECK(SampleSourceSdf(sdf, {0, 0, 0}, distance, diagnostic) == Status::InvalidValue);
	CHECK(distance == 321);
	sdf.Data->Shapes[0].Scale = 0;
	CHECK_FALSE(ValidateSourceSdfValue(sdf));
}
TEST_CASE(
	"Source SDF graph refuses uniform overflow before publishing a composition", "[imagegraph][source_sdf]"
) {
	Document document;
	document.FormatVersion = 9;
	for (size_t index = 0; index < 17; ++index) {
		const auto id = "shape" + std::to_string(index);
		document.Nodes.push_back(
			{id,
			 "pc.rm_primitive",
			 "",
			 {},
			 {{"shape", EnumValue{5}}, {"position", Vector3{double(index) * 3, 0, 0}}}}
		);
		if (index == 0) continue;
		const auto combine = "join" + std::to_string(index);
		document.Nodes.push_back({combine, "pc.rm_combine", "", {}, {}});
		document.Links.push_back(
			{index == 1 ? "shape0" : "join" + std::to_string(index - 1), "sdf_object", combine, "shape_1"}
		);
		document.Links.push_back({id, "sdf_object", combine, "shape_2"});
	}
	document.Outputs = {{"accepted", "join15", "sdf_object"}, {"overflow", "join16", "sdf_object"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "accepted", {}, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<SdfValue>(result.Data).Data->Shapes.size() == 16);
	const auto saved = result.Data;
	CHECK(EvaluateValue(document, plan, "overflow", {}, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.Data == saved);
	CHECK(diagnostic.NodeId == "join16");
}
