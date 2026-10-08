#include <engine/imagegraph/PortCompatibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.port_compatibility")

TEST_CASE("Source junction casts preserve direction and reference isolation", "[imagegraph]") {
	using namespace engine::imagegraph;
	constexpr ValueType numbers[] = {
		ValueType::Boolean,
		ValueType::Integer,
		ValueType::Enum,
		ValueType::Scalar,
		ValueType::Vector2,
		ValueType::Vector3,
		ValueType::Vector4,
		ValueType::Quaternion,
		ValueType::Array,
		ValueType::Area,
		ValueType::Matrix
	};
	for (const auto from : numbers) {
		for (const auto to : numbers)
			CHECK(CatalogueJunctionCompatible(from, to));
		CHECK(CatalogueJunctionCompatible(from, ValueType::Text));
		CHECK_FALSE(CatalogueJunctionCompatible(ValueType::Text, from));
		CHECK(CatalogueJunctionCompatible(ValueType::Image, from));
		CHECK_FALSE(CatalogueJunctionCompatible(from, ValueType::Image));
		CHECK(CatalogueJunctionCompatible(from, ValueType::Colour));
		CHECK(CatalogueJunctionCompatible(ValueType::Colour, from));
	}
	CHECK(CatalogueJunctionCompatible(ValueType::Colour, ValueType::Gradient));
	CHECK_FALSE(CatalogueJunctionCompatible(ValueType::Gradient, ValueType::Colour));
	CHECK(CatalogueJunctionCompatible(ValueType::Strand, ValueType::Path2D));
	CHECK_FALSE(CatalogueJunctionCompatible(ValueType::Path2D, ValueType::Strand));
	for (size_t index = 0; index <= static_cast<size_t>(ValueType::Noise3DVector3); ++index) {
		const auto type = static_cast<ValueType>(index);
		CHECK(CatalogueJunctionCompatible(type, type));
		CHECK(CatalogueJunctionCompatible(ValueType::Any, type) == (type != ValueType::NodeRef));
		CHECK(CatalogueJunctionCompatible(type, ValueType::Any) == (type != ValueType::NodeRef));
	}
}
