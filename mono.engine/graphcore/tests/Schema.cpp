#include <engine/graphcore/Schema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.graphcore.schema")

using namespace engine::graphcore;

namespace {
	Limits SchemaLimits() {
		return {
			.MaximumNodes = 16,
			.MaximumConnections = 32,
			.MaximumSchemas = 8,
			.MaximumPorts = 32,
			.MaximumIdentityBytes = 4096,
		};
	}

	NodeSchema PassSchema() {
		return {
			.Id = "pass",
			.Ports =
				{
					{.Id = "value", .ValueType = "number"},
					{.Id = "value", .ValueType = "number", .Direction = PortDirection::Output},
				},
			.BypassMappings = {{"value", "value"}},
		};
	}
}

TEST_CASE("schema types match exactly with only an explicit any wildcard", "[graphcore][schema]") {
	CHECK(ComparePortTypes("number", "number") == TypeCompatibility::Exact);
	CHECK(ComparePortTypes("any", "any") == TypeCompatibility::Exact);
	CHECK(ComparePortTypes("domain.mesh", "any") == TypeCompatibility::DestinationAny);
	CHECK(ComparePortTypes("any", "domain.mesh") == TypeCompatibility::SourceAny);
	CHECK(ComparePortTypes("domain.mesh", "domain.texture") == TypeCompatibility::Incompatible);
	CHECK(ComparePortTypes("number|boolean", "number") == TypeCompatibility::Incompatible);
	CHECK(ComparePortTypes("", "any") == TypeCompatibility::Incompatible);
	CHECK(ComparePortTypes("any", "") == TypeCompatibility::Incompatible);
}

TEST_CASE("schemas share port spelling across directions and validate bypass routes", "[graphcore][schema]") {
	std::array schemas{PassSchema()};
	Diagnostic diagnostic{.Status = CompileStatus::Cycle, .Node = "old", .Port = "old"};
	REQUIRE(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::Ok);
	CHECK(diagnostic.Status == CompileStatus::Ok);
	CHECK(diagnostic.Node.empty());
	CHECK(diagnostic.Port.empty());
	CHECK(diagnostic.Blocked == NO_NODE);
	CHECK(diagnostic.Cycle.empty());

	SECTION("missing input") {
		schemas[0].BypassMappings[0].Input = "absent";
	}
	SECTION("missing output") {
		schemas[0].BypassMappings[0].Output = "absent";
	}
	SECTION("incompatible types") {
		schemas[0].Ports[1].ValueType = "boolean";
	}
	SECTION("output used twice") {
		schemas[0].BypassMappings.push_back({"value", "value"});
	}
	CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::InvalidBypass);
	CHECK(diagnostic.Node == "pass");
}

TEST_CASE(
	"schema validation rejects ambiguous declarations before consumers use them", "[graphcore][schema]"
) {
	std::vector schemas{PassSchema()};
	Diagnostic diagnostic;
	SECTION("duplicate schema") {
		schemas.push_back(PassSchema());
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::DuplicateSchema);
	}
	SECTION("duplicate directional port") {
		schemas[0].Ports.push_back(schemas[0].Ports[0]);
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::DuplicatePort);
		CHECK(diagnostic.Port == "value");
	}
	SECTION("invalid direction") {
		schemas[0].Ports[0].Direction = static_cast<PortDirection>(255);
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::InvalidDirection);
	}
	SECTION("empty schema identity") {
		schemas[0].Id.clear();
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::InvalidIdentity);
	}
	SECTION("null byte in a port identity") {
		schemas[0].Ports[0].Id = std::string("left\0right", 10);
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::InvalidIdentity);
	}
	SECTION("empty type") {
		schemas[0].Ports[0].ValueType.clear();
		CHECK(ValidateSchemas(schemas, SchemaLimits(), diagnostic) == CompileStatus::InvalidIdentity);
	}
}

TEST_CASE("schema counts and all identity bytes have explicit inclusive budgets", "[graphcore][schema]") {
	const std::array schemas{PassSchema()};
	Limits limits = SchemaLimits();
	limits.MaximumSchemas = 1;
	limits.MaximumPorts = 2;
	// pass + two (value, number) declarations + one (value, value) bypass.
	limits.MaximumIdentityBytes = 4 + 2 * (5 + 6) + 5 + 5;
	Diagnostic diagnostic;
	REQUIRE(ValidateSchemas(schemas, limits, diagnostic) == CompileStatus::Ok);
	SECTION("schema budget") {
		limits.MaximumSchemas = 0;
	}
	SECTION("aggregate port budget") {
		limits.MaximumPorts = 1;
	}
	SECTION("identity byte budget") {
		--limits.MaximumIdentityBytes;
	}
	CHECK(ValidateSchemas(schemas, limits, diagnostic) == CompileStatus::LimitExceeded);
}

TEST_CASE("empty schema sets need no budget and refusal text stays bounded", "[graphcore][schema]") {
	Diagnostic diagnostic;
	REQUIRE(ValidateSchemas({}, {}, diagnostic) == CompileStatus::Ok);
	std::array schemas{PassSchema()};
	schemas[0].Id.assign(1024, 'x');
	Limits limits = SchemaLimits();
	limits.MaximumIdentityBytes = 10;
	CHECK(ValidateSchemas(schemas, limits, diagnostic) == CompileStatus::LimitExceeded);
	CHECK(diagnostic.Node.size() == 128);
	// Even malformed oversized text is rejected by its byte budget before scanning.
	schemas[0].Id.back() = '\0';
	CHECK(ValidateSchemas(schemas, limits, diagnostic) == CompileStatus::LimitExceeded);
}
