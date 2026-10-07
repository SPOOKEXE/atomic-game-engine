#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_aniso_noise_batch")
using namespace engine::imagegraph;
using namespace complex_generator_test;
TEST_CASE("Aniso complete array work admission preserves caller output", "[imagegraph]") {
	auto d = Graph("pc.noise_aniso", {256, 256});
	Set(d, "render_mode", EnumValue{1});
	d.Nodes.push_back(Array("seeds", ValueType::Scalar, 17.0, 18.0));
	d.Links.push_back({"seeds", "array", "generator", "seed"});
	Refuse(d, Status::LimitExceeded, "surface_out");
}
TEST_CASE("Aniso later level failure preserves caller output", "[imagegraph]") {
	auto d = Graph("pc.noise_aniso", {2, 1});
	Set(d, "render_mode", EnumValue{1});
	d.Nodes.push_back(Array("levels", ValueType::Vector2, Vector2{0, 1}, Vector2{1, 1}));
	d.Links.push_back({"levels", "array", "generator", "level_in"});
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
