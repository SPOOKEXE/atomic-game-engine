#include <engine/graphcore/Compile.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Schema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.schema")

using namespace engine;

namespace {
	graphcore::Limits Bounds() {
		return {
			.MaximumNodes = 16,
			.MaximumConnections = 16,
			.MaximumSchemas = 16,
			.MaximumPorts = 64,
			.MaximumIdentityBytes = 4096
		};
	}
}

TEST_CASE("image declarations resolve operation kinds and typed directional pins", "[imagegraph][schema]") {
	const std::array<imagegraph::Operation, 7> operations{
		imagegraph::Source{},
		imagegraph::Solid{},
		imagegraph::Resize{},
		imagegraph::Crop{},
		imagegraph::Transform{},
		imagegraph::Flip{},
		imagegraph::Blend{}
	};
	const auto schemas = imagegraph::NodeSchemas();
	graphcore::Diagnostic diagnostic;
	REQUIRE(graphcore::ValidateSchemas(schemas, Bounds(), diagnostic) == graphcore::CompileStatus::Ok);
	REQUIRE(schemas.size() == operations.size());
	for (size_t index = 0; index < operations.size(); ++index) {
		const auto *schema = imagegraph::FindNodeSchema(imagegraph::Kind(operations[index]));
		REQUIRE(schema != nullptr);
		CHECK(schema == &schemas[index]);
	}
	CHECK(imagegraph::FindNodeSchema("image.unknown") == nullptr);
	const std::array nodes{graphcore::Node{"source", "image.solid"}, graphcore::Node{"flip", "image.flip"}};
	const std::array links{graphcore::Connection{"source", "Image", "flip", "Image"}};
	graphcore::CompiledGraph compiled;
	REQUIRE(
		graphcore::CompileGraph(nodes, links, schemas, Bounds(), compiled, diagnostic) ==
		graphcore::CompileStatus::Ok
	);
	CHECK(compiled.Dependencies.Order == std::vector<graphcore::NodeIndex>{0, 1});
	REQUIRE(compiled.Connections.size() == 1);
	CHECK(compiled.Connections[0].FromPort == 0);
	CHECK(compiled.Connections[0].ToPort == 0);
}

TEST_CASE("image inputs refuse disconnected and foreign typed producers", "[imagegraph][schema]") {
	const auto imageSchemas = imagegraph::NodeSchemas();
	std::vector<graphcore::NodeSchema> schemas(imageSchemas.begin(), imageSchemas.end());
	schemas.push_back({"scalar.source", {{"Value", "scalar.number", graphcore::PortDirection::Output}}, {}});
	const std::array nodes{graphcore::Node{"source", "scalar.source"}, graphcore::Node{"flip", "image.flip"}};
	graphcore::CompiledGraph compiled;
	graphcore::Diagnostic diagnostic;
	CHECK(
		graphcore::CompileGraph(nodes, {}, schemas, Bounds(), compiled, diagnostic) ==
		graphcore::CompileStatus::MissingInput
	);
	CHECK(diagnostic.Node == "flip");
	CHECK(diagnostic.Port == "Image");
	const std::array links{graphcore::Connection{"source", "Value", "flip", "Image"}};
	CHECK(
		graphcore::CompileGraph(nodes, links, schemas, Bounds(), compiled, diagnostic) ==
		graphcore::CompileStatus::TypeMismatch
	);
	CHECK(diagnostic.Node == "flip");
	CHECK(diagnostic.Port == "Image");
}
