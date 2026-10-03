#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.array_clone_payload")
using namespace engine::imagegraph;
TEST_CASE(
	"Array clone admission counts borrowed retained capacity without first cloning", "[imagegraph][payload]"
) {
	ArrayValue array{ValueType::Scalar, {1.0, 2.0}};
	array.Elements.reserve(67);
	const auto bytes = ValueClonePayloadBytes(array);
	REQUIRE(bytes);
	CHECK(*bytes == sizeof(ArrayValue) + array.Elements.capacity() * sizeof(ElementValue));
	CHECK(array.Elements.capacity() == 67);
	array.Elements[0] = std::numeric_limits<double>::infinity();
	CHECK_FALSE(ValueClonePayloadBytes(array));
	ArrayValue nested{ValueType::Integer, {}};
	nested.Nested.emplace_back(std::initializer_list<ElementValue>{int64_t{7}, int64_t{9}});
	nested.Nested.reserve(17);
	nested.Nested[0].reserve(89);
	const auto retained = ValueClonePayloadBytes(nested);
	REQUIRE(retained);
	CHECK(
		*retained == sizeof(ArrayValue) + nested.Nested.capacity() * sizeof(std::vector<ElementValue>) +
						 nested.Nested[0].capacity() * sizeof(ElementValue)
	);
}
