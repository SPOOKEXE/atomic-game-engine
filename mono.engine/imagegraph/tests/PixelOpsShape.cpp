#include "../src/PixelOpsShape.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_shape")

using engine::imagegraph::detail::ShapeDistance;
using engine::imagegraph::detail::ShapeGeometry;
using engine::imagegraph::detail::ShapeKind;
using engine::imagegraph::detail::ShapeStatus;

TEST_CASE("Shape rectangle and ellipse match source signed distances", "[imagegraph]") {
	ShapeGeometry controls;
	double distance = 9.0;
	REQUIRE(ShapeDistance(controls, 4, 4, 0.5, 0.5, distance) == ShapeStatus::Ok);
	CHECK(distance == -0.5);
	REQUIRE(ShapeDistance(controls, 4, 4, 1.25, 0.5, distance) == ShapeStatus::Ok);
	CHECK(distance == 0.25);
	controls.Kind = ShapeKind::Ellipse;
	REQUIRE(ShapeDistance(controls, 4, 4, 0.5, 0.5, distance) == ShapeStatus::Ok);
	CHECK(distance == -1.0);
	REQUIRE(ShapeDistance(controls, 4, 4, 1.25, 0.5, distance) == ShapeStatus::Ok);
	CHECK(distance == 0.5);
}

TEST_CASE("Shape half uses pixel point and source rotation sign", "[imagegraph]") {
	ShapeGeometry controls;
	controls.Kind = ShapeKind::Half;
	controls.Point1 = {2.0, 2.0};
	double distance = 9.0;
	REQUIRE(ShapeDistance(controls, 4, 4, 0.5, 0.75, distance) == ShapeStatus::Ok);
	CHECK(distance == -0.25);
	controls.RotationRadians = std::acos(-1.0) / 2.0;
	REQUIRE(ShapeDistance(controls, 4, 4, 0.75, 0.5, distance) == ShapeStatus::Ok);
	CHECK(std::abs(distance + 0.25) < 1e-12);
}

TEST_CASE("Shape triangle signs interior and exterior from source SDF", "[imagegraph]") {
	ShapeGeometry controls;
	controls.Kind = ShapeKind::Triangle;
	controls.Point1 = {0.0, 0.0};
	controls.Point2 = {4.0, 0.0};
	controls.Point3 = {0.0, 4.0};
	double distance = 9.0;
	REQUIRE(ShapeDistance(controls, 4, 4, 0.25, 0.25, distance) == ShapeStatus::Ok);
	CHECK(distance == -0.5);
	REQUIRE(ShapeDistance(controls, 4, 4, 0.875, 0.875, distance) == ShapeStatus::Ok);
	CHECK(distance > 0.0);
}

TEST_CASE("Shape rejects undefined geometry without changing result", "[imagegraph]") {
	ShapeGeometry controls;
	double distance = 7.0;
	controls.HalfSize.X = 0.0;
	CHECK(ShapeDistance(controls, 4, 4, 0.5, 0.5, distance) == ShapeStatus::UndefinedDivision);
	CHECK(distance == 7.0);
	controls.HalfSize.X = 0.5;
	controls.Kind = ShapeKind::Triangle;
	controls.Point1 = {0.0, 0.0};
	controls.Point2 = {0.0, 0.0};
	controls.Point3 = {0.0, 4.0};
	CHECK(ShapeDistance(controls, 4, 4, 0.5, 0.5, distance) == ShapeStatus::UndefinedDivision);
	CHECK(distance == 7.0);
}
