#include <engine/bakegraph/Schema.hpp>
#include <engine/graphcore/Schema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

TEST_SUITE_ID("engine.bakegraph.schema")
TEST_DEPENDS("engine.graphcore.schema")

using engine::bake::NodeKind;
using engine::bake::NodeSchemaFor;
using engine::bake::NodeSchemas;

TEST_CASE("bake schemas cover the existing durable kind ordinals", "[bakegraph][schema]") {
	constexpr std::array<std::pair<NodeKind, std::string_view>, 14> kinds{{
		{NodeKind::Source, "source"},
		{NodeKind::Builtin, "builtin"},
		{NodeKind::Import, "import"},
		{NodeKind::Fit, "fit"},
		{NodeKind::Scale, "scale"},
		{NodeKind::Smooth, "smooth"},
		{NodeKind::Resize, "resize"},
		{NodeKind::Opaque, "opaque"},
		{NodeKind::Mipmap, "mipmap"},
		{NodeKind::Retime, "retime"},
		{NodeKind::Write, "write"},
		{NodeKind::Rasterize, "rasterize"},
		{NodeKind::Decimate, "decimate"},
		{NodeKind::Flipbook, "flipbook"},
	}};
	const auto schemas = NodeSchemas();
	REQUIRE(schemas.size() == kinds.size());
	for (size_t index = 0; index < kinds.size(); ++index) {
		CHECK(static_cast<size_t>(kinds[index].first) == index);
		REQUIRE(NodeSchemaFor(kinds[index].first) == &schemas[index]);
		CHECK(schemas[index].Id == kinds[index].second);
		CHECK(schemas[index].Ports.size() == (index < 2 ? 1 : 2));
	}
	CHECK(NodeSchemaFor(static_cast<NodeKind>(255)) == nullptr);

	const engine::graphcore::Limits limits{
		.MaximumNodes = 0,
		.MaximumConnections = 0,
		.MaximumSchemas = schemas.size(),
		.MaximumPorts = 26,
		.MaximumIdentityBytes = 4096,
	};
	engine::graphcore::Diagnostic diagnostic;
	CHECK(
		engine::graphcore::ValidateSchemas(schemas, limits, diagnostic) ==
		engine::graphcore::CompileStatus::Ok
	);
}

TEST_CASE("bake schemas describe payload kinds without moving runtime checks", "[bakegraph][schema]") {
	using engine::graphcore::PortDirection;
	const auto *source = NodeSchemaFor(NodeKind::Source);
	const auto *import = NodeSchemaFor(NodeKind::Import);
	const auto *fit = NodeSchemaFor(NodeKind::Fit);
	const auto *write = NodeSchemaFor(NodeKind::Write);
	REQUIRE(source != nullptr);
	REQUIRE(import != nullptr);
	REQUIRE(fit != nullptr);
	REQUIRE(write != nullptr);
	CHECK(source->Ports[0].Direction == PortDirection::Output);
	CHECK(source->Ports[0].ValueType == "bake.bytes");
	CHECK(import->Ports[0].ValueType == "bake.bytes");
	CHECK(import->Ports[0].Optional);
	CHECK(import->Ports[1].ValueType == "any");
	CHECK(fit->Ports[0].ValueType == "bake.mesh");
	CHECK(fit->Ports[0].MaximumConnections == 1);
	CHECK(write->Ports[0].ValueType == "any");
	CHECK(write->Ports[1].ValueType == "any");
}
