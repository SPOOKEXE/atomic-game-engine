#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_gradient_points_n")
using namespace engine::imagegraph;
namespace {
	void Set(Node &n, std::string port, Value v) {
		for (auto &x : n.Values)
			if (x.Port == port) {
				x.Data = std::move(v);
				return;
			}
		n.Values.push_back({std::move(port), std::move(v)});
	}
	Document Fixture(int64_t mode = 0) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"field",
			 "pc.gradient_points_n",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"blend_mode", EnumValue{mode}},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		d.Outputs = {{"image", "field", "surface_out"}};
		return d;
	}
	void Point(Document &d, Vector2 p, Colour color, double influence = 6, int64_t unit = 0) {
		size_t count = 0;
		for (const auto &input : d.Nodes[0].DynamicInputs)
			if (input.Id.starts_with("point_i_") && !input.Id.starts_with("point_i_unit_")) ++count;
		auto suffix = std::to_string(count);
		auto &inputs = d.Nodes[0].DynamicInputs;
		inputs.push_back({"point_i_" + suffix, ValueType::Vector2, p});
		inputs.push_back({"color_i_" + suffix, ValueType::Colour, color});
		inputs.push_back({"influence_i_" + suffix, ValueType::Scalar, influence});
		inputs.push_back({"point_i_unit_" + suffix, ValueType::Enum, EnumValue{unit}});
	}
	Plan Compiled(const Document &d) {
		Plan plan;
		Diagnostic diag;
		const auto s = Compile(d, plan, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(s == Status::Ok);
		return plan;
	}
	Image Run(const Document &d, EvaluationRequest request = {}) {
		Image out;
		Diagnostic diag;
		const auto plan = Compiled(d);
		const auto s = Evaluate(d, plan, "image", request, out, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(s == Status::Ok);
		return out;
	}
	void
	Pixel(const Image &im, uint32_t x, uint32_t y, std::array<double, 4> expected, double epsilon = 1e-6) {
		std::array<double, 4> actual{};
		REQUIRE(LoadSurfacePixel(im, x, y, actual));
		for (size_t i = 0; i < 4; ++i)
			CHECK(actual[i] == Catch::Approx(expected[i]).margin(epsilon));
	}
	void BytePixel(const Image &im, uint32_t x, uint32_t y, Colour p) {
		Pixel(im, x, y, {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.});
	}
	void Refused(
		const Document &d,
		Status wanted,
		std::string_view port,
		EvaluationRequest req = {},
		uint64_t budget = Limits::MaximumOutputBytes
	) {
		auto plan = Compiled(d);
		Image out{1, 1, {17, 18, 19, 20}, 0};
		auto prior = out;
		Diagnostic diag;
		CHECK(Evaluate(d, plan, "image", req, out, diag, budget) == wanted);
		INFO(diag.Message);
		CHECK(diag.Port == port);
		CHECK(out == prior);
	}
} // namespace
TEST_CASE("N-point zero inputs produce source opaque black in every weight mode", "[gradient_points_n]") {
	for (int mode = 0; mode < 3; ++mode)
		BytePixel(Run(Fixture(mode)), 0, 0, {0, 0, 0, 255});
}
TEST_CASE(
	"N-point single Exponential color retains source initial alpha in "
	"float surfaces",
	"[gradient_points_n]"
) {
	auto d = Fixture();
	Point(d, {0, 0}, {64, 128, 192, 64});
	BytePixel(Run(d), 0, 0, {64, 128, 192, 255});
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Pixel(Run(d), 0, 0, {64 / 255., 128 / 255., 192 / 255., 1 + 64 / 255.});
}
TEST_CASE("N-point equal distances distinguish L2 from Gaussian normalization", "[gradient_points_n]") {
	auto d = Fixture();
	Point(d, {0, .5}, {255, 0, 0, 64}, 1);
	Point(d, {1, .5}, {0, 0, 255, 128}, 1);
	BytePixel(Run(d), 0, 0, {180, 0, 180, 255});
	Set(d.Nodes[0], "blend_mode", EnumValue{1});
	BytePixel(Run(d), 0, 0, {128, 0, 128, 255});
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Pixel(Run(d), 0, 0, {.5, 0, .5, 1 + 96 / 255.});
}
TEST_CASE("N-point Exponential uses each influence before L2 normalization", "[gradient_points_n]") {
	auto d = Fixture();
	Point(d, {0, .5}, {255, 0, 0, 255}, 6);
	Point(d, {1, .5}, {0, 0, 255, 255}, 0);
	BytePixel(Run(d), 0, 0, {4, 0, 255, 255});
}
TEST_CASE(
	"N-point Gaussian source distance range uses raw width and permits "
	"signed range",
	"[gradient_points_n]"
) {
	auto d = Fixture(1);
	Point(d, {.5, .5}, {255, 0, 0, 255}, 1);
	Point(d, {1.5, .5}, {0, 0, 255, 255}, 1);
	BytePixel(Run(d), 0, 0, {186, 0, 69, 255});
	d.Nodes[0].DynamicInputs[2].Default = -1.;
	d.Nodes[0].DynamicInputs[6].Default = -1.;
	BytePixel(Run(d), 0, 0, {186, 0, 69, 255});
}
TEST_CASE("N-point Linear sums independent weights and alpha without normalization", "[gradient_points_n]") {
	auto d = Fixture(2);
	Point(d, {.5, .5}, {255, 0, 0, 128}, 2);
	Point(d, {1.5, .5}, {0, 0, 255, 64}, 2);
	BytePixel(Run(d), 0, 0, {255, 0, 128, 255});
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Pixel(Run(d), 0, 0, {1, 0, .5, 1 + 160 / 255.});
}
TEST_CASE(
	"N-point negative Linear influence preserves unbounded source float "
	"weights",
	"[gradient_points_n]"
) {
	auto d = Fixture(2);
	Point(d, {1.5, .5}, {255, 0, 0, 255}, -2);
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Pixel(Run(d), 0, 0, {1.5, 0, 0, 2.5});
}
TEST_CASE(
	"N-point raw fractional dimensions and point Reference units survive "
	"persistence",
	"[gradient_points_n]"
) {
	auto d = Fixture(2);
	Set(d.Nodes[0], "dimension", Vector2{1.5, 2.5});
	Point(d, {.5, .5}, {200, 100, 50, 128}, 1, 1);
	auto reference = Run(d);
	REQUIRE(reference.Width == 2);
	REQUIRE(reference.Height == 2);
	d.Nodes[0].DynamicInputs[0].Default = Vector2{.75, 1.25};
	d.Nodes[0].DynamicInputs[3].Default = EnumValue{0};
	CHECK(Run(d) == reference);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(Run(restored) == reference);
}
TEST_CASE("N-point color keys seek without retaining later observations", "[gradient_points_n]") {
	auto d = Fixture();
	Point(d, {0, 0}, {255, 0, 0, 255});
	d.Keyframes = {
		{"field", "color_i_0", 0, Colour{255, 0, 0, 255}, "linear"},
		{"field", "color_i_0", 10, Colour{0, 0, 255, 255}, "linear"}
	};
	EvaluationRequest req;
	auto first = Run(d, req);
	req.Tick = 10;
	BytePixel(Run(d, req), 0, 0, {0, 0, 255, 255});
	req.Tick = 5;
	// Native timeline rounds the signed channel delta before adding the starting byte.
	BytePixel(Run(d, req), 0, 0, {127, 0, 128, 255});
	req.Tick = 0;
	CHECK(Run(d, req) == first);
}
TEST_CASE("N-point point-array rows retain source geometry independently", "[gradient_points_n]") {
	auto d = Fixture(2);
	Point(d, {0, 0}, {255, 0, 0, 255}, 1);
	ArrayValue positions;
	positions.ElementType = ValueType::Vector2;
	positions.Elements = {Vector2{.5, .5}, Vector2{1.5, .5}};
	d.Nodes[0].DynamicInputs[0].Default = positions;
	auto plan = Compiled(d);
	ImageArray result;
	Diagnostic diag;
	const auto status = EvaluateArray(d, plan, "image", {}, result, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 2);
	BytePixel(result.Images[0], 0, 0, {255, 0, 0, 255});
	BytePixel(result.Images[1], 0, 0, {0, 0, 0, 255});
	Document restored;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	auto restoredPlan = Compiled(restored);
	ImageArray restoredImages;
	REQUIRE(EvaluateArray(restored, restoredPlan, "image", {}, restoredImages, diag) == Status::Ok);
	CHECK(restoredImages.Images == result.Images);
	CHECK(restoredImages.Items == result.Items);
}
TEST_CASE(
	"N-point all 64 shader slots compile and persist while a 65th point exceeds the authored limit",
	"[gradient_points_n]"
) {
	auto d = Fixture();
	for (size_t i = 0; i < 64; ++i)
		Point(d, {0, 0}, {8, 0, 0, 0}, 0);
	REQUIRE(d.Nodes[0].DynamicInputs.size() == 64 * 4);
	CHECK(MaximumDynamicInputsForType("pc.gradient_points_n") == 64 * 4);
	CHECK(MaximumDynamicInputsForType("pc.gradient_points") == Limits::MaximumDynamicInputsPerNode);
	const auto image = Run(d);
	BytePixel(image, 0, 0, {64, 0, 0, 255});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(Run(restored) == image);
	Point(d, {0, 0}, {8, 0, 0, 0}, 0);
	Plan rejected;
	CHECK(Compile(d, rejected, diag) == Status::LimitExceeded);
	CHECK(diag.NodeId == "field");
	CHECK(diag.Port.empty());
	CHECK(diag.Message == "node exceeds the dynamic input limit");
}
TEST_CASE("N-point source undefined divisions and finite overflow refuse atomically", "[gradient_points_n]") {
	auto d = Fixture();
	Point(d, {.5, .5}, {255, 0, 0, 255});
	Refused(d, Status::UnsupportedExecution, "point_i");
	for (int mode = 1; mode <= 2; ++mode) {
		Set(d.Nodes[0], "blend_mode", EnumValue{mode});
		d.Nodes[0].DynamicInputs[2].Default = 0.;
		Refused(d, Status::UnsupportedExecution, "influence_i_0");
	}
	Set(d.Nodes[0], "blend_mode", EnumValue{0});
	d.Nodes[0].DynamicInputs[0].Default = Vector2{0, 0};
	d.Nodes[0].DynamicInputs[2].Default = -1e6;
	Refused(d, Status::UnsupportedExecution, "influence_i");
}
TEST_CASE(
	"N-point oversized second dimension row is refused before pixels "
	"replace output",
	"[gradient_points_n]"
) {
	auto d = Fixture();
	for (size_t i = 0; i < 64; ++i)
		Point(d, {0, 0}, {8, 0, 0, 0}, 0);
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Vector2;
	dimensions.Elements = {Vector2{1, 1}, Vector2{513, 513}};
	Set(d.Nodes[0], "dimension", dimensions);
	EvaluationRequest req;
	const uint64_t budget = 512 * 1024;
	Refused(d, Status::LimitExceeded, "dimension", req, budget);
	auto plan = Compiled(d);
	Image out{1, 1, {1, 2, 3, 4}, 0};
	auto prior = out;
	Diagnostic diag;
	CHECK(Evaluate(d, plan, "image", req, out, diag, budget) == Status::LimitExceeded);
	CHECK(diag.Message.find("entire processor batch") != std::string::npos);
	CHECK(out == prior);
}
TEST_CASE(
	"N-point resolved format and byte budget refuse without replacing "
	"caller pixels",
	"[gradient_points_n]"
) {
	auto d = Fixture();
	Point(d, {0, 0}, {255, 255, 255, 255});
	// This root graph resolves Inherited from the document's concrete group depth.
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{1});
	const auto inherited = Run(d);
	CHECK(inherited.Format == SurfaceFormat::RGBA8Unorm);
	BytePixel(inherited, 0, 0, {255, 255, 255, 255});
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{3});
	CHECK(Run(d) == inherited);
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Set(d.Nodes[0], "dimension", Vector2{256, 256});
	EvaluationRequest req;
	const uint64_t budget = 64 * 1024;
	Refused(d, Status::LimitExceeded, "surface_out", req, budget);
}
