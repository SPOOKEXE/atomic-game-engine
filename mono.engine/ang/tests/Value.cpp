#include <engine/ang/Value.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.ang.value")

using namespace engine::ang;

TEST_CASE("scalar values carry their type and hash by content", "[ang][value]") {
	CHECK(Value(1.5).TypeName() == "number");
	CHECK(Value(int64_t{3}).TypeName() == "int");
	CHECK(Value("text").TypeName() == "string");
	CHECK(Value().Empty());
	CHECK(Value(2.0).Hash() == Value(2.0).Hash());
	CHECK(Value(2.0).Hash() != Value(3.0).Hash());
	// Same bits, different type tag: different hash.
	CHECK(Value(2.0).Hash() != Value(2.0).WithType("meters").Hash());
	CHECK(Value(engine::core::Name("alpha")).Hash() == Value(engine::core::Name("alpha")).Hash());
	CHECK(Value(int64_t{4}).AsNumber() == 4.0);
	CHECK(Value(true).AsNumber() == 1.0);
	CHECK(Value("x").AsNumber(-1.0) == -1.0);
}

TEST_CASE("blobs share their payload and hash by identity only", "[ang][value]") {
	auto payload = std::make_shared<int>(5);
	const Value first = Value::FromBlob("image", {.Data = payload, .Bytes = 400, .Identity = 77});
	const Value copy = first;
	CHECK(copy.Get<Blob>()->Data.get() == payload.get());
	CHECK(first == copy);
	CHECK(first.Bytes() >= 400);
	CHECK(!first.Opaque());
	const Value other =
		Value::FromBlob("image", {.Data = std::make_shared<int>(5), .Bytes = 400, .Identity = 77});
	CHECK(first.Hash() == other.Hash());
	const Value unknown = Value::FromBlob("image", {.Data = payload, .Bytes = 400});
	CHECK(unknown.Opaque());
	CHECK(unknown.Hash() == 0);
}

TEST_CASE("text hashing separates boundaries", "[ang][value]") {
	CHECK(HashText(HashText(HASH_SEED, "ab"), "c") != HashText(HashText(HASH_SEED, "a"), "bc"));
}
