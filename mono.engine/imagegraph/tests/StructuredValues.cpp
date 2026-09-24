#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_SUITE_ID("engine.imagegraph.structured_values")

using namespace engine::imagegraph;

namespace {
	Gradient BlackWhite() {
		return Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}};
	}

	Curve LinearCurve() {
		return Curve{
			{0, 1, 0, 0, 1, 0}, {{{0, 0, 0, 0, 1.0 / 3.0, 1.0 / 3.0}, {-1.0 / 3.0, -1.0 / 3.0, 1, 1, 0, 0}}}
		};
	}

	Document StructuredDocument() {
		Document document;
		document.FormatVersion = 3;
		Node node{"opaque", "vendor.structured", "", {}, {}};
		node.Values = {
			{"gradient", BlackWhite()},
			{"area", Area{}},
			{"curve", LinearCurve()},
			{"vector4", Vector4{1, 2, 3, 4}},
			{"path2d", Path2D{true, {{{0, 0, 0, 0, 0, 0}, 0}, {{1, 1, 0, 0, 0, 0}, 1}}, {{0, 1}, {100, 1}}}},
		};
		node.DynamicInputs = {{"gradient_in", ValueType::Gradient, BlackWhite()}};
		document.Nodes.push_back(node);
		document.Junctions.push_back({"j", "", ValueType::Area, Area{}});
		document.Keyframes.push_back({"opaque", "curve", 2, LinearCurve(), "step"});
		document.Outputs.push_back({"out", "opaque", "image"});
		return document;
	}
}

TEST_CASE(
	"v3 structured authored values round trip through all record locations", "[imagegraph][structured]"
) {
	const Document source = StructuredDocument();
	const std::string serialized = Write(source);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(serialized, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == source);
	CHECK(Write(parsed) == serialized);
	CHECK(serialized.find("value 0") != std::string::npos);
	CHECK(serialized.find(" g ") != std::string::npos);
	CHECK(serialized.find(" r ") != std::string::npos);
	CHECK(serialized.find(" q ") != std::string::npos);
	CHECK(serialized.find(" w ") != std::string::npos);
	CHECK(serialized.find(" p ") != std::string::npos);
}

TEST_CASE(
	"v3 structured ports route by type without executing unsupported elements", "[imagegraph][structured]"
) {
	Document document;
	document.FormatVersion = 3;
	Node array{"array", "value.array", "", {}, {}};
	array.DynamicInputs = {{"gradient", ValueType::Gradient, BlackWhite()}};
	document.Nodes.push_back(array);
	document.Junctions.push_back({"source", "", ValueType::Gradient, BlackWhite()});
	document.Links.push_back({"source", "value", "array", "gradient"});
	document.Outputs.push_back({"out", "array", "array"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray result;
	CHECK(
		EvaluateArray(document, plan, "out", EvaluationRequest{}, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "array");
	CHECK(diagnostic.Port == "gradient");
	document.Junctions[0].Type = ValueType::Area;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "source");
}

TEST_CASE(
	"v3 reader bounds structured lengths and rejects invalid numeric values", "[imagegraph][structured]"
) {
	const Document source = StructuredDocument();
	const std::string serialized = Write(source);
	Document parsed = source;
	Diagnostic diagnostic;
	for (const auto &[needle, replacement] : {
			 std::pair{"g 0 2", "g 0 129"},
			 std::pair{"q 2", "q 257"},
			 std::pair{"p 1 2 2", "p 1 1025 2"},
			 std::pair{"w 1 2 3 4", "w nan 2 3 4"},
		 }) {
		std::string malformed = serialized;
		const size_t position = malformed.find(needle);
		REQUIRE(position != std::string::npos);
		malformed.replace(position, std::string(needle).size(), replacement);
		CHECK(Read(malformed, parsed, diagnostic) == Status::Malformed);
		CHECK(parsed == source);
	}
	Document invalid;
	invalid.FormatVersion = 3;
	Node array{"array", "value.array", "", {}, {}};
	Gradient bad = BlackWhite();
	bad.Keys[0].Time = std::numeric_limits<double>::infinity();
	array.DynamicInputs.push_back({"gradient", ValueType::Gradient, bad});
	invalid.Nodes.push_back(array);
	invalid.Outputs.push_back({"out", "array", "array"});
	Plan plan;
	CHECK(Compile(invalid, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "array");
	CHECK(diagnostic.Port == "gradient");
}

TEST_CASE("v1 and v2 documents migrate to v3 without changing old values", "[imagegraph][structured]") {
	{
		Document parsed;
		Diagnostic diagnostic;
		const std::string forged = "imagegraph 2\nnode \"array\" \"value.array\" \"\" 0 0\n"
								   "dynamic 0 \"array\" \"gradient\" gradient 0\n"
								   "output \"out\" \"array\" \"array\"\n";
		CHECK(Read(forged, parsed, diagnostic) == Status::Malformed);
	}
	for (uint32_t oldVersion : {1u, 2u}) {
		Document document;
		document.FormatVersion = oldVersion;
		document.Nodes.push_back(
			{"solid",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 4}}}}
		);
		document.Outputs.push_back({"out", "solid", "image"});
		const auto oldValues = document.Nodes[0].Values;
		Diagnostic diagnostic;
		REQUIRE(Migrate(document, diagnostic) == Status::Ok);
		CHECK(document.FormatVersion == 6);
		CHECK(document.Nodes[0].Values == oldValues);
		Document parsed;
		REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
	}
}
