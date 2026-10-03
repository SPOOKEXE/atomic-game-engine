#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>
TEST_SUITE_ID("engine.imagegraph.source_herringbone_honeycomb")
using namespace engine::imagegraph;
namespace {
	Document Graph(std::string type) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"pattern",
			 std::move(type),
			 "",
			 {},
			 {{"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"position_unit", EnumValue{0}},
			  {"position", Vector2{0, 0}},
			  {"seed", 17.0},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		if (d.Nodes[0].Type == "pc.herringbone_tile") {
			d.Nodes[0].Values.push_back({"scale_unit", EnumValue{0}});
			d.Nodes[0].Values.push_back({"scale", Vector2{4, 4}});
			d.Nodes[0].Values.push_back({"tile_length", 1.0});
			d.Nodes[0].Values.push_back({"render_type", EnumValue{1}});
		}
		d.Outputs = {{"out", "pattern", "surface_out"}};
		return d;
	}
	void Set(Document &d, std::string port, Value value) {
		for (auto &v : d.Nodes[0].Values)
			if (v.Port == port) {
				v.Data = std::move(value);
				return;
			}
		d.Nodes[0].Values.push_back({std::move(port), std::move(value)});
	}
	Image Draw(const Document &d, EvaluationRequest request = {}) {
		Plan p;
		Diagnostic diagnostic;
		auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image result;
		status = Evaluate(d, p, "out", request, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
	void Replay(const Document &d) {
		Diagnostic diagnostic;
		Document copy;
		REQUIRE(Read(Write(d), copy, diagnostic) == Status::Ok);
		CHECK(Draw(copy) == Draw(d));
	}
	Node Solid(std::string id, Colour colour) {
		return {
			std::move(id),
			"image.solid",
			"",
			{},
			{{"width", int64_t(1)}, {"height", int64_t(1)}, {"colour", colour}}
		};
	}
}
TEST_CASE("Herringbone height retains the exact unit-length cell ridge", "[source_patterns]") {
	const auto image = Draw(Graph("pc.herringbone_tile"));
	for (size_t y = 0; y < 4; ++y)
		for (size_t x = 0; x < 4; ++x) {
			const uint8_t expected = (x == 1 || x == 2) && (y == 1 || y == 2) ? 191 : 64;
			for (size_t c = 0; c < 3; ++c)
				CHECK(image.Pixels[(y * 4 + x) * 4 + c] == expected);
			CHECK(image.Pixels[(y * 4 + x) * 4 + 3] == 255);
		}
}
TEST_CASE("Honeycomb zero octaves retains source loop behavior and levels", "[source_patterns]") {
	auto d = Graph("pc.honeycomb_noise");
	Set(d, "iteration", int64_t(0));
	Set(d, "level_out", Vector2{.25, .75});
	const auto image = Draw(d);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 64);
		CHECK(image.Pixels[i + 3] == 255);
	}
	Replay(d);
}
TEST_CASE("Honeycomb degenerate transform retains the pinned lattice hash", "[source_patterns]") {
	auto d = Graph("pc.honeycomb_noise");
	Set(d, "scale", Vector2{0, 0});
	const auto image = Draw(d);
	for (size_t i = 0; i < image.Pixels.size(); i += 4)
		CHECK(image.Pixels[i] == 58);
	Set(d, "mode", EnumValue{1});
	CHECK(Draw(d) == image);
}
TEST_CASE("Pattern outputs retain all nine authored native depth choices", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"})
		for (int64_t depth = 0; depth <= 8; ++depth) {
			auto d = Graph(type);
			Set(d, "attribute_color_depth", EnumValue{depth});
			const auto image = Draw(d);
			CHECK(image.Width == 4);
			CHECK(image.Height == 4);
			CHECK(DescribeSurfaceFormat(image.Format).has_value());
			Replay(d);
		}
}
TEST_CASE("Pattern UV alpha is sampled before source mask pass", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		auto d = Graph(type);
		d.Nodes.push_back(Solid("uv", {128, 128, 128, 128}));
		d.Links = {{"uv", "image", "pattern", "uv_map"}};
		const auto image = Draw(d);
		for (size_t i = 3; i < image.Pixels.size(); i += 4)
			CHECK(image.Pixels[i] == 128);
		d.Nodes.push_back(Solid("mask", {128, 128, 128, 128}));
		d.Links.push_back({"mask", "image", "pattern", "mask"});
		const auto masked = Draw(d);
		for (size_t i = 3; i < masked.Pixels.size(); i += 4)
			CHECK(masked.Pixels[i] == 32);
	}
}
TEST_CASE("Honeycomb physical position equals source reference units", "[source_patterns]") {
	auto d = Graph("pc.honeycomb_noise");
	Set(d, "position", Vector2{1, 2});
	const auto physical = Draw(d);
	Set(d, "position_unit", EnumValue{1});
	Set(d, "position", Vector2{.25, .5});
	CHECK(Draw(d) == physical);
	Set(d, "mode", EnumValue{1});
	CHECK(Draw(d) != physical);
	Replay(d);
}
TEST_CASE("Herringbone all render modes preserve source gap and texture transforms", "[source_patterns]") {
	auto d = Graph("pc.herringbone_tile");
	Set(d, "render_type", EnumValue{0});
	Set(d, "tile_color", Gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 255, 0, 255}}}});
	const auto colored = Draw(d);
	Set(d, "render_type", EnumValue{2});
	d.Nodes.push_back(Solid("texture", {0, 0, 255, 255}));
	d.Links = {{"texture", "image", "pattern", "texture"}};
	const auto texture = Draw(d);
	CHECK(texture != colored);
	Set(d, "truchet", true);
	Set(d, "texture_seed", int64_t(19));
	Set(d, "random_position", Vector4{-.5, -.5, .5, .5});
	Replay(d);
}
TEST_CASE("Herringbone mapped controls retain physical endpoint priority", "[source_patterns]") {
	for (const auto port : {"angle", "gap", "scale"}) {
		auto d = Graph("pc.herringbone_tile");
		Set(d, "render_type", EnumValue{0});
		Set(d, std::string(port) + "_mapped", true);
		const Vector2 range = std::string_view(port) == "angle" ? Vector2{0, 90}
							  : std::string_view(port) == "gap" ? Vector2{.25, .75}
																: Vector2{2, 4};
		Set(d, std::string(port) + "_map_range", range);
		d.Nodes.push_back(Solid("map", {255, 255, 255, 0}));
		d.Links = {{"map", "image", "pattern", std::string(port) + "_map"}};
		// Remove the explicit physical default so the synthetic field represents that source slot.
		auto &values = d.Nodes[0].Values;
		std::erase_if(values, [&](const auto &v) { return v.Port == port; });
		auto expected = Graph("pc.herringbone_tile");
		Set(expected, "render_type", EnumValue{0});
		Set(expected,
			port,
			std::string_view(port) == "scale" ? Value{Vector2{range.Y, range.Y}} : Value{range.Y});
		CHECK(Draw(d) == Draw(expected));
		Replay(d);
	}
}
TEST_CASE("Honeycomb octave processor rows persist independently", "[source_patterns]") {
	auto d = Graph("pc.honeycomb_noise");
	ArrayValue iterations;
	iterations.ElementType = ValueType::Integer;
	iterations.Elements = {int64_t(1), int64_t(2)};
	Set(d, "iteration", iterations);
	Plan p;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
	ImageArray rows;
	const auto status = EvaluateArray(d, p, "out", {}, rows, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	for (size_t row = 0; row < 2; ++row) {
		auto scalar = Graph("pc.honeycomb_noise");
		Set(scalar, "iteration", int64_t(row + 1));
		CHECK(rows.Images[row] == Draw(scalar));
	}
}
TEST_CASE("Pattern invalid source divisions and work admission preserve caller output", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		auto d = Graph(type);
		Set(d, "level_in", Vector2{1, 1});
		Plan p;
		Diagnostic diagnostic;
		REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
		Image sentinel{1, 1, {8, 9, 10, 255}};
		const auto old = sentinel;
		CHECK(Evaluate(d, p, "out", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
		CHECK(sentinel == old);
		Set(d, "level_in", Vector2{0, 1});
		Set(d, "dimension", Vector2{4096, 4096});
		REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
		CHECK(Evaluate(d, p, "out", {}, sentinel, diagnostic) == Status::LimitExceeded);
		CHECK(sentinel == old);
	}
}

TEST_CASE("Pattern instances and fractional clocks use prepared source controls", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		auto d = Graph(type);
		const std::string port = std::string_view(type) == "pc.herringbone_tile" ? "angle" : "rotation";
		d.Keyframes = {{"pattern", port, 0, 0.0, "linear"}, {"pattern", port, 2, 90.0, "linear"}};
		d.Tracks = {{"pattern", port}};
		d.Nodes[0].SourceAnimatedInputs = {port};
		EvaluationRequest request;
		request.Tick = 1;
		request.Subframe = .5;
		auto expected = Graph(type);
		Set(expected, port, 67.5);
		CHECK(Draw(d, request) == Draw(expected));
		Node copy{"copy", type, "", {}, {}};
		copy.InstanceBase = "pattern";
		d.Nodes.push_back(std::move(copy));
		d.Outputs[0].NodeId = "copy";
		CHECK(Draw(d, request) == Draw(expected));
	}
}
TEST_CASE("Pattern original batch extrema refuse before early-row allocation", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		auto d = Graph(type);
		ArrayValue dimensions;
		dimensions.ElementType = ValueType::Vector2;
		dimensions.Elements = {Vector2{1, 1}, Vector2{4096, 4096}};
		Set(d, "dimension", dimensions);
		Plan p;
		Diagnostic diagnostic;
		REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
		ImageArray rows;
		rows.Images.push_back(Image{1, 1, {1, 2, 3, 255}});
		const auto prior = rows.Images;
		EvaluationRequest request;

		CHECK(EvaluateArray(d, p, "out", request, rows, diagnostic) == Status::LimitExceeded);
		CHECK(rows.Images == prior);
		CHECK(rows.Items.empty());
		CHECK(diagnostic.NodeId == "pattern");
	}
}

TEST_CASE(
	"Herringbone Shift is inert because the wrapper uploads its Surface map slot", "[source_patterns]"
) {
	auto d = Graph("pc.herringbone_tile");
	Set(d, "render_type", EnumValue{0});
	Set(d, "tile_color", Gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}});
	const auto first = Draw(d);
	Set(d, "shift", .375);
	CHECK(Draw(d) == first);
}
TEST_CASE(
	"Herringbone single-channel Texture replaces the pattern shader in every mode", "[source_patterns]"
) {
	for (int64_t depth = 6; depth <= 8; ++depth)
		for (int64_t mode = 0; mode <= 2; ++mode) {
			auto d = Graph("pc.herringbone_tile");
			Set(d, "render_type", EnumValue{mode});
			Set(d, "scale", Vector2{0, 0});
			Set(d, "level_in", Vector2{1, 1});
			d.Nodes.push_back(
				{"grey",
				 "pc.solid",
				 "",
				 {},
				 {{"dimension", Vector2{1, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"color", Colour{64, 0, 0, 255}},
				  {"attribute_color_depth", EnumValue{depth}}}}
			);
			d.Links = {{"grey", "surface_out", "pattern", "texture"}};
			const auto image = Draw(d);
			for (size_t i = 0; i < image.Pixels.size(); i += 4) {
				CHECK(image.Pixels[i] == 64);
				CHECK(image.Pixels[i + 1] == 64);
				CHECK(image.Pixels[i + 2] == 64);
				CHECK(image.Pixels[i + 3] == 255);
			}
		}
}
TEST_CASE("Herringbone Gradient Map preserves filtered colour and source alpha", "[source_patterns]") {
	auto d = Graph("pc.herringbone_tile");
	Set(d, "render_type", EnumValue{0});
	Set(d, "tile_color_mapped", true);
	Set(d, "tile_color_map_range", Vector4{0, 0, 1, 0});
	d.Nodes.push_back(Solid("map", {255, 0, 0, 128}));
	d.Links = {{"map", "image", "pattern", "tile_color_map"}};
	const auto image = Draw(d);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 255);
		CHECK(image.Pixels[i + 1] == 0);
		CHECK(image.Pixels[i + 3] == 128);
	}
	Replay(d);
}

TEST_CASE("Pattern Surface tuple getters retain dimensions before reference units", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		for (const auto port : {"dimension", "position", "scale"}) {
			auto linked = Graph(type);
			Node image = Solid("control", {255, 255, 255, 255});
			image.Values[0].Data = int64_t(3);
			image.Values[1].Data = int64_t(2);
			linked.Nodes.push_back(std::move(image));
			linked.Links = {{"control", "image", "pattern", port}};
			if (std::string_view(port) != "dimension" &&
				(std::string_view(port) != "scale" || std::string_view(type) == "pc.herringbone_tile"))
				Set(linked, std::string(port) + "_unit", EnumValue{1});
			auto explicitValue = Graph(type);
			Set(explicitValue, port, Vector2{3, 2});
			CHECK(Draw(linked) == Draw(explicitValue));
			Replay(linked);
		}
	}
}
TEST_CASE("Pattern heterogeneous Dimension rows use first Reference tuple", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"}) {
		auto relative = Graph(type);
		ArrayValue dimensions{ValueType::Vector2, {Vector2{4, 4}, Vector2{8, 2}}};
		Set(relative, "dimension", dimensions);
		Set(relative, "position", Vector2{.25, .5});
		Set(relative, "position_unit", EnumValue{1});
		if (std::string_view(type) == "pc.herringbone_tile") {
			Set(relative, "scale", Vector2{1, 1});
			Set(relative, "scale_unit", EnumValue{1});
		}
		auto physical = relative;
		Set(physical, "position", Vector2{1, 2});
		Set(physical, "position_unit", EnumValue{0});
		if (std::string_view(type) == "pc.herringbone_tile") {
			Set(physical, "scale", Vector2{4, 4});
			Set(physical, "scale_unit", EnumValue{0});
		}
		Plan p, q;
		Diagnostic diagnostic;
		ImageArray a, b;
		REQUIRE(Compile(relative, p, diagnostic) == Status::Ok);
		REQUIRE(Compile(physical, q, diagnostic) == Status::Ok);
		REQUIRE(EvaluateArray(relative, p, "out", {}, a, diagnostic) == Status::Ok);
		REQUIRE(EvaluateArray(physical, q, "out", {}, b, diagnostic) == Status::Ok);
		REQUIRE(a.Images.size() == 2);
		CHECK(a.Images == b.Images);
		CHECK(a.Images[1].Width == 8);
		CHECK(a.Images[1].Height == 2);
	}
}
TEST_CASE("Honeycomb late octave work refuses the complete original batch", "[source_patterns]") {
	auto d = Graph("pc.honeycomb_noise");
	Set(d, "dimension", Vector2{64, 64});
	d.Nodes.push_back(
		{"octaves", "pc.number", "", {}, {{"value", ArrayValue{ValueType::Scalar, {1.0, 100.5}}}}}
	);
	d.Links.push_back({"octaves", "number", "pattern", "iteration"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(d, plan, "pattern", {}, snapshot, diagnostic) == Status::Ok);
	const auto input =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "iteration";
		});
	REQUIRE(input != snapshot.Values().end());
	REQUIRE(input->Linked);
	const auto *iteration = std::get_if<ArrayValue>(&input->Data);
	REQUIRE(iteration);
	CHECK(iteration->ElementType == ValueType::Integer);
	CHECK(iteration->Elements == std::vector<ElementValue>{int64_t(1), int64_t(100)});
	ImageArray result;
	result.Images.push_back(Image{1, 1, {1, 2, 3, 255}});
	const auto before = result.Images;
	CHECK(EvaluateArray(d, plan, "out", {}, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.Images == before);
	CHECK(result.Items.empty());
	CHECK(diagnostic.NodeId == "pattern");
}
TEST_CASE("Pattern whole Surface arrays resolve nonsurface tuple dimensions", "[source_patterns]") {
	for (const auto type : {"pc.herringbone_tile", "pc.honeycomb_noise"})
		for (const auto port : {"position", "scale"}) {
			auto linked = Graph(type);
			Node image = Solid("control", {255, 128, 64, 255});
			image.Values[0].Data = int64_t(3);
			image.Values[1].Data = int64_t(2);
			linked.Nodes.push_back(std::move(image));
			linked.Nodes.push_back({"channels", "pc.rgb_channel", "", {}, {{"output_array", true}}});
			linked.Links = {
				{"control", "image", "channels", "surface_in"}, {"channels", "red", "pattern", port}
			};
			if (std::string_view(port) != "scale" || std::string_view(type) == "pc.herringbone_tile")
				Set(linked, std::string(port) + "_unit", EnumValue{1});
			auto expected = Graph(type);
			Set(expected, port, Vector2{1, 1});
			CHECK(Draw(linked) == Draw(expected));
			Replay(linked);
		}
}
