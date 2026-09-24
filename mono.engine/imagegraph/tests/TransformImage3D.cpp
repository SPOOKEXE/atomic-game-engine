#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.transform_image_3d")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;

	Node Solid(std::string id) {
		return {
			std::move(id),
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}
		};
	}

	Node Transform() {
		return {
			"transform",
			"image.transform_3d",
			"",
			{},
			{{"position", Vector3{}},
			 {"anchor", Vector3{}},
			 {"rotation", Quaternion{}},
			 {"scale", Vector3{1, 1, 1}},
			 {"texture_tiling", Vector2{1, 1}},
			 {"projection", EnumValue{1}},
			 {"fov", 45.0},
			 {"view_range", Vector2{.001, 10}},
			 {"depth_range", Vector2{0, 1}}}
		};
	}
}

TEST_CASE("Transform Image 3D publishes typed source-backed ports", "[imagegraph]") {
	const NodeSchema *schema = FindSchema("image.transform_3d");
	REQUIRE(schema != nullptr);
	REQUIRE(schema->Ports.size() == 5);
	CHECK(schema->Ports[0].Id == "surface");
	CHECK(schema->Ports[0].Type == ValueType::Image);
	CHECK(schema->Ports[2].Id == "mesh");
	CHECK(schema->Ports[2].Type == ValueType::Mesh);
	CHECK(schema->Ports[3].Id == "rendered");
	CHECK(schema->Ports[4].Id == "depth");
	CHECK(schema->Properties.size() == 9);
	CHECK(schema->Properties[0].Type == ValueType::Vector3);
	CHECK(schema->Properties[2].Type == ValueType::Quaternion);
	CHECK(schema->Properties[5].Type == ValueType::Enum);
}

TEST_CASE("Transform Image 3D refuses a missing front surface and CPU execution", "[imagegraph]") {
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {Transform()};
	document.Outputs = {{"out", "transform", "rendered"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "transform");
	CHECK(diagnostic.Port == "surface");

	document.Nodes.insert(document.Nodes.begin(), Solid("surface"));
	document.Links = {{"surface", "image", "transform", "surface"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::string encoded = Write(document);
	Document decoded;
	REQUIRE(Read(encoded, decoded, diagnostic) == Status::Ok);
	CHECK(decoded == document);
	Image image;
	CHECK(Evaluate(document, plan, "out", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "transform");
}

TEST_CASE("Transform Image 3D resolves selected animated controls without CPU rendering", "[imagegraph]") {
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {Solid("surface"), Transform()};
	document.Links = {{"surface", "image", "transform", "surface"}};
	document.Outputs = {{"out", "transform", "rendered"}};
	document.Keyframes = {
		{"transform", "fov", 0, 45.0, "linear"},
		{"transform", "fov", 10, 90.0, "linear"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::vector<AuthoredValue> values;
	REQUIRE(
		ResolveNodeValues(document, plan, "out", "transform", {.Tick = 5}, values, diagnostic) == Status::Ok
	);
	const auto fov = std::find_if(values.begin(), values.end(), [](const AuthoredValue &value) {
		return value.Port == "fov";
	});
	REQUIRE(fov != values.end());
	CHECK(std::get<double>(fov->Data) == 67.5);
	CHECK(
		ResolveNodeValues(document, plan, "out", "missing", {}, values, diagnostic) == Status::InvalidValue
	);
	CHECK(diagnostic.NodeId == "missing");
}
