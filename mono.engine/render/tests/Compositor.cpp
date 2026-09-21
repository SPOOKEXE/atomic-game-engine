// CPU-side packing shared by the built-in compositor shader tail.

#include "Compositor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.compositor")

using Catch::Approx;
using engine::core::Name;
using engine::graph::Node;
using engine::render::CompositorParametersFor;

TEST_CASE("compositor settings pack into stable shader lanes", "[render][compositor]") {
	Node grade;
	grade.Kind = Name("exposure-grade");
	grade.Parameters = {
		{Name("exposure"), "1.5"},
		{Name("contrast"), "1.25"},
		{Name("pivot"), "0.2"},
		{Name("gamma"), "2.2"},
	};
	const auto gradeValues = CompositorParametersFor(grade);
	CHECK(gradeValues[0].x == 1.5f);
	CHECK(gradeValues[0].y == 1.25f);
	CHECK(gradeValues[0].z == 0.2f);
	CHECK(gradeValues[0].w == 2.2f);

	Node transform;
	transform.Kind = Name("transform-crop");
	transform.Parameters = {
		{Name("scale-x"), "2"},
		{Name("translate-y"), "-0.25"},
		{Name("rotation"), "90"},
		{Name("crop-right"), "0.8"},
		{Name("extend"), "mirror"},
	};
	const auto transformValues = CompositorParametersFor(transform);
	CHECK(transformValues[0].x == 2.0f);
	CHECK(transformValues[0].w == -0.25f);
	CHECK(transformValues[1].x == Approx(1.5707963f));
	CHECK(transformValues[1].w == 0.8f);
	CHECK(transformValues[2].y == 3.0f);
}

TEST_CASE("compositor settings reject unknown choices and bound shader work", "[render][compositor]") {
	Node mix;
	mix.Kind = Name("mix");
	mix.Parameters = {
		{Name("operation"), "unknown"},
		{Name("factor"), "4"},
		{Name("clamp"), "unit"},
	};
	const auto mixValues = CompositorParametersFor(mix);
	CHECK(mixValues[0].x == 7.0f);
	CHECK(mixValues[0].y == 1.0f);
	CHECK(mixValues[0].z == 2.0f);

	Node blur;
	blur.Kind = Name("blur");
	blur.Parameters = {
		{Name("kernel"), "box"},
		{Name("radius"), "500"},
		{Name("sigma"), "nan"},
	};
	const auto blurValues = CompositorParametersFor(blur);
	CHECK(blurValues[0].x == 1.0f);
	CHECK(blurValues[0].y == 32.0f);
	CHECK(blurValues[0].z == 2.0f);
}
