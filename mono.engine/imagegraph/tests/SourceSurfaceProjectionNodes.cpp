#include "NodeHarness.hpp"
#include "nodes/Sampler.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_surface_projection")
using namespace engine::imagegraph;
namespace {
	Document ProjectGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"front",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}},
			{"right",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 255, 0, 255}}}},
			{"top",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 0, 255, 255}}}},
			{"project",
			 "pc.surface_project_3_d",
			 "",
			 {},
			 {{"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"view_angle", Vector3{}},
			  {"scale", 2.},
			  {"noise_seed", 0.},
			  {"attribute_color_depth", EnumValue{5}},
			  {"interpolate", EnumValue{1}}}}
		};
		doc.Links = {
			{"front", "image", "project", "front"},
			{"right", "image", "project", "right"},
			{"top", "image", "project", "top"}
		};
		doc.Outputs = {
			{"colour", "project", "surface_out"},
			{"depth", "project", "depth_pass"},
			{"normal", "project", "normal_pass"}
		};
		return doc;
	}
	void SetProject(Document &doc, std::string_view id, Value v) {
		for (auto &entry : doc.Nodes[3].Values)
			if (entry.Port == id) {
				entry.Data = std::move(v);
				return;
			}
		doc.Nodes[3].Values.push_back({std::string(id), std::move(v)});
	}
	Image ProjectImage(
		const Document &doc, std::string_view output = "colour", const EvaluationRequest &request = {}
	) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto evaluated = Evaluate(doc, plan, std::string(output), request, image, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return image;
	}
}
TEST_CASE(
	"Surface Project3D retains physical atlas order and source entry depth through save",
	"[source_surface_projection]"
) {
	const auto doc = ProjectGraph();
	const auto colour = ProjectImage(doc), depth = ProjectImage(doc, "depth"),
			   normal = ProjectImage(doc, "normal");
	CHECK(detail::ReadPixel(colour, 1, 1) == detail::Rgba{1, 0, 0, 1});
	CHECK(detail::ReadPixel(normal, 1, 1) == detail::Rgba{0, 0, 1, 1});
	const double expected = (std::sqrt(3.) - 1) * std::sqrt(1 + 2e-6) / 2;
	CHECK(detail::ReadPixel(depth, 1, 1)[0] == Catch::Approx(expected).margin(2e-6));
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	CHECK(ProjectImage(restored) == colour);
	CHECK(ProjectImage(restored, "depth") == depth);
}
TEST_CASE(
	"Surface Project3D face average and except choices use literal shader axes", "[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	SetProject(doc, "voxel_color", EnumValue{1});
	auto pixel = detail::ReadPixel(ProjectImage(doc), 1, 1);
	for (size_t c = 0; c < 3; ++c)
		CHECK(pixel[c] == Catch::Approx(1. / 3).margin(2e-7));
	SetProject(doc, "voxel_color", EnumValue{2});
	SetProject(doc, "except", EnumValue{0});
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 1) == detail::Rgba{1, 0, 0, 1});
	SetProject(doc, "except", EnumValue{1});
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 1) == detail::Rgba{.5, 0, .5, 1});
	SetProject(doc, "except", EnumValue{2});
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 1) == detail::Rgba{.5, .5, 0, 1});
}
TEST_CASE(
	"Surface Project3D fixed shader noise thresholds define complete occupancy extremes",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	const auto base = ProjectImage(doc);
	SetProject(doc, "use_noise", true);
	SetProject(doc, "threshold", 1.);
	SetProject(doc, "noise_seed", 12345.);
	CHECK(ProjectImage(doc) == base);
	SetProject(doc, "threshold", -1.);
	const auto empty = ProjectImage(doc);
	for (uint8_t byte : empty.Pixels)
		CHECK(byte == 0);
	for (uint8_t byte : ProjectImage(doc, "normal").Pixels)
		CHECK(byte == 0);
}
TEST_CASE(
	"Surface Project3D palette uses front and back face index rather than texture slot",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	ArrayValue palette;
	palette.ElementType = ValueType::Colour;
	palette.Elements = {
		Colour{255, 255, 255, 255},
		Colour{255, 255, 255, 255},
		Colour{255, 255, 255, 255},
		Colour{128, 255, 255, 255}
	};
	SetProject(doc, "face_blending", palette);
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 1)[0] == Catch::Approx(128 / 255.).margin(2e-7));
}
TEST_CASE(
	"Surface Project3D absent primaries use original flags and scalar surfaces", "[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Links.resize(1);
	const auto image = ProjectImage(doc);
	CHECK(detail::ReadPixel(image, 1, 1) == detail::Rgba{1, 0, 0, 1});
	doc.Links.clear();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {12, 34, 56, 255}};
	const auto previous = sentinel;
	CHECK(Evaluate(doc, plan, "colour", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(sentinel == previous);
	CHECK(diagnostic.Message.find("ambient") != std::string::npos);
}
TEST_CASE(
	"Surface Project3D supports each precision surface without hidden format conversion",
	"[source_surface_projection]"
) {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto doc = ProjectGraph();
		SetProject(doc, "attribute_color_depth", EnumValue{depth});
		const auto image = ProjectImage(doc);
		REQUIRE(ValidSurfaceLayout(image, 4096, 1 << 20));
		CHECK(FiniteSurfaceSamples(image));
		const auto pixel = detail::ReadPixel(image, 1, 1);
		CHECK(pixel[0] == 1);
		constexpr SurfaceFormat formats[]{
			SurfaceFormat::RGBA4Unorm,
			SurfaceFormat::RGBA8Unorm,
			SurfaceFormat::RGBA16Float,
			SurfaceFormat::RGBA32Float,
			SurfaceFormat::R8Unorm,
			SurfaceFormat::R16Float,
			SurfaceFormat::R32Float
		};
		CHECK(image.Format == formats[depth - 2]);
	}
}
TEST_CASE(
	"Surface Project3D original later rows are admitted before owned output allocation",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Nodes.push_back({"dimensions", "pc.array", "", {}, {}});
	doc.Nodes.back().DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{4, 4}}, {"input_1", ValueType::Vector2, Vector2{4096, 4096}}
	};
	doc.Links.push_back({"dimensions", "array", "project", "dimension"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {11, 22, 33, 255}});
	const auto previous = sentinel;
	CHECK(EvaluateArray(doc, plan, "colour", {}, sentinel, diagnostic) == Status::LimitExceeded);
	CHECK(sentinel.Images == previous.Images);
	CHECK(sentinel.Items.empty());
}
TEST_CASE(
	"Surface Project3D undefined shader depth leaves previous consumer image intact",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	SetProject(doc, "depth_range", Vector2{1, 1});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {11, 22, 33, 255}};
	const auto previous = sentinel;
	CHECK(Evaluate(doc, plan, "colour", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(sentinel == previous);
	doc = ProjectGraph();
	SetProject(doc, "scale", 0.);
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(doc, plan, "colour", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(sentinel == previous);
}
TEST_CASE(
	"Surface Project3D ordered dimension rows match independently resolved scalar cameras",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Nodes.push_back({"dimensions", "pc.array", "", {}, {}});
	doc.Nodes.back().DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{4, 4}}, {"input_1", ValueType::Vector2, Vector2{6, 6}}
	};
	doc.Links.push_back({"dimensions", "array", "project", "dimension"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	const auto status = EvaluateArray(doc, plan, "colour", {}, rows, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	CHECK(rows.Images[0] == ProjectImage(ProjectGraph()));
	auto scalar = ProjectGraph();
	SetProject(scalar, "dimension", Vector2{6, 6});
	CHECK(rows.Images[1] == ProjectImage(scalar));
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray saved;
	REQUIRE(EvaluateArray(restored, plan, "colour", {}, saved, diagnostic) == Status::Ok);
	CHECK(saved.Images == rows.Images);
}
TEST_CASE(
	"Surface Project3D reverse axial camera reads crossed source back atlas", "[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Nodes.push_back(
		{"back",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 0, 255}}}}
	);
	doc.Links.push_back({"back", "image", "project", "back"});
	SetProject(doc, "view_angle", Vector3{0, 180, 0});
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 1) == detail::Rgba{1, 1, 0, 1});
	SetProject(doc, "position", Vector3{100, 0, 0});
	const auto empty = ProjectImage(doc);
	for (uint8_t byte : empty.Pixels)
		CHECK(byte == 0);
}
TEST_CASE(
	"Surface Project3D orthographic scale and explicit depth range preserve geometric distance",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	const auto initial = detail::ReadPixel(ProjectImage(doc, "depth"), 1, 1)[0];
	SetProject(doc, "scale", 1.);
	CHECK(detail::ReadPixel(ProjectImage(doc, "depth"), 1, 1)[0] == Catch::Approx(initial * 2).margin(2e-6));
	SetProject(doc, "depth_range", Vector2{0, 2});
	CHECK(detail::ReadPixel(ProjectImage(doc, "depth"), 1, 1)[0] == Catch::Approx(initial).margin(2e-6));
	SetProject(doc, "projection", EnumValue{0});
	SetProject(doc, "distance", 1.);
	SetProject(doc, "fov", 30.);
	const auto perspective = ProjectImage(doc);
	CHECK(detail::ReadPixel(perspective, 1, 1) == detail::Rgba{1, 0, 0, 1});
}
TEST_CASE(
	"Surface Project3D declared Surface getters retain tuple and scalar row contracts",
	"[source_surface_projection]"
) {
	for (std::string_view port :
		 {"view_angle", "position", "depth_range", "fov", "distance", "scale", "threshold"}) {
		auto doc = ProjectGraph();
		doc.Nodes.push_back(
			{"getter",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}}
		);
		doc.Links.push_back({"getter", "image", "project", std::string(port)});
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationSnapshot snapshot;
		status = EvaluateNodeInputs(doc, plan, "project", {}, snapshot, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *value = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) value = &input.Data;
		REQUIRE(value);
		if (port == "view_angle" || port == "position")
			REQUIRE(*value == Value{Vector3{1, 2, 0}});
		else if (port == "depth_range")
			REQUIRE(*value == Value{Vector2{1, 2}});
		else {
			const auto *array = std::get_if<ArrayValue>(value);
			REQUIRE(array);
			REQUIRE(array->Elements == std::vector<ElementValue>{1., 2.});
			ImageArray result;
			status = EvaluateArray(doc, plan, "depth", {}, result, diagnostic);
			INFO(diagnostic.Port << ":" << diagnostic.Message);
			REQUIRE(status == Status::Ok);
			REQUIRE(result.Images.size() == 2);
			for (size_t i = 0; i < 2; ++i) {
				auto expected = ProjectGraph();
				SetProject(expected, port, double(i + 1));
				REQUIRE(result.Images[i].Pixels == ProjectImage(expected, "depth").Pixels);
			}
		}
	}
}
TEST_CASE(
	"Surface Project3D whole Surface-array getters project nonsurface dimensions before batching",
	"[source_surface_projection]"
) {
	for (std::string_view port :
		 {"view_angle", "position", "depth_range", "fov", "distance", "scale", "threshold"}) {
		auto doc = ProjectGraph();
		doc.Nodes.push_back(
			{"getter", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
		);
		doc.Junctions.push_back(
			{"widths", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 3}, Vector2{4, 3}}}}
		);
		doc.Links.push_back({"widths", "value", "getter", "dimension"});
		doc.Links.push_back({"getter", "surface_out", "project", std::string(port)});
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationSnapshot snapshot;
		status = EvaluateNodeInputs(doc, plan, "project", {}, snapshot, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const Value *value = nullptr;
		for (const auto &input : snapshot.Values())
			if (input.Port == port) value = &input.Data;
		REQUIRE(value);
		if (port == "view_angle" || port == "position")
			REQUIRE(*value == Value{Vector3{1, 1, 0}});
		else if (port == "depth_range")
			REQUIRE(*value == Value{Vector2{1, 1}});
		else {
			const auto *array = std::get_if<ArrayValue>(value);
			REQUIRE(array);
			REQUIRE(array->Elements == std::vector<ElementValue>{1., 1.});
		}
	}
}
TEST_CASE(
	"Surface Project3D oversized back draws spill across packed cells with source alpha blending",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	SetProject(doc, "view_angle", Vector3{0, 180, 0});
	SetProject(doc, "voxel_color", EnumValue{1});
	doc.Nodes.push_back(
		{"back",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{4}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 0, 255}}}}
	);
	doc.Nodes.push_back(
		{"left",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 0}}}}
	);
	doc.Links.push_back({"back", "image", "project", "back"});
	doc.Links.push_back({"left", "image", "project", "left"});
	const auto pixel = detail::ReadPixel(ProjectImage(doc), 1, 1);
	CHECK(pixel[0] == Catch::Approx(2. / 3).margin(2e-7));
	CHECK(pixel[1] == Catch::Approx(2. / 3).margin(2e-7));
	CHECK(pixel[2] == Catch::Approx(1. / 3).margin(2e-7));
	CHECK(pixel[3] == 1);
}
TEST_CASE(
	"Surface Project3D opposite-side alpha thresholds distinguish one-sided extrusion",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	for (size_t i = 0; i < 3; ++i)
		std::get<Colour>(doc.Nodes[i].Values[2].Data).Alpha = 128;
	const auto both = ProjectImage(doc);
	for (uint32_t y = 0; y < both.Height; ++y)
		for (uint32_t x = 0; x < both.Width; ++x)
			CHECK(detail::ReadPixel(both, x, y) == detail::Rgba{});
	SetProject(doc, "extrude_both_side", false);
	CHECK(detail::ReadPixel(ProjectImage(doc), 1, 2) == detail::Rgba{1, 0, 0, 1});
}
TEST_CASE(
	"Surface Project3D source general tuples retain physical row coordinates", "[source_surface_projection]"
) {
	for (const auto &[port, json, expectedValue] : std::vector<std::tuple<std::string, std::string, Value>>{
			 {"dimension", R"([4,4,"unused"])", Vector2{4, 4}},
			 {"view_angle", R"([0,180,0,"unused"])", Vector3{0, 180, 0}},
			 {"position", R"([100,0,0,"unused"])", Vector3{100, 0, 0}},
			 {"depth_range", R"([0,2,"unused"])", Vector2{0, 2}}
		 }) {
		auto doc = ProjectGraph();
		doc.Nodes.push_back({"json", "pc.struct_json_parse", "", {}, {{"json_string", json}}});
		doc.Nodes.push_back({"array", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}});
		doc.Nodes.back().DynamicInputs = {{"input_0", ValueType::Any, std::nullopt}};
		doc.Links.push_back({"json", "struct", "array", "input_0"});
		doc.Links.push_back({"array", "array", "project", port});
		auto expected = ProjectGraph();
		SetProject(expected, port, expectedValue);
		for (std::string_view output : {"colour", "depth", "normal"})
			CHECK(ProjectImage(doc, output).Pixels == ProjectImage(expected, output).Pixels);
	}
}
TEST_CASE(
	"Surface Project3D three attachments share a bounded operation byte budget", "[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	SetProject(doc, "dimension", Vector2{64, 64});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {12, 34, 56, 255}};
	const auto previous = sentinel;
	const auto status = Evaluate(doc, plan, "colour", {}, sentinel, diagnostic, 96 * 1024);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(sentinel == previous);
}
TEST_CASE(
	"Surface Project3D projected later Distance row is admitted before first attachment",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	SetProject(doc, "projection", EnumValue{0});
	doc.Nodes.push_back(
		{"getter",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{4096}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	doc.Links.push_back({"getter", "image", "project", "distance"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {12, 34, 56, 255}});
	const auto previous = sentinel;
	const auto status = EvaluateArray(doc, plan, "colour", {}, sentinel, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(sentinel.Images == previous.Images);
	CHECK(sentinel.Items == previous.Items);
}
TEST_CASE(
	"Surface Project3D Palette getter wraps a scalar Colour producer without batching its channels",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Junctions.push_back({"tint", "", ValueType::Colour, Colour{128, 255, 255, 17}});
	doc.Links.push_back({"tint", "value", "project", "face_blending"});
	auto expected = ProjectGraph();
	SetProject(expected, "face_blending", ArrayValue{ValueType::Colour, {Colour{128, 255, 255, 17}}});
	const auto image = ProjectImage(doc);
	CHECK(image.Pixels == ProjectImage(expected).Pixels);
	CHECK(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(128. / 255).margin(2e-7));
	CHECK(detail::ReadPixel(image, 1, 1)[3] == 1);
}
TEST_CASE(
	"Surface Project3D ordered source face surfaces retain processor rows", "[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Nodes[0] = {
		"front", "pc.solid", "", {}, {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}
	};
	doc.Links[0].FromPort = "surface_out";
	doc.Junctions.push_back(
		{"colours",
		 "",
		 ValueType::Array,
		 ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}}}}
	);
	doc.Links.push_back({"colours", "value", "front", "color"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray result;
	const auto status = EvaluateArray(doc, plan, "colour", {}, result, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 2);
	CHECK(detail::ReadPixel(result.Images[0], 1, 1) == detail::Rgba{1, 0, 0, 1});
	CHECK(detail::ReadPixel(result.Images[1], 1, 1) == detail::Rgba{0, 1, 0, 1});
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	Plan restoredPlan;
	REQUIRE(Compile(restored, restoredPlan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, restoredPlan, "colour", {}, replay, diagnostic) == Status::Ok);
	CHECK(replay.Images == result.Images);
}
TEST_CASE(
	"Surface Project3D Palette retains defined integral packed Colour transport",
	"[source_surface_projection]"
) {
	auto expected = ProjectGraph();
	SetProject(expected, "face_blending", ArrayValue{ValueType::Colour, {Colour{128, 255, 255, 255}}});
	auto doc = ProjectGraph();
	SetProject(doc, "face_blending", ArrayValue{ValueType::Integer, {int64_t{0x00ffff80}}});
	CHECK(ProjectImage(doc).Pixels == ProjectImage(expected).Pixels);
	doc = ProjectGraph();
	doc.Junctions.push_back({"tint", "", ValueType::Integer, int64_t{0x00ffff80}});
	doc.Links.push_back({"tint", "value", "project", "face_blending"});
	CHECK(ProjectImage(doc).Pixels == ProjectImage(expected).Pixels);
}

TEST_CASE(
	"Surface Project3D scalar unit attribute refuses arrays and original native unit bounds allocate nothing",
	"[source_surface_projection]"
) {
	auto doc = ProjectGraph();
	doc.Project = ProjectSettings{.SurfaceWidth = 4096, .SurfaceHeight = 4096};
	doc.Junctions.push_back({"units", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 1.}}});
	doc.Links.push_back({"units", "value", "project", "dimension_unit"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {12, 34, 56, 255}});
	const auto previous = sentinel;
	EvaluationRequest publicRequest;
	publicRequest.MaximumImageDimension = 64;
	const auto status = EvaluateArray(doc, plan, "colour", publicRequest, sentinel, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	CHECK(status == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "dimension_unit");
	CHECK(diagnostic.Message == "integer reader cannot consume an array input");
	CHECK(sentinel.Images == previous.Images);
	CHECK(sentinel.Items == previous.Items);

	const auto *entry = FindCatalogueEntry("pc.surface_project_3_d");
	const auto executor = detail::FindExecutor("pc.surface_project_3_d");
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(doc.Nodes[3], *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Project.SurfaceWidth = doc.Project->SurfaceWidth;
	context.Project.SurfaceHeight = doc.Project->SurfaceHeight;
	context.Values = {
		{"dimension", Vector2{4, 4}},
		{"dimension_unit", EnumValue{0}},
		{"face_blending", ArrayValue{ValueType::Colour, {Colour{255, 255, 255, 255}}}}
	};
	Value originalUnits = ArrayValue{ValueType::Scalar, {0., 1.}};
	std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"dimension_unit", &originalUnits}}};
	context.ProcessorOriginalValues = originals;
	context.ProcessorCount = 2;
	Image face{2, 2, std::vector<uint8_t>(16, 255)};
	context.Images.emplace_back("front", &face);
	CHECK_FALSE(executor(context));
	INFO(context.FailurePort << ":" << context.FailureMessage);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
	CHECK(budget.Peak() == 0);
}
