#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.ang.registry")
TEST_DEPENDS("engine.ang.value")

using namespace engine::ang;

TEST_CASE("definitions refuse duplicates, unknown types and bad ports", "[ang][registry]") {
	Registry registry = Registry::WithScalars();
	CHECK(!registry.DefineType({.Id = "number"}).Ok);
	CHECK(!registry.DefineType({.Id = "any"}).Ok);
	CHECK(!registry.DefineType({.Id = ""}).Ok);
	CHECK(registry.DefineNode({.Id = "a", .Outputs = {{.Name = "out", .Type = "number"}}}).Ok);
	CHECK(!registry.DefineNode({.Id = "a"}).Ok);
	CHECK(!registry.DefineNode({.Id = "b", .Inputs = {{.Name = "x", .Type = "nope"}}}).Ok);
	CHECK(!registry
			   .DefineNode(
				   {.Id = "c", .Inputs = {{.Name = "x", .Type = "number"}, {.Name = "x", .Type = "number"}}}
			   )
			   .Ok);
	CHECK(!registry.DefineNode({.Id = "d", .Version = 0}).Ok);
	CHECK(!registry.DefineNode({.Id = "e", .Bypass = {{.Input = "x", .Output = "y"}}}).Ok);
	CHECK(!registry.DefineSubgraph({.Id = "a", .Body = std::make_shared<Graph>()}).Ok);
	CHECK(!registry.DefineSubgraph({.Id = "f"}).Ok);
	CHECK(registry.FindNode("a")->Label == "a");
	CHECK(registry.Nodes().size() == 1);
}

TEST_CASE("links are exact, wildcard or through one conversion", "[ang][registry]") {
	const Registry registry = tests::MakeRegistry();
	CHECK(registry.CheckLink("number", "number").Ok);
	CHECK(registry.CheckLink("number", "any").Ok);
	CHECK(registry.CheckLink("any", "string").Ok);
	CHECK(!registry.CheckLink("number", "string").Ok);
	const LinkCheck widened = registry.CheckLink("int", "number");
	REQUIRE(widened.Ok);
	REQUIRE(widened.Via != nullptr);
	CHECK(widened.Via->Convert(Value(int64_t{3})).TypeName() == "number");
	// An unregistered type connects to nothing, not even the wildcard.
	CHECK(!registry.CheckLink("ghost", "any").Ok);
	CHECK(NormaliseType("data.ANY") == "any");
}

TEST_CASE("byte accounting uses the type hook", "[ang][registry]") {
	const Registry registry = tests::MakeRegistry();
	const Value image =
		Value::FromBlob("image", {.Data = std::make_shared<int>(0), .Bytes = 4096, .Identity = 1});
	CHECK(registry.BytesOf(image) == 4096);
	CHECK(registry.BytesOf(Value(1.0)) == sizeof(Value));
	CHECK(DomainFromName("Tick") == Domain::Tick);
	CHECK(!DomainFromName("Later").has_value());
	CHECK(std::string_view(DomainName(Domain::Frame)) == "Frame");
}
