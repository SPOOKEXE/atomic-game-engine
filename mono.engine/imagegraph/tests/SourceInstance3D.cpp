#include <engine/imagegraph/SourceInstance3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <numbers>
TEST_SUITE_ID("engine.imagegraph.source_instance_3d")
using namespace engine::imagegraph;
TEST_CASE(
	"Source instance position uses Euler look scale translation and leaves normal look independent",
	"[imagegraph][source_instance_3d]"
) {
	MeshInstance3D instance;
	instance.Fields[0] = 10;
	instance.Fields[1] = 20;
	instance.Fields[2] = 30;
	instance.Fields[8] = 2;
	instance.Fields[9] = 3;
	instance.Fields[10] = 4;
	instance.Fields[13] = 1;
	CHECK((SourceInstancePosition3D(instance, {1, 2, 3}) == Vector3{8, 29, 38}));
	CHECK((SourceInstanceNormal3D(instance, {1, 2, 3}) == Vector3{1, 2, 3}));
	instance.Fields[12] = instance.Fields[13] = 0;
	instance.Fields[14] = 1;
	CHECK((SourceInstancePosition3D(instance, {1, 2, 3}) == Vector3{12, 26, 42}));
	instance.Fields[6] = float(std::numbers::pi / 2);
	const auto normal = SourceInstanceNormal3D(instance, {1, 0, 0});
	CHECK(std::abs(normal.X) < 1e-6);
	CHECK(std::abs(normal.Y + 1) < 1e-6);
}
