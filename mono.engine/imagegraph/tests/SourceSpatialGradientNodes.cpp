#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_spatial_gradients")
using namespace engine::imagegraph;
namespace {
	Document Fixture(std::string type, std::vector<AuthoredValue> values = {}) {
		Document d;
		d.FormatVersion = 9;
		values.push_back({"dimension", Vector2{2, 2}});
		values.push_back({"dimension_unit", EnumValue{0}});
		values.push_back({"attribute_color_depth", EnumValue{3}});
		d.Nodes = {{"field", type, "", {}, std::move(values)}};
		d.Outputs = {{"image", "field", "surface_out"}};
		return d;
	}
	void Set(Node &n, std::string id, Value v) {
		for (auto &x : n.Values)
			if (x.Port == id) {
				x.Data = std::move(v);
				return;
			}
		n.Values.push_back({std::move(id), std::move(v)});
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		auto s = Compile(d, p, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(s == Status::Ok);
		return p;
	}
	Image Run(const Document &d, EvaluationRequest req = {}) {
		auto p = Compiled(d);
		Image out;
		Diagnostic diag;
		auto s = Evaluate(d, p, "image", req, out, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(s == Status::Ok);
		return out;
	}
	void Pixel(const Image &im, uint32_t x, uint32_t y, Colour c) {
		std::array<double, 4> p{};
		REQUIRE(LoadSurfacePixel(im, x, y, p));
		CHECK(p[0] == Catch::Approx(c.Red / 255.));
		CHECK(p[1] == Catch::Approx(c.Green / 255.));
		CHECK(p[2] == Catch::Approx(c.Blue / 255.));
		CHECK(p[3] == Catch::Approx(c.Alpha / 255.));
	}
	Document ColoredGrid() {
		auto d = Fixture(
			"pc.gradient_grid", {{"grid", Vector2{1, 1}}, {"subdivision", int64_t{1}}, {"smooth_mesh", 0.}}
		);
		const std::array pos{Vector2{0, 0}, Vector2{2, 0}, Vector2{0, 2}, Vector2{2, 2}};
		const std::array colors{
			Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}, Colour{0, 0, 255, 255}, Colour{255, 255, 255, 255}
		};
		for (size_t i = 0; i < 4; i++) {
			const auto suffix = std::to_string(i);
			d.Nodes[0].DynamicInputs.push_back({"anchor_i_" + suffix, ValueType::Vector2, pos[i]});
			d.Nodes[0].DynamicInputs.push_back({"color_i_" + suffix, ValueType::Colour, colors[i]});
		}
		for (size_t i = 0; i < 4; i++)
			d.Nodes[0].DynamicInputs.push_back(
				{"anchor_i_unit_" + std::to_string(i), ValueType::Enum, EnumValue{0}}
			);
		return d;
	}
}
TEST_CASE("Four-point sum versus L2 weights preserve source independent alpha", "[spatial_gradient]") {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_1", Colour{255, 0, 0, 64}},
		 {"color_2", Colour{0, 255, 0, 64}},
		 {"color_3", Colour{0, 0, 255, 64}},
		 {"color_4", Colour{0, 0, 0, 64}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	Pixel(Run(d), 0, 0, {64, 64, 64, 64});
	Set(d.Nodes[0], "normalize_weight", true);
	Pixel(Run(d), 0, 0, {128, 128, 128, 128});
}
TEST_CASE("Four-point short palette uses opaque black and ignores individual colors", "[spatial_gradient]") {
	ArrayValue pal;
	pal.ElementType = ValueType::Colour;
	pal.Elements = {Colour{255, 0, 0, 128}};
	auto d =
		Fixture("pc.gradient_points", {{"normalize_weight", false}, {"use_palette", true}, {"palette", pal}});
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	Pixel(Run(d), 0, 0, {64, 0, 0, 223});
}
TEST_CASE("Grid gradient source triangle topology has literal corner-color golden", "[spatial_gradient]") {
	auto d = ColoredGrid();
	const auto out = Run(d);
	Pixel(out, 0, 0, {128, 64, 64, 255});
	Pixel(out, 1, 0, {0, 191, 64, 255});
	Pixel(out, 0, 1, {0, 64, 191, 255});
	Pixel(out, 1, 1, {128, 191, 191, 255});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(Run(restored) == out);
}
TEST_CASE(
	"Grid default anchor reset renders white while moved quad leaves transparent exterior",
	"[spatial_gradient]"
) {
	auto d = Fixture("pc.gradient_grid");
	Pixel(Run(d), 0, 0, {255, 255, 255, 255});
	d = ColoredGrid();
	for (size_t i = 0; i < 4; i++) {
		auto &v = d.Nodes[0].DynamicInputs[i * 2].Default;
		auto &p = std::get<Vector2>(*v);
		p.X += 1;
	}
	auto im = Run(d);
	Pixel(im, 0, 0, {0, 0, 0, 0});
	Pixel(im, 1, 0, {128, 64, 64, 255});
}
TEST_CASE(
	"Grid subdivision quantizes nested merge colors before triangle alpha interpolation", "[spatial_gradient]"
) {
	auto d = ColoredGrid();
	Set(d.Nodes[0], "subdivision", int64_t{2});
	Pixel(Run(d), 0, 0, {127, 64, 64, 255});
	d.Nodes[0].DynamicInputs[1].Default = Colour{255, 0, 0, 0};
	d.Nodes[0].DynamicInputs[3].Default = Colour{0, 255, 0, 128};
	d.Nodes[0].DynamicInputs[5].Default = Colour{0, 0, 255, 128};
	d.Nodes[0].DynamicInputs[7].Default = Colour{255, 255, 255, 255};
	Pixel(Run(d), 0, 0, {127, 64, 64, 64});
}
TEST_CASE(
	"Spatial gradients refuse oversized work and undefined weights without replacing pixels",
	"[spatial_gradient]"
) {
	auto d = ColoredGrid();
	Set(d.Nodes[0], "subdivision", int64_t{4096});
	auto p = Compiled(d);
	Image out{1, 1, {17, 18, 19, 20}, 0};
	const auto prior = out;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "image", {}, out, diag) == Status::LimitExceeded);
	CHECK(out == prior);
	d = Fixture("pc.gradient_points");
	for (size_t i = 1; i <= 4; i++)
		Set(d.Nodes[0], "center_" + std::to_string(i), Vector2{.5, .5});
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	p = Compiled(d);
	CHECK(Evaluate(d, p, "image", {}, out, diag) == Status::UnsupportedExecution);
	CHECK(out == prior);
}
TEST_CASE(
	"Four-point RGB and source LMS mixing have independent literal color goldens", "[spatial_gradient]"
) {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_1", Colour{255, 0, 0, 255}},
		 {"color_2", Colour{0, 255, 0, 255}},
		 {"color_3", Colour{0, 0, 255, 255}},
		 {"color_4", Colour{255, 255, 255, 255}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	Pixel(Run(d), 0, 0, {128, 128, 128, 255});
	Set(d.Nodes[0], "color_space", EnumValue{1});
	Pixel(Run(d), 0, 0, {161, 170, 169, 255});
}
TEST_CASE(
	"Four-point UV alpha and zero mix keep the source independent UV alpha observation", "[spatial_gradient]"
) {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"uv_mix", 0.},
		 {"color_1", Colour{255, 0, 0, 255}},
		 {"color_2", Colour{255, 0, 0, 255}},
		 {"color_3", Colour{255, 0, 0, 255}},
		 {"color_4", Colour{255, 0, 0, 255}}}
	);
	d.Nodes.push_back({"uv", "image.captured", "", {}, {{"source_id", std::string("uv")}}});
	d.Links = {{"uv", "image", "field", "uv_map"}};
	std::array sources{RequestImageSource{"uv", Image{1, 1, {17, 203, 0, 128}, 0}}};
	EvaluationRequest req;
	req.ImageSources = sources;
	Pixel(Run(d, req), 0, 0, {255, 0, 0, 128});
	sources[0].Data.Pixels[3] = 0;
	Pixel(Run(d, req), 1, 1, {255, 0, 0, 0});
}
TEST_CASE(
	"Four-point color animation seeks reproduce source frame observations after persistence",
	"[spatial_gradient]"
) {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_1", Colour{255, 0, 0, 255}},
		 {"color_2", Colour{0, 0, 0, 255}},
		 {"color_3", Colour{0, 0, 0, 255}},
		 {"color_4", Colour{0, 0, 0, 255}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	d.Keyframes = {
		{"field", "color_1", 0, Colour{255, 0, 0, 255}, "linear"},
		{"field", "color_1", 10, Colour{0, 0, 255, 255}, "linear"}
	};
	EvaluationRequest req;
	const auto first = Run(d, req);
	Pixel(first, 0, 0, {64, 0, 0, 255});
	req.Tick = 10;
	Pixel(Run(d, req), 0, 0, {0, 0, 64, 255});
	req.Tick = 5;
	Pixel(Run(d, req), 0, 0, {32, 0, 32, 255});
	req.Tick = 0;
	CHECK(Run(d, req) == first);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Run(restored, req) == first);
}
TEST_CASE("Four-point image row batches retain all source color observations", "[spatial_gradient]") {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_2", Colour{0, 0, 0, 255}},
		 {"color_3", Colour{0, 0, 0, 255}},
		 {"color_4", Colour{0, 0, 0, 255}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	ArrayValue colors;
	colors.ElementType = ValueType::Colour;
	colors.Elements = {Colour{255, 0, 0, 255}, Colour{0, 0, 255, 255}};
	Set(d.Nodes[0], "color_1", colors);
	auto p = Compiled(d);
	ImageArray out;
	Diagnostic diag;
	const auto status = EvaluateArray(d, p, "image", {}, out, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Items.size() == 2);
	Pixel(out.Images[0], 0, 0, {64, 0, 0, 255});
	Pixel(out.Images[1], 0, 0, {0, 0, 64, 255});
	auto grid = ColoredGrid();
	ArrayValue topology;
	topology.ElementType = ValueType::Vector2;
	topology.Elements = {Vector2{1, 1}, Vector2{2, 2}};
	Set(grid.Nodes[0], "grid", topology);
	p = Compiled(grid);
	const auto prior = out;
	CHECK(EvaluateArray(grid, p, "image", {}, out, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "grid");
	CHECK(out.Images == prior.Images);
	CHECK(out.Items == prior.Items);
}
TEST_CASE(
	"Grid smoothing changes only geometry through the source nine-neighbor mean", "[spatial_gradient]"
) {
	auto d = Fixture(
		"pc.gradient_grid", {{"grid", Vector2{2, 2}}, {"subdivision", int64_t{1}}, {"smooth_mesh", 0.}}
	);
	Set(d.Nodes[0], "dimension", Vector2{4, 4});
	for (size_t i = 0; i < 9; i++) {
		const auto suffix = std::to_string(i);
		d.Nodes[0].DynamicInputs.push_back(
			{"anchor_i_" + suffix,
			 ValueType::Vector2,
			 i == 4 ? Vector2{1, 1} : Vector2{double(i % 3) * 2, double(i / 3) * 2}}
		);
		d.Nodes[0].DynamicInputs.push_back(
			{"color_i_" + suffix,
			 ValueType::Colour,
			 i == 4 ? Colour{255, 255, 255, 255} : Colour{0, 0, 0, 255}}
		);
		d.Nodes[0].DynamicInputs.push_back({"anchor_i_unit_" + suffix, ValueType::Enum, EnumValue{0}});
	}
	Pixel(Run(d), 1, 1, {191, 191, 191, 255});
	Set(d.Nodes[0], "smooth_mesh", .5);
	Pixel(Run(d), 1, 1, {246, 246, 246, 255});
	Set(d.Nodes[0], "smooth_mesh", 1.);
	Pixel(Run(d), 1, 1, {143, 143, 143, 255});
}
TEST_CASE(
	"Four-point UV XY mapping flips its green channel before computing source distances", "[spatial_gradient]"
) {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_1", Colour{255, 0, 0, 255}},
		 {"color_2", Colour{0, 255, 0, 255}},
		 {"color_3", Colour{0, 0, 255, 255}},
		 {"color_4", Colour{255, 255, 255, 255}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	d.Nodes.push_back({"uv", "image.captured", "", {}, {{"source_id", std::string("uv")}}});
	d.Links = {{"uv", "image", "field", "uv_map"}};
	std::array sources{RequestImageSource{"uv", Image{1, 1, {0, 255, 0, 255}, 0}}};
	EvaluationRequest req;
	req.ImageSources = sources;
	Pixel(Run(d, req), 0, 0, {223, 19, 19, 255});
	Set(d.Nodes[0], "uv_mix", 0.);
	Pixel(Run(d, req), 0, 0, {128, 128, 128, 255});
}
TEST_CASE("Four-point center getters use unrounded non-square source dimensions", "[spatial_gradient]") {
	auto d = Fixture(
		"pc.gradient_points",
		{{"normalize_weight", false},
		 {"color_1", Colour{255, 0, 0, 255}},
		 {"color_2", Colour{0, 255, 0, 255}},
		 {"color_3", Colour{0, 0, 255, 255}},
		 {"color_4", Colour{255, 255, 255, 255}}}
	);
	Set(d.Nodes[0], "dimension", Vector2{3.3, 1.3});
	const auto reference = Run(d);
	CHECK(reference.Width == 3);
	CHECK(reference.Height == 1);
	for (size_t i = 0; i < 4; i++) {
		const auto suffix = std::to_string(i + 1);
		Set(d.Nodes[0], "center_" + suffix, Vector2{double(i % 2) * 3.3, double(i / 2) * 1.3});
		Set(d.Nodes[0], "center_" + suffix + "_unit", EnumValue{0});
	}
	CHECK(Run(d) == reference);
}
TEST_CASE("Spatial gradient output feeds a real masked image compositor", "[spatial_gradient]") {
	auto d = ColoredGrid();
	d.Nodes.push_back(
		{"background",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 0, 0, 255}}}}
	);
	d.Nodes.push_back({"mask", "image.captured", "", {}, {{"source_id", std::string("mask")}}});
	d.Nodes.push_back({"composite", "image.blend", "", {}, {{"mask_alpha_only", true}}});
	d.Links = {
		{"background", "image", "composite", "background"},
		{"field", "surface_out", "composite", "foreground"},
		{"mask", "image", "composite", "mask"}
	};
	d.Outputs = {{"image", "composite", "image"}};
	std::array sources{RequestImageSource{
		"mask", Image{2, 2, {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 255}, 0}
	}};
	EvaluationRequest req;
	req.ImageSources = sources;
	const auto out = Run(d, req);
	Pixel(out, 0, 0, {128, 64, 64, 255});
	Pixel(out, 1, 0, {0, 0, 0, 255});
	Pixel(out, 0, 1, {0, 0, 0, 255});
	Pixel(out, 1, 1, {128, 191, 191, 255});
}
TEST_CASE("Grid final corner preserves source color-remapped coordinate quirk", "[spatial_gradient]") {
	auto d = ColoredGrid();
	Set(d.Nodes[0], "smooth_color", 2.);
	// Source final vertex is (1.5,2), not (2,2); the selected triangle weights are 1/4,1/12,2/3.
	Pixel(Run(d), 1, 1, {138, 180, 159, 255});
}
TEST_CASE(
	"Four-point whole image batch work is refused before generated images replace prior rows",
	"[spatial_gradient]"
) {
	auto d = Fixture("pc.gradient_points");
	Set(d.Nodes[0], "dimension", Vector2{2048, 2048});
	ArrayValue colors;
	colors.ElementType = ValueType::Colour;
	colors.Elements.assign(5, Colour{255, 255, 255, 255});
	Set(d.Nodes[0], "color_1", colors);
	auto p = Compiled(d);
	ImageArray out;
	out.Images.push_back(Image{1, 1, {1, 2, 3, 4}, 0});
	const auto images = out.Images;
	Diagnostic diag;
	CHECK(EvaluateArray(d, p, "image", {}, out, diag) == Status::LimitExceeded);
	CHECK(diag.Port == "dimension");
	CHECK(out.Images == images);
}
TEST_CASE(
	"Grid vertex workspace refuses a caller cap before publishing the generated surface", "[spatial_gradient]"
) {
	auto d = ColoredGrid();
	Set(d.Nodes[0], "subdivision", int64_t{63});
	auto p = Compiled(d);
	Image out{1, 1, {17, 18, 19, 20}, 0};
	const auto prior = out;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "image", {}, out, diag, 131072) == Status::LimitExceeded);
	CHECK(diag.Port == "subdivision");
	CHECK(out == prior);
}
TEST_CASE(
	"Spatial batch preflight sees an expensive second dimension before first output allocation",
	"[spatial_gradient]"
) {
	for (const auto type : {"pc.gradient_points", "pc.gradient_grid"}) {
		auto d = Fixture(type);
		if (type == std::string_view("pc.gradient_grid")) {
			Set(d.Nodes[0], "grid", Vector2{1, 1});
			Set(d.Nodes[0], "subdivision", int64_t{1});
			Set(d.Nodes[0], "smooth_mesh", 0.);
		}
		ArrayValue sizes;
		sizes.ElementType = ValueType::Vector2;
		sizes.Elements = {Vector2{512, 512}, Vector2{4096, 4096}};
		Set(d.Nodes[0], "dimension", sizes);
		const auto plan = Compiled(d);
		Image output{1, 1, {11, 12, 13, 14}, 0};
		const auto prior = output;
		Diagnostic diagnostic;
		// The first output alone needs 1MiB. A work diagnostic under 512KiB proves no first-row allocation.
		CHECK(Evaluate(d, plan, "image", {}, output, diagnostic, 524288) == Status::LimitExceeded);
		CHECK(diagnostic.Message.find("processor work budget") != std::string::npos);
		CHECK(
			diagnostic.Port == (type == std::string_view("pc.gradient_points") ? "dimension" : "subdivision")
		);
		CHECK(output == prior);
		auto firstOnly = d;
		Set(firstOnly.Nodes[0], "dimension", Vector2{512, 512});
		CHECK(
			Evaluate(firstOnly, Compiled(firstOnly), "image", {}, output, diagnostic, 524288) ==
			Status::LimitExceeded
		);
		CHECK(diagnostic.Port == "surface_out");
		CHECK(diagnostic.Message.find("processor work budget") == std::string::npos);
		CHECK(output == prior);
	}
}
TEST_CASE(
	"Grid batch preflight sees later subdivision and smoothing before vertex or image allocation",
	"[spatial_gradient]"
) {
	for (const auto control : {"subdivision", "smooth_mesh"}) {
		auto d = Fixture(
			"pc.gradient_grid", {{"grid", Vector2{1, 1}}, {"subdivision", int64_t{1}}, {"smooth_mesh", 0.}}
		);
		Set(d.Nodes[0], "dimension", Vector2{512, 512});
		ArrayValue values;
		values.ElementType =
			control == std::string_view("subdivision") ? ValueType::Integer : ValueType::Scalar;
		values.Elements = control == std::string_view("subdivision")
							  ? std::vector<ElementValue>{int64_t{1}, int64_t{9}}
							  : std::vector<ElementValue>{double{0}, double{2000000}};
		Set(d.Nodes[0], control, values);
		const auto plan = Compiled(d);
		Image output{1, 1, {21, 22, 23, 24}, 0};
		const auto prior = output;
		Diagnostic diagnostic;
		CHECK(Evaluate(d, plan, "image", {}, output, diagnostic, 524288) == Status::LimitExceeded);
		CHECK(diagnostic.Message == "Grid gradient exceeds processor work budget");
		CHECK(diagnostic.Port == "subdivision");
		CHECK(output == prior);
	}
}
TEST_CASE(
	"Spatial preflight preserves actual dimension and topology selection in each accepted row",
	"[spatial_gradient]"
) {
	for (const auto type : {"pc.gradient_points", "pc.gradient_grid"}) {
		auto document = Fixture(type);
		if (type == std::string_view("pc.gradient_grid")) {
			Set(document.Nodes[0], "grid", Vector2{1, 1});
			Set(document.Nodes[0], "smooth_mesh", 0.);
			ArrayValue subdivisions;
			subdivisions.ElementType = ValueType::Integer;
			subdivisions.Elements = {int64_t{1}, int64_t{2}};
			Set(document.Nodes[0], "subdivision", subdivisions);
		}
		ArrayValue dimensions;
		dimensions.ElementType = ValueType::Vector2;
		dimensions.Elements = {Vector2{2, 1}, Vector2{4, 3}};
		Set(document.Nodes[0], "dimension", dimensions);
		ImageArray actual;
		Diagnostic diagnostic;
		REQUIRE(EvaluateArray(document, Compiled(document), "image", {}, actual, diagnostic) == Status::Ok);
		REQUIRE(actual.Images.size() == 2);
		for (size_t row = 0; row < 2; ++row) {
			auto independent = document;
			Set(independent.Nodes[0], "dimension", std::get<Vector2>(dimensions.Elements[row]));
			if (type == std::string_view("pc.gradient_grid"))
				Set(independent.Nodes[0], "subdivision", int64_t(row + 1));
			CHECK(actual.Images[row] == Run(independent));
		}
	}
}
