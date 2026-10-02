#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_matrix_nodes")
using namespace engine::imagegraph;
TEST_CASE(
	"Source matrix projection retains left-handed clipping and dimension aspect",
	"[imagegraph][source_matrix]"
) {
	const auto ortho = imagegraph_test::RunNode(
		"pc.matrix_projection",
		{},
		{{"view", Vector2{8, 4}},
		 {"view_unit", EnumValue{0}},
		 {"orthographic_scale", .25},
		 {"clipping", Vector2{2, 6}}}
	);
	INFO(ortho.Message);
	REQUIRE(ortho.Ok);
	const auto &m = std::get<MatrixValue>(*ortho.OutputValue("matrix"));
	REQUIRE(m.Values.size() == 16);
	CHECK(m.Values[0] == .5);
	CHECK(m.Values[5] == 1);
	CHECK(m.Values[10] == .25);
	CHECK(m.Values[14] == -.5);
	CHECK(m.Values[15] == 1);
	const auto perspective = imagegraph_test::RunNode(
		"pc.matrix_projection",
		{},
		{{"projection", EnumValue{0}},
		 {"view", Vector2{8, 4}},
		 {"view_unit", EnumValue{0}},
		 {"fov", int64_t{90}},
		 {"clipping", Vector2{2, 6}}}
	);
	REQUIRE(perspective.Ok);
	const auto &p = std::get<MatrixValue>(*perspective.OutputValue("matrix"));
	CHECK(p.Values[0] == Catch::Approx(.5));
	CHECK(p.Values[5] == Catch::Approx(1));
	CHECK(p.Values[10] == 1.5);
	CHECK(p.Values[11] == 1);
	CHECK(p.Values[14] == -3);
	const auto degenerate =
		imagegraph_test::RunNode("pc.matrix_projection", {}, {{"clipping", Vector2{2, 2}}});
	REQUIRE(degenerate.Ok);
	const auto &d = std::get<MatrixValue>(*degenerate.OutputValue("matrix"));
	CHECK(d.Values[0] == 1);
	CHECK(d.Values[5] == 1);
	CHECK(d.Values[10] == 1);
	CHECK(d.Values[15] == 1);
}
TEST_CASE(
	"Source 3D transform matrix affine flag controls translation and homogeneous entry",
	"[imagegraph][source_matrix]"
) {
	for (bool affine : {false, true}) {
		const auto run = imagegraph_test::RunNode(
			"pc.matrix_transform_3_d",
			{},
			{{"affine", affine}, {"position", Vector3{2, 3, 4}}, {"scale", Vector3{5, 6, 7}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto &m = std::get<MatrixValue>(*run.OutputValue("matrix"));
		CHECK(m.Columns == 4);
		CHECK(m.Rows == 4);
		CHECK(m.Values[0] == 5);
		CHECK(m.Values[5] == 6);
		CHECK(m.Values[10] == 7);
		CHECK(m.Values[12] == (affine ? 2 : 0));
		CHECK(m.Values[13] == (affine ? 3 : 0));
		CHECK(m.Values[14] == (affine ? 4 : 0));
		CHECK(m.Values[15] == (affine ? 1 : 0));
	}
}
TEST_CASE(
	"Source eigen preserves break-before-assignment and rejects undefined zero normalization",
	"[imagegraph][source_matrix]"
) {
	const auto scalar = imagegraph_test::RunNode("pc.matrix_eigen", {}, {{"matrix", MatrixValue{1, 1, {7}}}});
	INFO(scalar.Message);
	REQUIRE(scalar.Ok);
	const auto &vector = std::get<ArrayValue>(*scalar.OutputValue("eigenvector"));
	REQUIRE(vector.Nested.size() == 1);
	CHECK(std::get<double>(vector.Nested[0][0]) == 1);
	const auto &values = std::get<ArrayValue>(*scalar.OutputValue("eigenvalue"));
	CHECK(std::get<double>(values.Elements[0]) == 7);
	const auto zero = imagegraph_test::RunNode("pc.matrix_eigen", {}, {{"matrix", MatrixValue{1, 1, {0}}}});
	CHECK_FALSE(zero.Ok);
	CHECK(zero.Code == Status::InvalidValue);
	const auto nonsquare =
		imagegraph_test::RunNode("pc.matrix_eigen", {}, {{"matrix", MatrixValue{1, 2, {1, 2}}}});
	CHECK_FALSE(nonsquare.Ok);
	CHECK(nonsquare.Code == Status::InvalidValue);
}
