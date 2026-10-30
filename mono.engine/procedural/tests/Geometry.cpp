#include <engine/procedural/Geometry.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

TEST_SUITE_ID("engine.procedural.geometry")
TEST_DEPENDS("engine.procedural.region")

using namespace engine::procedural;

namespace {
	constexpr int64_t CELL = SUBCELLS_PER_CELL;

	RegionResult Example() {
		RegionResult result;
		result.Placements = {
			{"tree/a", PlacementKind::Tree, "", {2 * CELL, 3 * CELL}, 0, 2, 3},
			{"lot/a", PlacementKind::VillageLot, "town", {4 * CELL, 3 * CELL}, 0, 0, 4},
			{"house/a", PlacementKind::House, "town", {5 * CELL, 3 * CELL}, 1, 1, 5},
			{"entrance/a", PlacementKind::Entrance, "gate", {6 * CELL, 3 * CELL}, 0, 0, 6}
		};
		result.Roads = {
			{"road/a", "gate", "town", {{{6 * CELL, 3 * CELL}, {8 * CELL, 4 * CELL}, {10 * CELL, 3 * CELL}}}}
		};
		return result;
	}

	const GeometryObject &Find(const RegionGeometry &geometry, std::string_view kind) {
		const auto found =
			std::find_if(geometry.Objects.begin(), geometry.Objects.end(), [&](const auto &object) {
				return object.Kind == kind;
			});
		REQUIRE(found != geometry.Objects.end());
		return *found;
	}

	float MaximumHeight(const GeometryObject &object) {
		float height = -1;
		for (const GeometryVertex &vertex : object.Vertices)
			height = std::max(height, vertex.Position[1]);
		return height;
	}
}

TEST_CASE("region geometry contains visible typed meshes with stable identities", "[procedural][geometry]") {
	const RegionResult result = Example();
	RegionGeometry geometry;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	REQUIRE(geometry.Objects.size() == 5);
	CHECK(geometry.VertexCount == 72 + 6 + 54 + 108 + 34);
	CHECK(geometry.IndexCount == 72 + 6 + 54 + 108 + 96);
	CHECK(geometry.Objects[0].Id == "entrance/a");
	CHECK(geometry.Objects[1].Id == "house/a");
	CHECK(geometry.Objects[2].Id == "lot/a");
	CHECK(geometry.Objects[3].Id == "road/a");
	CHECK(geometry.Objects[4].Id == "tree/a");
	const GeometryObject &tree = Find(geometry, "tree");
	const GeometryObject &house = Find(geometry, "house");
	const GeometryObject &lot = Find(geometry, "village-lot");
	const GeometryObject &entrance = Find(geometry, "entrance");
	CHECK(tree.Material == 3);
	CHECK(tree.Variant == 2);
	CHECK(tree.Colour != house.Colour);
	CHECK(tree.Colour[3] == 1.0f);
	for (const auto &object : geometry.Objects)
		for (const float channel : object.Colour)
			CHECK((channel >= 0.0f && channel <= 1.0f));
	CHECK((tree.Origin == Point{2 * CELL, 3 * CELL}));
	CHECK(MaximumHeight(tree) > 2.3f);
	CHECK(MaximumHeight(house) > 2.0f);
	CHECK(MaximumHeight(entrance) > 2.0f);
	CHECK(MaximumHeight(lot) == 0.01f);
	for (const GeometryVertex &vertex : entrance.Vertices)
		if (vertex.Position[1] < 1.8f) CHECK(std::abs(vertex.Position[0]) > 0.4f);
	for (const GeometryObject &object : geometry.Objects) {
		CHECK(object.Indices.size() % 3 == 0);
		for (uint32_t index : object.Indices)
			CHECK(index < object.Vertices.size());
		for (const GeometryVertex &vertex : object.Vertices) {
			for (float component : vertex.Position)
				CHECK(std::isfinite(component));
			for (float component : vertex.Normal)
				CHECK(std::isfinite(component));
			for (float component : vertex.UV)
				CHECK(std::isfinite(component));
		}
	}
}

TEST_CASE("road ribbon follows its quadratic controls and exact end centres", "[procedural][geometry]") {
	RegionGeometry geometry;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(Example(), {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	const GeometryObject &road = Find(geometry, "road");
	REQUIRE(road.Vertices.size() == 34);
	REQUIRE(road.Indices.size() == 96);
	CHECK((road.Origin == Point{6 * CELL, 3 * CELL}));
	const auto centre = [&](size_t step) {
		const auto &left = road.Vertices[2 * step].Position;
		const auto &right = road.Vertices[2 * step + 1].Position;
		return std::array<float, 2>{(left[0] + right[0]) * 0.5f, (left[2] + right[2]) * 0.5f};
	};
	CHECK(std::abs(centre(0)[0]) < 0.0001f);
	CHECK(std::abs(centre(0)[1]) < 0.0001f);
	CHECK(std::abs(centre(8)[0] - 2.0f) < 0.0001f);
	CHECK(std::abs(centre(8)[1] - 0.5f) < 0.0001f);
	CHECK(std::abs(centre(16)[0] - 4.0f) < 0.0001f);
	CHECK(std::abs(centre(16)[1]) < 0.0001f);
	CHECK(road.Vertices.front().Normal[1] == 1.0f);
}

TEST_CASE(
	"triangle winding agrees with outward normals for every generated shape", "[procedural][geometry]"
) {
	RegionGeometry geometry;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(Example(), {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	for (const GeometryObject &object : geometry.Objects) {
		for (size_t triangle = 0; triangle < object.Indices.size(); triangle += 3) {
			const auto &a = object.Vertices[object.Indices[triangle]].Position;
			const auto &b = object.Vertices[object.Indices[triangle + 1]].Position;
			const auto &c = object.Vertices[object.Indices[triangle + 2]].Position;
			const auto &normalA = object.Vertices[object.Indices[triangle]].Normal;
			const auto &normalB = object.Vertices[object.Indices[triangle + 1]].Normal;
			const auto &normalC = object.Vertices[object.Indices[triangle + 2]].Normal;
			const double abX = static_cast<double>(b[0]) - a[0];
			const double abY = static_cast<double>(b[1]) - a[1];
			const double abZ = static_cast<double>(b[2]) - a[2];
			const double acX = static_cast<double>(c[0]) - a[0];
			const double acY = static_cast<double>(c[1]) - a[1];
			const double acZ = static_cast<double>(c[2]) - a[2];
			const double dot = ((abY * acZ - abZ * acY) * (normalA[0] + normalB[0] + normalC[0]) +
								(abZ * acX - abX * acZ) * (normalA[1] + normalB[1] + normalC[1]) +
								(abX * acY - abY * acX) * (normalA[2] + normalB[2] + normalC[2])) /
							   3.0;
			CHECK(dot > 0.000001);
		}
	}
}

TEST_CASE("quarter turns rotate asymmetric house geometry exactly", "[procedural][geometry]") {
	RegionResult result;
	result.Placements = {{"house/a", PlacementKind::House, "town", {CELL, CELL}, 0, 1, 0}};
	RegionGeometry first;
	RegionGeometry second;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, first, diagnostic) == Status::Ok);
	result.Placements.front().RotationQuarterTurns = 1;
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, second, diagnostic) == Status::Ok);
	REQUIRE(first.Objects.front().Vertices.size() == second.Objects.front().Vertices.size());
	for (size_t index = 0; index < first.Objects.front().Vertices.size(); ++index) {
		const auto &before = first.Objects.front().Vertices[index].Position;
		const auto &after = second.Objects.front().Vertices[index].Position;
		CHECK(after[0] == before[2]);
		CHECK(after[1] == before[1]);
		CHECK(after[2] == -before[0]);
	}
}

TEST_CASE("entrance geometry rotates without changing its topology", "[procedural][geometry]") {
	RegionResult result;
	result.Placements = {{"entrance/a", PlacementKind::Entrance, "gate", {CELL, CELL}, 0, 0, 0}};
	RegionGeometry first;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, first, diagnostic) == Status::Ok);
	const GeometryObject reference = first.Objects.front();
	REQUIRE(reference.Vertices.size() == 108);
	REQUIRE(reference.Indices.size() == 108);

	for (uint8_t turn = 1; turn < 4; ++turn) {
		result.Placements.front().RotationQuarterTurns = turn;
		RegionGeometry rotated;
		REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, rotated, diagnostic) == Status::Ok);
		const GeometryObject &object = rotated.Objects.front();
		REQUIRE(object.Vertices.size() == reference.Vertices.size());
		CHECK(object.Indices == reference.Indices);
		for (size_t index = 0; index < reference.Vertices.size(); ++index) {
			const auto rotate = [turn](const std::array<float, 3> &v) {
				switch (turn) {
				case 1:
					return std::array<float, 3>{v[2], v[1], -v[0]};
				case 2:
					return std::array<float, 3>{-v[0], v[1], -v[2]};
				default:
					return std::array<float, 3>{-v[2], v[1], v[0]};
				}
			};
			CHECK(object.Vertices[index].Position == rotate(reference.Vertices[index].Position));
			CHECK(object.Vertices[index].Normal == rotate(reference.Vertices[index].Normal));
			CHECK(object.Vertices[index].UV == reference.Vertices[index].UV);
		}
	}

	GeometryLimits limits;
	limits.MaximumIndices = 107;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, first, diagnostic, limits) == Status::LimitExceeded);
	CHECK(first.Objects.front().Indices == reference.Indices);
	result.Placements.front().RotationQuarterTurns = 4;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, first, diagnostic) == Status::InvalidInput);
	CHECK(first.Objects.front().Vertices.size() == 108);
}

TEST_CASE("extreme region origins stay exact while hostile coordinates refuse", "[procedural][geometry]") {
	for (const int32_t coordinate :
		 {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}) {
		const RegionScope scope{coordinate, coordinate, 64};
		const int64_t start = static_cast<int64_t>(coordinate) * scope.CellsPerSide * CELL;
		RegionResult result;
		result.Placements = {{"tree/a", PlacementKind::Tree, "", {start + CELL, start + CELL}, 0, 0, 0}};
		RegionGeometry geometry;
		Diagnostic diagnostic;
		REQUIRE(BuildRegionGeometry(result, scope, geometry, diagnostic) == Status::Ok);
		REQUIRE(geometry.Objects.size() == 1);
		CHECK((geometry.Objects.front().Origin == Point{start + CELL, start + CELL}));
		result.Placements.front().Position = {std::numeric_limits<int64_t>::max(), 0};
		CHECK(BuildRegionGeometry(result, scope, geometry, diagnostic) == Status::InvalidInput);
		CHECK((geometry.Objects.front().Origin == Point{start + CELL, start + CELL}));
	}
}

TEST_CASE("invalid rows and exhausted budgets preserve the previous whole batch", "[procedural][geometry]") {
	RegionGeometry geometry;
	Diagnostic diagnostic;
	RegionResult result = Example();
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	const auto oldCount = geometry.VertexCount;
	const auto oldId = geometry.Objects.front().Id;
	GeometryLimits limits;
	limits.MaximumVertices = oldCount - 1;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.VertexCount == oldCount);
	CHECK(geometry.Objects.front().Id == oldId);
	limits = {};
	limits.MaximumBytes = 1;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.Objects.front().Id == oldId);
	result.Roads.front().Id = "tree/a";
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::InvalidInput);
	CHECK(geometry.Objects.front().Id == oldId);
	result.Roads.front().Id = "road/a";
	result.Roads.front().Controls[1].X = result.Roads.front().Controls[0].X + 65 * CELL;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::InvalidInput);
	CHECK(geometry.Objects.front().Id == oldId);
	result.Roads.front().Controls[1] = result.Roads.front().Controls[0];
	result.Roads.front().Controls[2] = result.Roads.front().Controls[0];
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::InvalidInput);
	CHECK(geometry.Objects.front().Id == oldId);
}

TEST_CASE("caller cannot raise the pure geometry hard ceilings", "[procedural][geometry]") {
	RegionGeometry geometry;
	Diagnostic diagnostic;
	const RegionResult result = Example();
	REQUIRE(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	const auto savedCount = geometry.VertexCount;
	const auto savedId = geometry.Objects.front().Id;
	GeometryLimits limits;
	++limits.MaximumObjects;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.VertexCount == savedCount);
	CHECK(geometry.Objects.front().Id == savedId);
	limits = {};
	++limits.MaximumVertices;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.VertexCount == savedCount);
	limits = {};
	++limits.MaximumIndices;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.VertexCount == savedCount);
	limits = {};
	++limits.MaximumBytes;
	CHECK(BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic, limits) == Status::LimitExceeded);
	CHECK(geometry.VertexCount == savedCount);
}

TEST_CASE("shared geometry bounds preserve object and aggregate placement", "[procedural][geometry]") {
	RegionGeometry geometry;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionGeometry(Example(), {0, 0, 16}, geometry, diagnostic) == Status::Ok);
	std::array<float, 3> minimum{};
	std::array<float, 3> maximum{};
	minimum.fill(std::numeric_limits<float>::infinity());
	maximum.fill(-std::numeric_limits<float>::infinity());
	for (const auto &object : geometry.Objects) {
		std::array<float, 3> localMinimum{};
		std::array<float, 3> localMaximum{};
		localMinimum.fill(std::numeric_limits<float>::infinity());
		localMaximum.fill(-std::numeric_limits<float>::infinity());
		for (const auto &vertex : object.Vertices) {
			const auto relative = GeometryPositionRelativeTo(object, geometry.Origin, vertex.Position);
			for (size_t axis = 0; axis < 3; ++axis) {
				localMinimum[axis] = std::min(localMinimum[axis], vertex.Position[axis]);
				localMaximum[axis] = std::max(localMaximum[axis], vertex.Position[axis]);
				minimum[axis] = std::min(minimum[axis], relative[axis]);
				maximum[axis] = std::max(maximum[axis], relative[axis]);
			}
		}
		for (size_t axis = 0; axis < 3; ++axis) {
			CHECK(object.Centre[axis] == (localMinimum[axis] + localMaximum[axis]) * 0.5f);
			CHECK(object.Size[axis] == localMaximum[axis] - localMinimum[axis]);
		}
		CHECK(GeometryWorldCentre(object) == GeometryPositionRelativeTo(object, {}, object.Centre));
	}
	for (size_t axis = 0; axis < 3; ++axis) {
		CHECK(geometry.Centre[axis] == (minimum[axis] + maximum[axis]) * 0.5f);
		CHECK(geometry.Size[axis] == maximum[axis] - minimum[axis]);
	}
	CHECK(Find(geometry, "village-lot").Size[1] == 0.0f);
	RegionGeometry empty;
	REQUIRE(BuildRegionGeometry({}, {-2, 3, 16}, empty, diagnostic) == Status::Ok);
	CHECK((empty.Origin == Point{-32 * CELL, 48 * CELL}));
	CHECK((empty.Size == std::array<float, 3>{}));
	CHECK((GeometryWorldCentre(empty) == std::array<float, 3>{-32, 0, 48}));
}
