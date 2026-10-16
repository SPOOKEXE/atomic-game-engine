#include <engine/computegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.computegraph.document")

using namespace engine::computegraph;

TEST_CASE("compute element layouts have fixed scalar and vector strides", "[computegraph]") {
	CHECK(ElementStride(ElementType::U32) == 4);
	CHECK(ElementStride(ElementType::F32) == 4);
	CHECK(ElementStride(ElementType::F32x4) == 16);
	CHECK(ElementStride(ElementType::EngineInstance) == 0);
	CHECK(ElementStride(static_cast<ElementType>(255)) == 0);
	CHECK(std::string_view(Describe(ElementType::U32)) == "u32");
	CHECK(std::string_view(Describe(Status::OutputOverflow)) == "output capacity exceeded");
}
