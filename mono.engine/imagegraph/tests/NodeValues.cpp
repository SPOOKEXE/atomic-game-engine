// Fixtures for value and project-level catalogue executors.

#include "NodeHarness.hpp"
#include "../src/ValueNodeEval.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <numbers>
#include <tuple>

TEST_SUITE_ID("engine.imagegraph.node_values")

using namespace engine::imagegraph;
using imagegraph_test::MakeImage;
using imagegraph_test::RunNode;

TEST_CASE("Number passes its value and rounds halves to even when Integer", "[imagegraph]") {
	const auto raw = RunNode("pc.number", {}, {{"value", 2.5}});
	REQUIRE(raw.Ok);
	CHECK(*raw.OutputValue("number") == Value{2.5});
	for (const auto &[input, expected] : {std::pair{2.5, 2.0}, std::pair{3.5, 4.0}, std::pair{-1.2, -1.0}}) {
		const auto rounded = RunNode("pc.number", {}, {{"value", input}, {"integer", true}});
		REQUIRE(rounded.Ok);
		CHECK(*rounded.OutputValue("number") == Value{expected});
	}
}

TEST_CASE("Number drives a catalogue filter through a scalar link", "[imagegraph]") {
	Document document;
	document.FormatVersion = 7;
	document.Nodes.push_back(
		{"fill",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{20, 20, 20, 255}}}}
	);
	document.Nodes.push_back({"shift", "pc.number", "", {}, {{"value", 0.5}}});
	document.Nodes.push_back({"bw", "pc.bw", "", {}, {}});
	document.Nodes.push_back({"out", "pc.project_output", "", {}, {}});
	document.Links.push_back({"fill", "image", "bw", "surface_in"});
	document.Links.push_back({"shift", "number", "bw", "brightness"});
	document.Links.push_back({"bw", "surface_out", "out", "surface_in"});
	document.Outputs.push_back({"final", "out", "surface_out"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "final", image, diagnostic) == Status::Ok);
	// 20/255 + 0.5 = 0.578 is above the 0.5 threshold only because the linked brightness applies.
	CHECK(image.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}

TEST_CASE("String nodes follow their GML functions", "[imagegraph]") {
	const auto text = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		REQUIRE(run.Ok);
		return std::get<std::string>(*run.OutputValue(port));
	};
	CHECK(
		text(
			RunNode(
				"pc.string_get_char",
				{},
				{{"text", std::string("héllo")}, {"index", int64_t{2}}, {"amount", int64_t{3}}}
			),
			"text"
		) == "éll"
	);
	// Delete's index is zero-based in the node and one-based in string_delete.
	CHECK(
		text(
			RunNode(
				"pc.string_delete",
				{},
				{{"text", std::string("abcdef")}, {"index", int64_t{1}}, {"amount", int64_t{2}}}
			),
			"text"
		) == "adef"
	);
	CHECK(
		text(
			RunNode(
				"pc.string_change_case", {}, {{"text", std::string("big red dog")}, {"type", EnumValue{2}}}
			),
			"text"
		) == "Big Red Dog"
	);
	CHECK(
		text(
			RunNode(
				"pc.string_replace",
				{},
				{{"text", std::string("a-b-c")},
				 {"find", std::string("-")},
				 {"replace", std::string("+")},
				 {"all", false}}
			),
			"results"
		) == "a+b-c"
	);
	CHECK(text(RunNode("pc.unicode", {}, {{"unicode", int64_t{0x263A}}}), "character") == "\xE2\x98\xBA");
	CHECK(text(RunNode("pc.to_text", {}, {{"value", 1.5}}), "text") == "1.50");
	CHECK(text(RunNode("pc.to_text", {}, {{"value", 3.0}}), "text") == "3");
	const auto count =
		RunNode("pc.string_count", {}, {{"text", std::string("aaaa")}, {"count_text", std::string("aa")}});
	REQUIRE(count.Ok);
	CHECK(*count.OutputValue("amount") == Value{int64_t{2}});
	const auto words =
		RunNode("pc.string_length", {}, {{"text", std::string("a  b")}, {"mode", EnumValue{1}}});
	REQUIRE(words.Ok);
	CHECK(*words.OutputValue("length") == Value{int64_t{3}});
}

TEST_CASE("To Number keeps digits and applies the exponent", "[imagegraph]") {
	const auto number = [](std::string input) {
		const auto run = RunNode("pc.to_number", {}, {{"text", input}});
		REQUIRE(run.Ok);
		return std::get<double>(*run.OutputValue("number"));
	};
	CHECK(number("-12.5px") == -12.5);
	CHECK(number("3e2") == 300.0);
	CHECK(number("abc") == 0.0);
	CHECK(number("1.2.3") == 1.23);
}

TEST_CASE("Vector nodes follow their GML formulas", "[imagegraph]") {
	const auto value = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		REQUIRE(run.Ok);
		return *run.OutputValue(port);
	};
	CHECK(
		value(
			RunNode("pc.vector_cross_2_d", {}, {{"point_1", Vector2{2, 3}}, {"point_2", Vector2{5, 7}}}),
			"result"
		) == Value{-1.0}
	);
	CHECK(
		value(
			RunNode(
				"pc.vector_cross_3_d", {}, {{"point_1", Vector3{1, 0, 0}}, {"point_2", Vector3{0, 1, 0}}}
			),
			"result"
		) == Value{Vector3{0, 0, 1}}
	);
	// point_direction is counterclockwise on a Y-down canvas: (0, -1) points up at 90 degrees.
	CHECK(
		value(RunNode("pc.vector_direction", {}, {{"vector", Vector2{0, -1}}}), "direction") == Value{90.0}
	);
	CHECK(value(RunNode("pc.vector_magnitude", {}, {{"vector", Vector2{3, 4}}}), "magnitude") == Value{5.0});
	CHECK(
		value(
			RunNode("pc.vector_dot", {}, {{"point_1", Vector2{1, 2}}, {"point_2", Vector2{3, 4}}}), "result"
		) == Value{11.0}
	);
	const Value swizzled = value(RunNode("pc.vector_swizzle", {}, {{"vector", Vector3{1, 2, 3}}}), "result");
	CHECK(std::get<ArrayValue>(swizzled).Elements == std::vector<ElementValue>{2.0, 1.0, 3.0});
	const Value cart =
		value(RunNode("pc.vector_polar_to_cart", {}, {{"polar_coord", Vector2{2, 90}}}), "cartesian_coord");
	CHECK(std::abs(std::get<Vector2>(cart).X) < 1e-12);
	CHECK(std::get<Vector2>(cart).Y == -2.0);
	CHECK(
		value(RunNode("pc.vector3", {}, {{"x", 1.5}, {"y", 2.5}, {"integer", true}}), "vector") ==
		Value{Vector3{2, 2, 0}}
	);
}

TEST_CASE("Colour nodes split and mix channels", "[imagegraph]") {
	const auto rgb = RunNode("pc.color_to_rgb", {}, {{"color", Colour{255, 51, 0, 255}}});
	REQUIRE(rgb.Ok);
	CHECK(*rgb.OutputValue("green") == Value{0.2});
	const auto hsv = RunNode("pc.color_to_hsv", {}, {{"color", Colour{0, 255, 0, 255}}});
	REQUIRE(hsv.Ok);
	CHECK(*hsv.OutputValue("hue") == Value{1.0 / 3.0});
	CHECK(*hsv.OutputValue("value") == Value{1.0});
	const auto mixed = RunNode(
		"pc.color_mix", {}, {{"color_from", Colour{0, 0, 0, 255}}, {"color_to", Colour{200, 100, 50, 255}}}
	);
	REQUIRE(mixed.Ok);
	CHECK(*mixed.OutputValue("color") == Value{Colour{100, 50, 25, 255}});
}

TEST_CASE("Matrix nodes follow matrix_functions.gml", "[imagegraph]") {
	const MatrixValue square{3, 3, {2, 0, 1, 1, 3, 2, 1, 1, 1}};
	const auto det = RunNode("pc.matrix_det", {}, {{"matrix", square}});
	REQUIRE(det.Ok);
	// 2 * (3 - 2) - 0 + 1 * (1 - 3) = 0.
	CHECK(*det.OutputValue("determinant") == Value{0.0});
	const auto singular = RunNode("pc.matrix_invert", {}, {{"matrix", square}});
	REQUIRE(singular.Ok);
	CHECK(*singular.OutputValue("matrix") == Value{square});
	const auto inverse = RunNode("pc.matrix_invert", {}, {{"matrix", MatrixValue{2, 2, {4, 7, 2, 6}}}});
	REQUIRE(inverse.Ok);
	CHECK(*inverse.OutputValue("matrix") == Value{MatrixValue{2, 2, {0.6, -0.7, -0.2, 0.4}}});
	const auto transposed =
		RunNode("pc.matrix_transpose", {}, {{"matrix", MatrixValue{3, 2, {1, 2, 3, 4, 5, 6}}}});
	REQUIRE(transposed.Ok);
	CHECK(*transposed.OutputValue("matrix") == Value{MatrixValue{2, 3, {1, 4, 2, 5, 3, 6}}});
	// The vector has two components, so the third column multiplies by 1.
	const auto product = RunNode(
		"pc.matrix_multiply_vector",
		{},
		{{"matrix", MatrixValue{3, 2, {1, 2, 3, 4, 5, 6}}}, {"vector", Vector2{1, 1}}}
	);
	REQUIRE(product.Ok);
	CHECK(
		std::get<ArrayValue>(*product.OutputValue("vector")).Elements == std::vector<ElementValue>{6.0, 15.0}
	);
	const auto identity = RunNode("pc.matrix_identity", {}, {{"size", int64_t{2}}});
	REQUIRE(identity.Ok);
	CHECK(*identity.OutputValue("matrix") == Value{MatrixValue{2, 2, {1, 0, 0, 1}}});
}

TEST_CASE("Matrix values round trip through document text", "[imagegraph]") {
	Document document;
	document.FormatVersion = 7;
	document.Nodes.push_back(
		{"m", "pc.matrix", "", {}, {{"size", Vector2{2, 2}}, {"data", MatrixValue{2, 2, {1.5, 2, 3, 4}}}}}
	);
	document.Outputs.push_back({"matrix", "m", "matrix"});
	Document read;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), read, diagnostic) == Status::Ok);
	CHECK(read == document);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "matrix", {0, 0}, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{MatrixValue{2, 2, {1.5, 2, 3, 4}}});
}

TEST_CASE("Data, point and quaternion nodes follow their source helpers", "[imagegraph]") {
	const auto value = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		REQUIRE(run.Ok);
		return *run.OutputValue(port);
	};
	CHECK(
		value(
			RunNode("pc.padding_data", {}, {{"left", 1.0}, {"right", 2.0}, {"top", 3.0}, {"bottom", 4.0}}),
			"padding"
		) == Value{Vector4{2, 3, 1, 4}}
	);
	CHECK(
		value(
			RunNode("pc.point_in_area", {}, {{"area", Area{0, 0, 1, 1, 0, 0}}, {"point", Vector2{1, 0}}}),
			"is_in"
		) == Value{true}
	);
	CHECK(
		value(
			RunNode(
				"pc.point_in_area",
				{},
				{{"area", Area{0, 0, 1, 1, 0, 0}}, {"point", Vector2{1, 0}}, {"include_boundary", false}}
			),
			"is_in"
		) == Value{false}
	);
	CHECK(
		value(
			RunNode(
				"pc.move_point",
				{},
				{{"point", Vector2{2, 0}}, {"anchor_point", Vector2{0, 0}}, {"rotation", 180.0}}
			),
			"result"
		) == Value{Vector2{-2, 0}}
	);
	CHECK(
		value(
			RunNode(
				"pc.base_convert",
				{},
				{{"value", std::string("ff")}, {"base_from", int64_t{16}}, {"base_to", int64_t{2}}}
			),
			"result"
		) == Value{std::string("11111111")}
	);
	const Vector4 rotation = std::get<Vector4>(
		value(RunNode("pc.quarternion_from_euler", {}, {{"euler_rotation", Vector3{0, 0, 90}}}), "rotation")
	);
	CHECK(rotation.X == 0.0);
	CHECK(std::abs(rotation.Z + std::sqrt(0.5)) < 1e-15);
	CHECK(std::abs(rotation.W - std::sqrt(0.5)) < 1e-15);
	CHECK(
		value(
			RunNode(
				"pc.quarternion_to_euler",
				{},
				{{"rotation", Quaternion{0, 0, -std::sqrt(0.5), std::sqrt(0.5)}}}
			),
			"euler_angles"
		) == Value{Vector3{0, 0, -90}}
	);
	const Image blank = MakeImage(1, 1, {0, 0, 0, 0}), dot = MakeImage(1, 1, {0, 0, 0, 1});
	CHECK(value(RunNode("pc.surface_is_empty", {{"surface_in", &blank}}), "is_empty") == Value{true});
	CHECK(value(RunNode("pc.surface_is_empty", {{"surface_in", &dot}}), "is_empty") == Value{false});
	CHECK(
		value(RunNode("pc.surface_data", {{"surface", &dot}}), "format_string") ==
		Value{std::string("8bit RGBA")}
	);
}

TEST_CASE("Pass-through and packing value nodes", "[imagegraph]") {
	const auto value = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		REQUIRE(run.Ok);
		return *run.OutputValue(port);
	};
	CHECK(
		value(RunNode("pc.corner_data", {}, {{"top_left", 1.0}, {"bottom_right", 4.0}}), "corner") ==
		Value{Vector4{1, 0, 0, 4}}
	);
	CHECK(
		value(RunNode("pc.rotation_range_data", {}, {{"start", 10.0}}), "rotation_range") ==
		Value{Vector2{10, 360}}
	);
	CHECK(
		value(RunNode("pc.color", {}, {{"color", Colour{1, 2, 3, 4}}}), "color") == Value{Colour{1, 2, 3, 4}}
	);
	CHECK(value(RunNode("pc.string", {}, {{"text", std::string("hi")}}), "text") == Value{std::string("hi")});
	CHECK(
		value(RunNode("pc.matrix_to_array", {}, {{"matrix", MatrixValue{2, 1, {5, 6}}}}), "array") ==
		Value{ArrayValue{ValueType::Scalar, {5.0, 6.0}}}
	);
	CHECK(
		value(RunNode("pc.vector_cart_to_polar", {}, {{"cartesian_coord", Vector2{0, 2}}}), "polar_coord") ==
		Value{Vector2{2, 90}}
	);
	CHECK(
		value(RunNode("pc.vector_normalize", {}, {{"vector", Vector2{3, 4}}}), "normalized_vector") ==
		Value{ArrayValue{ValueType::Scalar, {0.6, 0.8}}}
	);
	const auto split = RunNode("pc.vector_split", {}, {{"vector", Vector4{1, 2, 3, 4}}});
	REQUIRE(split.Ok);
	CHECK(*split.OutputValue("z") == Value{3.0});
	// A straight 10 pixel segment measures 10 at the source path resolution.
	Path2D line;
	line.Anchors = {PathAnchor{{0, 0, 0, 0, 0, 0}, 0}, PathAnchor{{10, 0, 0, 0, 0, 0}, 0}};
	CHECK(value(RunNode("pc.path_length", {}, {{"path", line}}), "surface_out") == Value{10.0});
}

TEST_CASE("String Merge joins dynamic inputs in group order", "[imagegraph]") {
	Document document;
	document.FormatVersion = 7;
	document.Nodes.push_back(
		{"merge",
		 "pc.string_merge",
		 "",
		 {},
		 {},
		 {{"text_1", ValueType::Text, Value{std::string("b")}},
		  {"text_0", ValueType::Text, Value{std::string("a")}}}}
	);
	document.Outputs.push_back({"text", "merge", "text"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "text", {0, 0}, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{std::string("ab")});
}

TEST_CASE("Stateless source Number Simple preserves typed numeric payloads", "[imagegraph][node_math]") {
	for (const Value &value :
		 {Value{2.5},
		  Value{int64_t{-3}},
		  Value{true},
		  Value{Vector2{2, 3}},
		  Value{ArrayValue{ValueType::Integer, {int64_t{2}, int64_t{3}}}}}) {
		const auto run = RunNode("pc.number_simple", {}, {{"value", value}});
		REQUIRE(run.Ok);
		CHECK(*run.OutputValue("number") == value);
	}
	const auto invalid =
		RunNode("pc.number_simple", {}, {{"value", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Values.empty());
}

TEST_CASE(
	"Source Compare implements six relations and documented default epsilon", "[imagegraph][node_math]"
) {
	const auto compare = [](int64_t mode, double a, double b) {
		const auto run = RunNode("pc.compare", {}, {{"type", EnumValue{mode}}, {"a", a}, {"b", b}});
		REQUIRE(run.Ok);
		return std::get<bool>(*run.OutputValue("result"));
	};
	for (int64_t mode = 0; mode < 6; ++mode) {
		CAPTURE(mode);
		CHECK(compare(mode, 2, 3) == std::array{false, true, false, false, true, true}[mode]);
		CHECK(compare(mode, 3, 2) == std::array{false, true, true, true, false, false}[mode]);
		CHECK(compare(mode, 0, .00001) == std::array{true, false, false, true, false, true}[mode]);
	}
	CHECK_FALSE(compare(0, 0, .000010001));
	const auto fractional = RunNode("pc.compare", {}, {{"type", .5}, {"a", 1.0}, {"b", 1.0}});
	REQUIRE(fractional.Ok);
	CHECK(*fractional.OutputValue("result") == Value{0.0});
}

TEST_CASE("Source Math evaluates all twenty two source modes", "[imagegraph][node_math]") {
	struct Fixture {
		int64_t Mode;
		double A;
		double B;
		double C;
		double Expected;
	};
	const Fixture fixtures[] = {{0, 6, 2, 0, 8},	   {1, 6, 2, 0, 4},		  {2, 6, 2, 0, 12},
								{3, 6, 2, 0, 3},	   {4, 4, -2, 0, .0625},  {5, 27, 3, 0, 3},
								{6, 30, 4, 0, 2},	   {7, 60, 4, 0, 2},	  {8, 45, 3, 0, 3},
								{9, -7, 3, 0, -1},	   {10, -2.25, 0, 0, -3}, {11, -2.25, 0, 0, -2},
								{12, 2.5, 0, 0, 2},	   {13, 2, 6, 1.5, 8},	  {14, -3, 0, 0, 3},
								{15, 8, 2, 5, 5},	   {16, 5, 2, 0, 4},	  {17, -2.75, 0, 0, -.75},
								{18, .25, 0, 0, 12.5}, {19, 8, 2, 0, 3},	  {20, 2, 6, 0, 6},
								{21, 2, 6, 0, 2}};
	for (const auto &f : fixtures) {
		CAPTURE(f.Mode);
		const auto run = RunNode(
			"pc.math",
			{},
			{{"type", EnumValue{f.Mode}},
			 {"a", f.A},
			 {"b", f.B},
			 {"amount", f.C},
			 {"from", Vector2{0, 1}},
			 {"to", Vector2{10, 20}}}
		);
		REQUIRE(run.Ok);
		CHECK(std::abs(std::get<double>(*run.OutputValue("result")) - f.Expected) < 1e-10);
	}
	const auto radians = RunNode(
		"pc.math",
		{},
		{{"type", EnumValue{6}}, {"angle", EnumValue{0}}, {"a", std::numbers::pi / 2}, {"b", 2.0}}
	);
	REQUIRE(radians.Ok);
	CHECK(std::get<double>(*radians.OutputValue("result")) == 2.0);
	const auto fractional = RunNode("pc.math", {}, {{"type", .5}, {"a", 4.0}, {"b", 2.0}});
	REQUIRE(fractional.Ok);
	CHECK(*fractional.OutputValue("result") == Value{0.0});
}

TEST_CASE(
	"Source Math handles zero branches and rejects undefined finite domains atomically",
	"[imagegraph][node_math]"
) {
	for (const auto &[mode, a, b, expected] :
		 {std::tuple{3, 4.0, 0.0, 0.0},
		  {3, 4.0, .000005, 0.0},
		  {3, 4.0, .00002, 200000.0},
		  {5, 4.0, -2.0, 0.0},
		  {5, 4.0, .000005, 0.0},
		  {9, 4.0, 0.0, 0.0},
		  {16, 2.5, 0.0, 2.5},
		  {16, -5.0, 2.0, -4.0},
		  {12, -3.5, 0.0, -4.0}}) {
		const auto run = RunNode("pc.math", {}, {{"type", EnumValue{mode}}, {"a", a}, {"b", b}});
		REQUIRE(run.Ok);
		CHECK(std::abs(std::get<double>(*run.OutputValue("result")) - expected) < 1e-8);
	}
	for (const auto &[mode, a, b] :
		 {std::tuple{4, -1.0, .5}, {4, 0.0, -1.0}, {5, -1.0, 2.0}, {19, -1.0, 2.0}, {19, 2.0, 1.0}}) {
		const auto run = RunNode("pc.math", {}, {{"type", EnumValue{mode}}, {"a", a}, {"b", b}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Values.empty());
	}
	const auto equalRange =
		RunNode("pc.math", {}, {{"type", EnumValue{18}}, {"a", 2.0}, {"from", Vector2{1, 1}}});
	CHECK_FALSE(equalRange.Ok);
	CHECK(equalRange.Code == Status::InvalidValue);
	CHECK(equalRange.Values.empty());
}

TEST_CASE(
	"Compare zero padding and Math loop broadcasting retain distinct source array semantics",
	"[imagegraph][node_math]"
) {
	const ArrayValue a{ValueType::Scalar, {2.0, 3.0, 4.0}};
	const ArrayValue b{ValueType::Integer, {int64_t{2}, int64_t{3}}};
	const auto compare = RunNode("pc.compare", {}, {{"type", EnumValue{0}}, {"a", a}, {"b", b}});
	REQUIRE(compare.Ok);
	CHECK(*compare.OutputValue("result") == Value{ArrayValue{ValueType::Boolean, {true, true, false}}});
	const auto math = RunNode("pc.math", {}, {{"a", a}, {"b", b}});
	REQUIRE(math.Ok);
	CHECK(*math.OutputValue("result") == Value{ArrayValue{ValueType::Scalar, {4.0, 6.0, 6.0}}});
	const auto packed = RunNode("pc.math", {}, {{"a", Vector3{1, 2, 3}}, {"b", 2.0}});
	REQUIRE(packed.Ok);
	CHECK(*packed.OutputValue("result") == Value{ArrayValue{ValueType::Scalar, {3.0, 4.0, 5.0}}});
	const auto bools = RunNode("pc.math", {}, {{"a", *compare.OutputValue("result")}, {"b", 2.0}});
	REQUIRE(bools.Ok);
	CHECK(*bools.OutputValue("result") == Value{ArrayValue{ValueType::Scalar, {3.0, 3.0, 2.0}}});
}

TEST_CASE(
	"Source recursive arithmetic preserves empty policies and diagnoses unrepresentable mixed shapes",
	"[imagegraph][node_math]"
) {
	const ArrayValue empty{ValueType::Scalar, {}};
	const auto compareEmpty = RunNode("pc.compare", {}, {{"a", empty}, {"b", empty}});
	REQUIRE(compareEmpty.Ok);
	CHECK(*compareEmpty.OutputValue("result") == Value{0.0});
	const auto mathEmpty = RunNode("pc.math", {}, {{"a", empty}, {"b", empty}, {"amount", empty}});
	REQUIRE(mathEmpty.Ok);
	CHECK(*mathEmpty.OutputValue("result") == Value{empty});
	ArrayValue a{ValueType::Scalar, {}};
	a.Nested = {{1.0, 2.0}, {3.0}};
	ArrayValue b{ValueType::Scalar, {}};
	b.Nested = {{10.0}, {20.0, 30.0}};
	const auto nested = RunNode("pc.math", {}, {{"a", a}, {"b", b}});
	REQUIRE(nested.Ok);
	ArrayValue expected{ValueType::Scalar, {}};
	expected.Nested = {{11.0, 12.0}, {23.0, 33.0}};
	CHECK(*nested.OutputValue("result") == Value{expected});
	a.Nested = {{}, {1.0}};
	const auto mixed = RunNode("pc.compare", {}, {{"a", a}, {"b", 0.0}});
	CHECK_FALSE(mixed.Ok);
	CHECK(mixed.Code == Status::UnsupportedExecution);
	CHECK(mixed.Values.empty());
}

TEST_CASE("Source arithmetic preflights expanded aggregate output counts", "[imagegraph][node_math]") {
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested.resize(65, {1.0});
	ArrayValue wide{ValueType::Scalar, {}};
	wide.Elements.resize(65, 2.0);
	const auto run = RunNode("pc.math", {}, {{"a", rows}, {"b", wide}});
	// Every scalar selected from wide broadcasts into one row, so this remains65 leaves.
	REQUIRE(run.Ok);
	ArrayValue row{ValueType::Scalar, {}};
	row.Nested = {std::vector<ElementValue>(65, 2.0)};
	const auto oversized = RunNode("pc.math", {}, {{"a", rows}, {"b", row}});
	CHECK_FALSE(oversized.Ok);
	CHECK(oversized.Code == Status::LimitExceeded);
	CHECK(oversized.Values.empty());
	const auto wrong = RunNode("pc.math", {}, {{"a", std::string("wrong")}});
	CHECK_FALSE(wrong.Ok);
	CHECK(wrong.Code == Status::UnsupportedExecution);
}

TEST_CASE(
	"Persisted source Math real rounding feeds an Integer control without inventing payload casts",
	"[imagegraph][node_math]"
) {
	for (const auto &[input, width] : {std::pair{2.5, size_t{2}}, std::pair{3.5, size_t{4}}}) {
		Document source;
		source.FormatVersion = 8;
		source.Nodes.push_back(
			{"math",
			 "pc.math",
			 "",
			 {},
			 {{"type", EnumValue{12}}, {"a", input}, {"to_integer", true}, {"output_vector", true}}}
		);
		source.Nodes.push_back(
			{"audio",
			 "pc.audio_window",
			 "",
			 {},
			 {{"width", int64_t{1}},
			  {"step", int64_t{1}},
			  {"match_timeline", false},
			  {"cursor_location", EnumValue{0}},
			  {"location", 0.0}}}
		);
		source.Nodes.push_back({"file", "pc.wav_file_read", "", {}, {{"path", std::string("tone.wav")}}});
		source.Links.push_back({"file", "data", "audio", "audio_data"});
		source.Links.push_back({"math", "result", "audio", "width"});
		source.Outputs.push_back({"result", "audio", "bit_array"});
		Document parsed;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(source), parsed, diagnostic) == Status::Ok);
		CHECK(std::get<bool>(parsed.Nodes[0].Values[2].Data));
		Plan plan;
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		const std::array assets{AudioClipSource{"tone.wav", AudioBit{{1, 2, 3, 4, 5}, 8}}};
		EvaluationRequest request;
		request.AudioClips = assets;
		EvaluatedValue result;
		const Status status = EvaluateValue(parsed, plan, "result", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto &samples = std::get<ArrayValue>(result.Data);
		REQUIRE(samples.Nested.size() == 1);
		CHECK(samples.Nested.front().size() == width);
		CHECK(std::get<double>(samples.Nested.front().front()) == 1.0);
		const auto rounded =
			RunNode("pc.math", {}, {{"type", EnumValue{12}}, {"a", input}, {"to_integer", true}});
		REQUIRE(rounded.Ok);
		CHECK(*rounded.OutputValue("result") == Value{double(width)});
	}
}

TEST_CASE(
	"Persisted stateless source arithmetic routes homogeneous authored and linked arrays",
	"[imagegraph][node_math]"
) {
	Document source;
	source.FormatVersion = 8;
	source.Nodes = {
		{"simple",
		 "pc.number_simple",
		 "",
		 {},
		 {{"value", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}, int64_t{3}}}}}},
		{"math", "pc.math", "", {}, {{"b", ArrayValue{ValueType::Scalar, {2.0, 3.0}}}}},
		{"compare", "pc.compare", "", {}, {{"b", ArrayValue{ValueType::Scalar, {3.0, 5.0, 4.0}}}}},
		{"out", "pc.number_simple", "", {}, {}}
	};
	source.Links = {
		{"simple", "number", "math", "a"},
		{"math", "result", "compare", "a"},
		{"compare", "result", "out", "value"}
	};
	source.Outputs = {{"result", "out", "number"}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(source), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == source);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(parsed, plan, "result", EvaluationRequest{}, result, diagnostic) == Status::Ok);
	CHECK(result.Data == Value{ArrayValue{ValueType::Boolean, {true, true, false}}});
}

TEST_CASE(
	"Source arithmetic typed settings and packed components preserve payload semantics",
	"[imagegraph][node_math]"
) {
	for (const Value &value : {Value{Vector4{1, 2, 3, 4}}, Value{Quaternion{1, 2, 3, 4}}}) {
		const auto run = RunNode("pc.math", {}, {{"a", value}, {"b", 1.0}});
		REQUIRE(run.Ok);
		CHECK(*run.OutputValue("result") == Value{ArrayValue{ValueType::Scalar, {2.0, 3.0, 4.0, 5.0}}});
	}
	const auto area = RunNode("pc.math", {}, {{"a", Area{1, 2, 3, 4, 1, 2}}, {"b", 1.0}});
	REQUIRE(area.Ok);
	CHECK(
		*area.OutputValue("result") == Value{ArrayValue{ValueType::Scalar, {2.0, 3.0, 4.0, 5.0, 2.0, 3.0}}}
	);
	for (bool integer : {false, true}) {
		const auto add =
			RunNode("pc.math", {}, {{"a", 2.5}, {"to_integer", integer}, {"output_vector", true}});
		REQUIRE(add.Ok);
		CHECK(*add.OutputValue("result") == Value{2.5});
	}
	const auto angleFalse = RunNode(
		"pc.math", {}, {{"type", EnumValue{6}}, {"angle", .5}, {"a", std::numbers::pi / 2}, {"b", 1.0}}
	);
	REQUIRE(angleFalse.Ok);
	CHECK(*angleFalse.OutputValue("result") == Value{1.0});
	const auto overflow = RunNode(
		"pc.math", {}, {{"type", EnumValue{2}}, {"a", std::numeric_limits<double>::max()}, {"b", 2.0}}
	);
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Code == Status::InvalidValue);
	CHECK(overflow.Values.empty());
}

TEST_CASE(
	"A source scalar-declared array cannot silently become an Integer control zero", "[imagegraph][node_math]"
) {
	Document source;
	source.FormatVersion = 8;
	source.Nodes = {
		{"math", "pc.math", "", {}, {{"a", ArrayValue{ValueType::Scalar, {1.0, 2.0}}}}},
		{"window", "pc.audio_window", "", {}, {}},
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string("tone.wav")}}}
	};
	source.Links = {{"math", "result", "window", "width"}, {"file", "data", "window", "audio_data"}};
	source.Outputs = {{"result", "window", "bit_array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(source, plan, diagnostic) == Status::Ok);
	const std::array assets{AudioClipSource{"tone.wav", AudioBit{{1, 2, 3, 4, 5}, 8}}};
	EvaluationRequest request;
	request.AudioClips = assets;
	EvaluatedValue result;
	CHECK(EvaluateValue(source, plan, "result", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "window");
	CHECK(diagnostic.Port == "width");
}

TEST_CASE("Number Simple checks remaining publication bytes before cloning", "[imagegraph][node_math]") {
	const auto *entry = FindCatalogueEntry("pc.number_simple");
	REQUIRE(entry);
	Node node{"simple", "pc.number_simple", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.Values = {{"value", ArrayValue{ValueType::Scalar, {1.0, 2.0}}}};
	REQUIRE(detail::FindExecutor("pc.number_simple"));
	CHECK_FALSE(detail::FindExecutor("pc.number_simple")(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
}

TEST_CASE(
	"Source arithmetic checks exact Value publication bytes before materializing results",
	"[imagegraph][node_math]"
) {
	const auto run = [](std::string_view type, Value a, Value b, uint64_t bytes) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"arithmetic", std::string(type), "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = bytes;
		for (const auto &input : entry->Inputs)
			if (auto value = CatalogueDefault(input)) context.Values.push_back({input.Id, *value});
		for (auto &value : context.Values) {
			if (value.first == "a") value.second = a;
			if (value.first == "b") value.second = b;
		}
		const auto executor = detail::FindExecutor(type);
		REQUIRE(executor);
		const bool ok = executor(context);
		if (!ok) CHECK(context.FailureCode == Status::LimitExceeded);
		return std::pair{ok, std::move(context.OutputValues)};
	};
	const ArrayValue values{ValueType::Scalar, {1.0, 2.0}};
	const uint64_t outputStorage = sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								   sizeof(std::pair<std::string, ImageArray>);
	const uint64_t resultName =
		std::max<uint64_t>(std::string{}.capacity(), std::string_view("result").size());
	const uint64_t arrayBytes = outputStorage + resultName + 2 * sizeof(ElementValue);
	const auto exactArray = run("pc.math", values, 2.0, arrayBytes);
	REQUIRE(exactArray.first);
	CHECK(exactArray.second.front().Data == Value{ArrayValue{ValueType::Scalar, {3.0, 4.0}}});
	const auto shortArray = run("pc.math", values, 2.0, arrayBytes - 1);
	CHECK_FALSE(shortArray.first);
	CHECK(shortArray.second.empty());
	const auto exactScalar = run("pc.math", 2.0, 3.0, outputStorage + resultName);
	REQUIRE(exactScalar.first);
	CHECK(exactScalar.second.front().Data == Value{5.0});
	const auto shortScalar = run("pc.math", 2.0, 3.0, outputStorage + resultName - 1);
	CHECK_FALSE(shortScalar.first);
	CHECK(shortScalar.second.empty());
	const ArrayValue empty{ValueType::Scalar, {}};
	const auto emptyCompare = run("pc.compare", empty, empty, outputStorage + resultName - 1);
	CHECK_FALSE(emptyCompare.first);
	CHECK(emptyCompare.second.empty());
}

TEST_CASE(
	"Value text, matrix, and vector buffers are admitted before construction", "[imagegraph][node_values]"
) {
	const auto attempt = [](std::string_view type,
							uint64_t bytes,
							std::vector<std::pair<std::string, Value>> values) {
		const CatalogueEntry *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"budget", std::string(type), "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = bytes;
		for (const CatalogueInput &input : entry->Inputs)
			if (const auto fallback = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, *fallback);
		for (auto &[id, value] : values) {
			auto found = std::find_if(context.Values.begin(), context.Values.end(), [&](const auto &item) {
				return item.first == id;
			});
			if (found == context.Values.end())
				context.Values.emplace_back(std::move(id), std::move(value));
			else
				found->second = std::move(value);
		}
		const auto executor = detail::FindExecutor(type);
		REQUIRE(executor);
		const bool ok = executor(context);
		return std::tuple{ok, context.FailureCode, context.OutputValues.size()};
	};
	const uint64_t outputStorage = sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								   sizeof(std::pair<std::string, ImageArray>);
	const uint64_t nameBytes = std::string{}.capacity();
	const auto textOk = attempt(
		"pc.string_delete",
		outputStorage + nameBytes * 2,
		{{"text", std::string("abcdef")}, {"index", int64_t{2}}, {"amount", int64_t{2}}}
	);
	CHECK(std::get<0>(textOk));
	const auto textShort = attempt(
		"pc.string_delete",
		outputStorage + nameBytes * 2 - 1,
		{{"text", std::string("abcdef")}, {"index", int64_t{2}}, {"amount", int64_t{2}}}
	);
	CHECK_FALSE(std::get<0>(textShort));
	CHECK(std::get<1>(textShort) == Status::LimitExceeded);
	CHECK(std::get<2>(textShort) == 0);
	const std::string sixteenBytes(16, 'x');
	const uint64_t characterBytes = outputStorage + nameBytes + sixteenBytes.size();
	const auto characterOk = attempt(
		"pc.string_get_char",
		characterBytes,
		{{"text", sixteenBytes}, {"index", int64_t{1}}, {"amount", int64_t{1}}}
	);
	CHECK(std::get<0>(characterOk));
	const auto characterShort = attempt(
		"pc.string_get_char",
		characterBytes - 1,
		{{"text", sixteenBytes}, {"index", int64_t{1}}, {"amount", int64_t{1}}}
	);
	CHECK_FALSE(std::get<0>(characterShort));
	CHECK(std::get<1>(characterShort) == Status::LimitExceeded);
	CHECK(std::get<2>(characterShort) == 0);
	const auto matrixOk = attempt(
		"pc.matrix_invert",
		outputStorage + nameBytes + 4 * sizeof(double),
		{{"matrix", MatrixValue{2, 2, {4, 7, 2, 6}}}}
	);
	CHECK(std::get<0>(matrixOk));
	const auto vectorBytes = outputStorage + nameBytes + 3 * sizeof(double) + 3 * sizeof(ElementValue);
	const auto vectorOk = attempt(
		"pc.vector_swizzle", vectorBytes, {{"vector", Vector3{1, 2, 3}}, {"swizzle", std::string("zyx")}}
	);
	CHECK(std::get<0>(vectorOk));
	const auto vectorShort = attempt(
		"pc.vector_swizzle", vectorBytes - 1, {{"vector", Vector3{1, 2, 3}}, {"swizzle", std::string("zyx")}}
	);
	CHECK_FALSE(std::get<0>(vectorShort));
	CHECK(std::get<1>(vectorShort) == Status::LimitExceeded);
	CHECK(std::get<2>(vectorShort) == 0);
}

TEST_CASE("Multi-value nodes reserve every output before publishing", "[imagegraph][node_values]") {
	constexpr uint64_t priorLive = 1024;
	const auto attempt = [&](std::string_view type, uint64_t totalBytes) {
		const CatalogueEntry *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"atomic", std::string(type), "", {}, {}};
		EvaluationRequest request;
		detail::EvaluationBudget ledger(totalBytes);
		auto prior = ledger.Reserve(priorLive);
		REQUIRE(prior);
		detail::NodeContext context(node, *entry, request, ledger);
		context.ByteBudget = totalBytes;
		context.Values = {
			{"x", 1.0}, {"y", 2.0}, {"z", 3.0}, {"integer", false}, {"color", Colour{32, 64, 128, 255}}
		};
		Image surface;
		surface.Width = 1;
		surface.Height = 1;
		surface.Pixels = {32, 64, 128, 255};
		context.Images.emplace_back("surface", &surface);
		const auto executor = detail::FindExecutor(type);
		REQUIRE(executor);
		const bool ok = executor(context);
		return std::tuple{ok, context.FailureCode, context.OutputValues.size(), ledger.Peak()};
	};
	const auto outputBytes = [](std::string_view type) {
		const CatalogueEntry *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		uint64_t bytes =
			entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
									 sizeof(std::pair<std::string, ImageArray>));
		for (const auto &output : entry->Outputs) {
			bytes += std::max<uint64_t>(std::string{}.capacity(), output.Id.size());
			if (type == "pc.surface_data" && output.Id == "format_string")
				bytes += detail::RetainedPayloadBytes(Value{std::string("8bit RGBA")});
		}
		return bytes;
	};
	for (const std::string_view type :
		 {"pc.vector3", "pc.color_to_hsv", "pc.color_to_rgb", "pc.surface_data"}) {
		const uint64_t exactLimit = priorLive + outputBytes(type);
		const auto exact = attempt(type, exactLimit);
		CHECK(std::get<0>(exact));
		CHECK(std::get<1>(exact) == Status::Ok);
		CHECK(std::get<2>(exact) == FindCatalogueEntry(type)->Outputs.size());
		CHECK(std::get<3>(exact) == exactLimit);
		const auto shortRun = attempt(type, exactLimit - 1);
		CHECK_FALSE(std::get<0>(shortRun));
		CHECK(std::get<1>(shortRun) == Status::LimitExceeded);
		CHECK(std::get<2>(shortRun) == 0);
		CHECK(std::get<3>(shortRun) <= exactLimit - 1);
	}
}

TEST_CASE(
	"Legacy UTF-8 value evaluation borrows text and admits its output first", "[imagegraph][node_values]"
) {
	constexpr uint64_t priorLive = 512;
	const std::string source(16, 'x');
	const uint64_t inputBytes = sizeof(detail::ValueInputView);
	const uint64_t outputBytes =
		sizeof(AuthoredValue) +
		std::max<uint64_t>(std::string{}.capacity(), std::string_view("text").size()) +
		std::max<uint64_t>(source.size(), std::string{}.capacity());
	const auto attempt = [&](std::string_view type, uint64_t totalBytes) {
		detail::EvaluationBudget budget(totalBytes);
		auto prior = budget.Reserve(priorLive);
		REQUIRE(prior);
		auto inputCharge = budget.Reserve(inputBytes);
		REQUIRE(inputCharge);
		const Value value = source;
		std::vector<detail::ValueInputView> inputs;
		inputs.reserve(1);
		inputs.push_back({"text", &value});
		Node node{"legacy-text", std::string(type), "", {}, {}};
		EvaluationRequest request;
		detail::AllocationReservation outputCharge;
		std::vector<AuthoredValue> outputs;
		std::string failedPort, failureMessage;
		const Status status = detail::EvaluateValueNode(
			node, inputs, request, nullptr, outputs, budget, outputCharge, failedPort, failureMessage
		);
		const uint64_t peak = budget.Peak();
		return std::tuple{status, outputs.size(), peak};
	};
	const uint64_t exactLimit = priorLive + inputBytes + outputBytes;
	for (const std::string_view type : {"value.text_get_char", "value.text_delete"}) {
		const auto exact = attempt(type, exactLimit);
		CHECK(std::get<0>(exact) == Status::Ok);
		CHECK(std::get<1>(exact) == 1);
		CHECK(std::get<2>(exact) == exactLimit);
		const auto shortRun = attempt(type, exactLimit - 1);
		CHECK(std::get<0>(shortRun) == Status::LimitExceeded);
		CHECK(std::get<1>(shortRun) == 0);
		CHECK(std::get<2>(shortRun) <= exactLimit - 1);
	}
}

TEST_CASE(
	"Growing string replacement fits the shared ledger's live output peak", "[imagegraph][node_values]"
) {
	constexpr uint64_t priorLive = 4096;
	const CatalogueEntry *entry = FindCatalogueEntry("pc.string_replace");
	REQUIRE(entry);
	const uint64_t outputStorage =
		entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								 sizeof(std::pair<std::string, ImageArray>));
	const uint64_t nameBytes =
		std::max<uint64_t>(std::string{}.capacity(), std::string_view("results").size());
	const auto attempt = [&](const std::string &replacement, uint64_t totalBytes) {
		detail::EvaluationBudget ledger(totalBytes);
		auto prior = ledger.Reserve(priorLive);
		REQUIRE(prior);
		Node node{"replace", "pc.string_replace", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request, ledger);
		context.ByteBudget = totalBytes;
		context.Values = {
			{"text", std::string("x")}, {"find", std::string("x")}, {"replace", replacement}, {"all", true}
		};
		const auto executor = detail::FindExecutor("pc.string_replace");
		REQUIRE(executor);
		const bool ok = executor(context);
		const bool matches = context.OutputValues.size() == 1 &&
							 std::get<std::string>(context.OutputValues.front().Data) == replacement;
		return std::tuple{ok, context.FailureCode, context.OutputValues.size(), ledger.Peak(), matches};
	};
	const std::string boundaryReplacement(16, 'b');
	const uint64_t boundaryPeak = outputStorage + nameBytes + boundaryReplacement.size();
	const auto exactBoundary = attempt(boundaryReplacement, priorLive + boundaryPeak);
	REQUIRE(std::get<0>(exactBoundary));
	CHECK(std::get<3>(exactBoundary) == priorLive + boundaryPeak);
	CHECK(std::get<4>(exactBoundary));
	const auto shortBoundary = attempt(boundaryReplacement, priorLive + boundaryPeak - 1);
	CHECK_FALSE(std::get<0>(shortBoundary));
	CHECK(std::get<1>(shortBoundary) == Status::LimitExceeded);
	CHECK(std::get<2>(shortBoundary) == 0);
	const std::string replacement(16 * 1024, 'r');
	const uint64_t nodePeak = outputStorage + nameBytes + replacement.size();
	const auto exact = attempt(replacement, priorLive + nodePeak);
	REQUIRE(std::get<0>(exact));
	CHECK(std::get<2>(exact) == 1);
	CHECK(std::get<3>(exact) == priorLive + nodePeak);
	CHECK(std::get<4>(exact));
	const auto shortPeak = attempt(replacement, priorLive + nodePeak - 1);
	CHECK_FALSE(std::get<0>(shortPeak));
	CHECK(std::get<1>(shortPeak) == Status::LimitExceeded);
	CHECK(std::get<2>(shortPeak) == 0);
}

TEST_CASE(
	"Number Simple source default executes through the compiled catalogue",
	"[imagegraph][number_simple_acceptance]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"simple", "pc.number_simple", "", {}, {}}};
	document.Outputs = {{"out", "simple", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "out", {}, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{0.0});
}

TEST_CASE(
	"Number Simple passes source integer key samples and a linear driver to a linked control",
	"[imagegraph][number_simple_acceptance]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{5, 0, 4, "loop", 30};
	document.Nodes = {
		{"simple", "pc.number_simple", "", {}, {}},
		{"consumer", "pc.math", "", {}, {{"b", 10.0}}}
	};
	document.Links = {{"simple", "number", "consumer", "a"}};
	document.Outputs = {{"out", "consumer", "result"}, {"value", "simple", "number"}};
	document.Keyframes = {
		{"simple", "value", 0, 2.0, "source"}, {"simple", "value", 4, 6.0, "source"}
	};
	for (auto &key : document.Keyframes)
		key.Ease = KeyframeEase{};
	document.Tracks = {{"simple", "value", "hold", -1}};
	const auto verify = [](const Document &authored, uint64_t tick, double expected) {
		Diagnostic diagnostic;
		Document parsed;
		REQUIRE(Read(Write(authored), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == authored);
		Plan plan;
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue value, controlled;
		REQUIRE(EvaluateValue(parsed, plan, "value", request, value, diagnostic) == Status::Ok);
		CHECK(value.Data == Value{expected});
		REQUIRE(EvaluateValue(parsed, plan, "out", request, controlled, diagnostic) == Status::Ok);
		CHECK(controlled.Data == Value{expected + 10});
	};
	// Pinned linear side easing gives 2 + (6 - 2) * tick / 4 before the hold boundary.
	for (const auto &[tick, expected] :
		 {std::pair{uint64_t{0}, 2.0}, {uint64_t{1}, 3.0}, {uint64_t{2}, 4.0}, {uint64_t{4}, 6.0}})
		verify(document, tick, expected);
	document.Keyframes.resize(1);
	document.Keyframes.front().SourceDriver = KeyframeLinearDriver{2};
	// The source single-key linear driver adds speed * absolute frame.
	verify(document, 0, 2);
	verify(document, 3, 8);
}

TEST_CASE(
	"Number Simple native signed interpolation stays separate from unverified source key maps",
	"[imagegraph][number_simple_acceptance]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"simple", "pc.number_simple", "", {}, {}}};
	document.Outputs = {{"out", "simple", "number"}};
	document.Keyframes = {
		{"simple", "value", 0, 2.0, "linear"}, {"simple", "value", 0, 6.0, "linear"}
	};
	FrameTime first, last;
	REQUIRE(SplitFrameTime(-.5, first));
	REQUIRE(SplitFrameTime(.5, last));
	REQUIRE(SetFrameTime(document.Keyframes[0], first));
	REQUIRE(SetFrameTime(document.Keyframes[1], last));
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	for (const auto &[frame, expected] :
		 {std::pair{-.5, 2.0}, {0.0, 4.0}, {.25, 5.0}, {.5, 6.0}}) {
		FrameTime clock;
		EvaluationRequest request;
		REQUIRE(SplitFrameTime(frame, clock));
		REQUIRE(SetFrameTime(request, clock));
		EvaluatedValue output;
		REQUIRE(EvaluateValue(parsed, plan, "out", request, output, diagnostic) == Status::Ok);
		CHECK(output.Data == Value{expected});
	}
	for (auto &key : parsed.Keyframes) {
		key.Interpolation = "source";
		key.Ease = KeyframeEase{};
	}
	parsed.Tracks = {{"simple", "value", "hold", -1}};
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Subframe = .25;
	EvaluatedValue sentinel;
	sentinel.Data = 99.0;
	CHECK(EvaluateValue(parsed, plan, "out", request, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Message == "source fractional key-map index coercion is unverified");
	CHECK(sentinel.Data == Value{99.0});
}

TEST_CASE(
	"Compiled Math and Compare defaults execute their source branches after persistence",
	"[imagegraph][math_compare_acceptance]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"math", "pc.math", "", {}, {}}, {"compare", "pc.compare", "", {}, {}}};
	document.Outputs = {{"math", "math", "result"}, {"compare", "compare", "result"}};
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(parsed, plan, "math", {}, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{0.0});
	REQUIRE(EvaluateValue(parsed, plan, "compare", {}, output, diagnostic) == Status::Ok);
	// The source's initial output is false; update evaluates its default 0 == 0 to true.
	CHECK(output.Data == Value{true});
}

TEST_CASE(
	"Persisted Math radians and rounding display controls preserve source numeric results",
	"[imagegraph][math_compare_acceptance]"
) {
	struct Fixture {
		int64_t Mode;
		double A;
		double B;
		double Expected;
	};
	for (const auto &fixture :
		 {Fixture{6, std::numbers::pi / 2, 2, 2},
		  Fixture{7, std::numbers::pi, 2, -2},
		  Fixture{8, std::numbers::pi / 4, 3, 3},
		  Fixture{10, -2.5, 0, -3},
		  Fixture{11, -2.5, 0, -2},
		  Fixture{12, -2.5, 0, -2}}) {
		for (bool setting : {false, true}) {
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"math",
				 "pc.math",
				 "",
				 {},
				 {{"type", EnumValue{fixture.Mode}},
				  {"a", fixture.A},
				  {"b", fixture.B},
				  {"angle", EnumValue{0}},
				  {"to_integer", setting},
				  {"output_vector", setting}}}
			};
			document.Outputs = {{"out", "math", "result"}};
			Document parsed;
			Diagnostic diagnostic;
			REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
			CHECK(parsed == document);
			Plan plan;
			REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
			EvaluatedValue output;
			REQUIRE(EvaluateValue(parsed, plan, "out", {}, output, diagnostic) == Status::Ok);
			// Pinned update changes output type/display metadata without casting evalArray's real result.
			REQUIRE(std::holds_alternative<double>(output.Data));
			CHECK(std::abs(std::get<double>(output.Data) - fixture.Expected) < 1e-12);
		}
	}
}

TEST_CASE(
	"Compiled Compare applies all six source relations after unequal array zero padding",
	"[imagegraph][math_compare_acceptance]"
) {
	const std::array<std::array<bool, 3>, 6> expected{{
		{true, true, false}, {false, false, true}, {false, false, true},
		{true, true, true}, {false, false, false}, {true, true, false}
	}};
	Document document;
	document.FormatVersion = 9;
	for (int64_t mode = 0; mode < 6; ++mode) {
		const std::string id = "compare_" + std::to_string(mode);
		document.Nodes.push_back(
			{id,
			 "pc.compare",
			 "",
			 {},
			 {{"type", EnumValue{mode}},
			  {"a", ArrayValue{ValueType::Scalar, {2.0, 3.0, 4.0}}},
			  {"b", ArrayValue{ValueType::Scalar, {2.0, 3.0}}}}}
		);
		document.Outputs.push_back({id, id, "result"});
	}
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	for (size_t mode = 0; mode < expected.size(); ++mode) {
		EvaluatedValue output;
		REQUIRE(EvaluateValue(parsed, plan, "compare_" + std::to_string(mode), {}, output, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(output.Data);
		CHECK(array.ElementType == ValueType::Boolean);
		CHECK(array.Elements == std::vector<ElementValue>{expected[mode][0], expected[mode][1], expected[mode][2]});
	}
}
