#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sstream>
TEST_SUITE_ID("engine.imagegraph.rigid_atlas_transport")
using namespace engine::imagegraph;
namespace {
	Document Transport(Value value) {
		Document document;
		document.FormatVersion = 9;
		Node object{"object", "pc.struct", ""};
		object.DynamicInputs = {
			{"key_0", ValueType::Text, Value{std::string{"payload"}}},
			{"value_0", ValueType::Any, std::move(value)}
		};
		document.Nodes = {
			std::move(object), {"get", "pc.struct_get", "", {}, {{"key", std::string{"payload"}}}}
		};
		document.Links = {{"object", "struct", "get", "struct"}};
		document.Outputs = {{"payload", "get", "value"}};
		return document;
	}
}
TEST_CASE(
	"Rigid aliases persist through native arrays and source Any struct transport", "[imagegraph][rigid]"
) {
	RigidValue alias;
	alias.Data.emplace() = {"owner", "body/0"};
	ArrayValue payload{ValueType::Rigid, {ElementValue{alias}}};
	const auto document = Transport(payload);
	std::string encoded;
	Diagnostic diagnostic;
	encoded = Write(document);
	REQUIRE(!encoded.empty());
	Document loaded;
	REQUIRE(Read(encoded, loaded, diagnostic) == Status::Ok);
	CHECK(loaded == document);
	Plan plan;
	INFO(diagnostic.Message);
	REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateValue(loaded, plan, "payload", {}, result, diagnostic) == Status::Ok);
	CHECK(result.Data == Value{payload});
	auto copy = std::get<ArrayValue>(result.Data);
	std::get<RigidValue>(copy.Elements[0]).Data->BodyId = "other";
	CHECK(std::get<RigidValue>(payload.Elements[0]).Data->BodyId == "body/0");
}
TEST_CASE("Atlas native transport retains source class, transforms and owned images", "[imagegraph][atlas]") {
	for (AtlasKind kind : {AtlasKind::Atlas, AtlasKind::SurfaceAtlas}) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = kind;
		data.Surface.Data = Image{1, 1, {10, 20, 30, 255}};
		data.OriginalSurface = SurfaceValue{Image{1, 1, {50, 60, 70, 255}}};
		data.Position = {2, 3};
		data.Scale = {4, 5};
		data.Dimension = {6, 7};
		data.OriginalDimension = {8, 9};
		data.RotationDegrees = 12;
		data.Alpha = .25;
		ArrayValue payload{ValueType::Atlas, {ElementValue{atlas}}};
		auto document = Transport(payload);
		std::string encoded;
		Diagnostic diagnostic;
		encoded = Write(document);
		REQUIRE(!encoded.empty());
		Document loaded;
		REQUIRE(Read(encoded, loaded, diagnostic) == Status::Ok);
		CHECK(loaded == document);
		Plan plan;
		INFO(diagnostic.Message);
		REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		INFO(diagnostic.Message);
		REQUIRE(EvaluateValue(loaded, plan, "payload", {}, result, diagnostic) == Status::Ok);
		CHECK(result.Data == Value{payload});
	}
}
