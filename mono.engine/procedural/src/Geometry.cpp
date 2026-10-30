#include <engine/procedural/Geometry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::procedural {

	namespace {
		constexpr int64_t MAXIMUM_ROAD_EXTENT = 64 * SUBCELLS_PER_CELL;
		constexpr uint32_t ROAD_SEGMENTS = 16;

		std::array<float, 4> GeometryColour(std::string_view kind, uint16_t material, uint16_t variant) {
			// Palette v1 quantizes in integer colour codes before float conversion,
			// so live and baked adapters never select different material colours.
			std::array<int, 3> rgb{90, 68, 42};
			if (kind == "tree")
				rgb = {36, 104, 44};
			else if (kind == "house")
				rgb = {175, 135, 83};
			else if (kind == "entrance")
				rgb = {224, 178, 47};
			else if (kind == "road")
				rgb = {118, 104, 78};
			const int variation =
				static_cast<int>((static_cast<uint32_t>(material) * 13u + variant * 7u) % 17u) - 8;
			return {
				static_cast<float>(rgb[0] + variation) / 255.0f,
				static_cast<float>(rgb[1] + variation) / 255.0f,
				static_cast<float>(rgb[2] + variation) / 255.0f,
				1.0f
			};
		}

		struct Point3 {
			double X = 0;
			double Y = 0;
			double Z = 0;
		};

		struct SourceRow {
			std::string_view Id{};
			const Placement *Placed = nullptr;
			const RoadSpline *Road = nullptr;
			uint32_t Vertices = 0;
			uint32_t Indices = 0;
		};

		Status Refuse(Diagnostic &diagnostic, Status status, const char *field) {
			diagnostic = {status, field};
			return status;
		}

		bool Contains(Bounds bounds, Point point) {
			return point.X >= bounds.Min.X && point.X < bounds.Max.X && point.Y >= bounds.Min.Y &&
				   point.Y < bounds.Max.Y;
		}

		bool Near(Point point, Point origin) {
			return point.X >= origin.X - MAXIMUM_ROAD_EXTENT && point.X <= origin.X + MAXIMUM_ROAD_EXTENT &&
				   point.Y >= origin.Y - MAXIMUM_ROAD_EXTENT && point.Y <= origin.Y + MAXIMUM_ROAD_EXTENT;
		}

		bool Spend(size_t &used, size_t maximum, size_t amount) {
			if (used > maximum || amount > maximum - used) return false;
			used += amount;
			return true;
		}

		bool EmitVertex(GeometryObject &object, Point3 position, Point3 normal, std::array<float, 2> uv) {
			GeometryVertex vertex;
			const std::array<double, 3> p{position.X, position.Y, position.Z};
			const std::array<double, 3> n{normal.X, normal.Y, normal.Z};
			for (size_t axis = 0; axis < 3; ++axis) {
				if (!std::isfinite(p[axis]) || !std::isfinite(n[axis]) ||
					std::abs(p[axis]) > std::numeric_limits<float>::max() ||
					std::abs(n[axis]) > std::numeric_limits<float>::max())
					return false;
				vertex.Position[axis] = static_cast<float>(p[axis]);
				vertex.Normal[axis] = static_cast<float>(n[axis]);
			}
			if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return false;
			vertex.UV = uv;
			object.Vertices.push_back(vertex);
			return true;
		}

		Point3 Rotate(Point3 point, uint8_t turns) {
			switch (turns) {
			case 0:
				return point;
			case 1:
				return {point.Z, point.Y, -point.X};
			case 2:
				return {-point.X, point.Y, -point.Z};
			default:
				return {-point.Z, point.Y, point.X};
			}
		}

		bool Triangle(GeometryObject &object, Point3 a, Point3 b, Point3 c, uint8_t turns) {
			a = Rotate(a, turns);
			b = Rotate(b, turns);
			c = Rotate(c, turns);
			const Point3 ab{b.X - a.X, b.Y - a.Y, b.Z - a.Z};
			const Point3 ac{c.X - a.X, c.Y - a.Y, c.Z - a.Z};
			Point3 normal{ab.Y * ac.Z - ab.Z * ac.Y, ab.Z * ac.X - ab.X * ac.Z, ab.X * ac.Y - ab.Y * ac.X};
			const double magnitude =
				std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
			if (!(magnitude > 0) || !std::isfinite(magnitude)) return false;
			normal = {normal.X / magnitude, normal.Y / magnitude, normal.Z / magnitude};
			const uint32_t first = static_cast<uint32_t>(object.Vertices.size());
			if (!EmitVertex(object, a, normal, {0, 0}) || !EmitVertex(object, b, normal, {1, 0}) ||
				!EmitVertex(object, c, normal, {1, 1}))
				return false;
			object.Indices.insert(object.Indices.end(), {first, first + 1, first + 2});
			return true;
		}

		bool Quad(GeometryObject &object, Point3 a, Point3 b, Point3 c, Point3 d, uint8_t turns) {
			return Triangle(object, a, b, c, turns) && Triangle(object, a, c, d, turns);
		}

		bool
		Box(GeometryObject &object, double halfX, double halfZ, double bottom, double top, uint8_t turns) {
			const double x = halfX;
			const double z = halfZ;
			const double lo = bottom;
			const double hi = top;
			return Quad(object, {-x, hi, z}, {x, hi, z}, {x, hi, -z}, {-x, hi, -z}, turns) &&
				   Quad(object, {-x, lo, -z}, {x, lo, -z}, {x, lo, z}, {-x, lo, z}, turns) &&
				   Quad(object, {-x, lo, z}, {x, lo, z}, {x, hi, z}, {-x, hi, z}, turns) &&
				   Quad(object, {x, lo, -z}, {-x, lo, -z}, {-x, hi, -z}, {x, hi, -z}, turns) &&
				   Quad(object, {x, lo, z}, {x, lo, -z}, {x, hi, -z}, {x, hi, z}, turns) &&
				   Quad(object, {-x, lo, -z}, {-x, lo, z}, {-x, hi, z}, {-x, hi, -z}, turns);
		}

		bool BuildPlacement(const Placement &placement, GeometryObject &object) {
			const uint8_t turns = placement.RotationQuarterTurns;
			switch (placement.Kind) {
			case PlacementKind::Tree: {
				const double height = 1.8 + 0.25 * (placement.Variant % 4);
				const double canopy = 0.55 + 0.08 * (placement.Variant % 3);
				return Box(object, 0.12, 0.12, 0, height, turns) &&
					   Box(object, canopy, canopy, height * 0.65, height + canopy, turns);
			}
			case PlacementKind::VillageLot:
				return Quad(
					object,
					{-0.95, 0.01, 0.95},
					{0.95, 0.01, 0.95},
					{0.95, 0.01, -0.95},
					{-0.95, 0.01, -0.95},
					turns
				);
			case PlacementKind::House: {
				const double width = 0.72 + 0.08 * (placement.Variant % 3);
				const double depth = 0.72 + 0.06 * (placement.Variant % 2);
				const double wall = 1.3 + 0.2 * (placement.Variant % 3);
				const double ridge = wall + 0.55;
				return Box(object, width, depth, 0, wall, turns) &&
					   Quad(
						   object,
						   {width, wall, depth},
						   {width, wall, -depth},
						   {0, ridge, -depth},
						   {0, ridge, depth},
						   turns
					   ) &&
					   Quad(
						   object,
						   {-width, wall, -depth},
						   {-width, wall, depth},
						   {0, ridge, depth},
						   {0, ridge, -depth},
						   turns
					   ) &&
					   Triangle(
						   object, {-width, wall, depth}, {width, wall, depth}, {0, ridge, depth}, turns
					   ) &&
					   Triangle(
						   object, {width, wall, -depth}, {-width, wall, -depth}, {0, ridge, -depth}, turns
					   );
			}
			case PlacementKind::Entrance:
				return false;
			}
			return false;
		}

		bool BuildEntrance(GeometryObject &object, uint8_t turns) {
			// Posts are offset before the exact quarter-turn rotation.
			const auto post = [&](double side) {
				const size_t first = object.Vertices.size();
				if (!Box(object, 0.12, 0.12, 0, 2.0, 0)) return false;
				for (size_t index = first; index < object.Vertices.size(); ++index) {
					GeometryVertex &vertex = object.Vertices[index];
					const Point3 position = Rotate(
						{vertex.Position[0] + side * 0.7, vertex.Position[1], vertex.Position[2]}, turns
					);
					const Point3 normal =
						Rotate({vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]}, turns);
					vertex.Position = {
						static_cast<float>(position.X),
						static_cast<float>(position.Y),
						static_cast<float>(position.Z)
					};
					vertex.Normal = {
						static_cast<float>(normal.X),
						static_cast<float>(normal.Y),
						static_cast<float>(normal.Z)
					};
				}
				return true;
			};
			return post(-1) && post(1) && Box(object, 0.83, 0.14, 1.85, 2.12, turns);
		}

		bool BuildRoad(const RoadSpline &road, GeometryObject &object) {
			const Point start = road.Controls[0];
			const auto relative = [&](Point point) {
				return std::array<double, 2>{
					static_cast<double>(point.X - start.X) / SUBCELLS_PER_CELL,
					static_cast<double>(point.Y - start.Y) / SUBCELLS_PER_CELL
				};
			};
			const auto control = relative(road.Controls[1]);
			const auto end = relative(road.Controls[2]);
			for (uint32_t step = 0; step <= ROAD_SEGMENTS; ++step) {
				const double t = static_cast<double>(step) / ROAD_SEGMENTS;
				const double inverse = 1.0 - t;
				const double x = 2 * inverse * t * control[0] + t * t * end[0];
				const double z = 2 * inverse * t * control[1] + t * t * end[1];
				double dx = inverse * control[0] + t * (end[0] - control[0]);
				double dz = inverse * control[1] + t * (end[1] - control[1]);
				if (dx == 0 && dz == 0) {
					dx = end[0];
					dz = end[1];
				}
				const double length = std::sqrt(dx * dx + dz * dz);
				if (!(length > 0) || !std::isfinite(length)) return false;
				const double sideX = -0.45 * dz / length;
				const double sideZ = 0.45 * dx / length;
				if (!EmitVertex(
						object, {x + sideX, 0.02, z + sideZ}, {0, 1, 0}, {static_cast<float>(t), 0}
					) ||
					!EmitVertex(object, {x - sideX, 0.02, z - sideZ}, {0, 1, 0}, {static_cast<float>(t), 1}))
					return false;
				if (step == 0) continue;
				const uint32_t prior = 2 * (step - 1);
				const uint32_t next = 2 * step;
				object.Indices.insert(
					object.Indices.end(), {prior, next, prior + 1, prior + 1, next, next + 1}
				);
			}
			return true;
		}
	}

	std::array<float, 3> GeometryPositionRelativeTo(
		const GeometryObject &object, Point origin, const std::array<float, 3> &position
	) {
		return {
			static_cast<float>(
				static_cast<double>(object.Origin.X - origin.X) / SUBCELLS_PER_CELL + position[0]
			),
			position[1],
			static_cast<float>(
				static_cast<double>(object.Origin.Y - origin.Y) / SUBCELLS_PER_CELL + position[2]
			)
		};
	}

	std::array<float, 3> GeometryWorldCentre(Point origin, const std::array<float, 3> &centre) {
		return {
			static_cast<float>(static_cast<double>(origin.X) / SUBCELLS_PER_CELL + centre[0]),
			centre[1],
			static_cast<float>(static_cast<double>(origin.Y) / SUBCELLS_PER_CELL + centre[2])
		};
	}
	std::array<float, 3> GeometryWorldCentre(const GeometryObject &object) {
		return GeometryWorldCentre(object.Origin, object.Centre);
	}
	std::array<float, 3> GeometryWorldCentre(const RegionGeometry &region) {
		return GeometryWorldCentre(region.Origin, region.Centre);
	}

	Status BuildRegionGeometry(
		const RegionResult &result,
		RegionScope scope,
		RegionGeometry &out,
		Diagnostic &diagnostic,
		const GeometryLimits &limits
	) {
		if (limits.MaximumObjects > GeometryLimits::HARD_MAXIMUM_OBJECTS ||
			limits.MaximumVertices > GeometryLimits::HARD_MAXIMUM_VERTICES ||
			limits.MaximumIndices > GeometryLimits::HARD_MAXIMUM_INDICES ||
			limits.MaximumBytes > GeometryLimits::HARD_MAXIMUM_BYTES)
			return Refuse(diagnostic, Status::LimitExceeded, "geometry.limits");
		if (scope.CellsPerSide == 0 || scope.CellsPerSide > 64)
			return Refuse(diagnostic, Status::InvalidInput, "geometry.scope");
		if (result.Placements.size() > limits.MaximumObjects ||
			result.Roads.size() > limits.MaximumObjects - result.Placements.size())
			return Refuse(diagnostic, Status::LimitExceeded, "geometry.objects");
		const int64_t minX = static_cast<int64_t>(scope.X) * scope.CellsPerSide * SUBCELLS_PER_CELL;
		const int64_t minY = static_cast<int64_t>(scope.Y) * scope.CellsPerSide * SUBCELLS_PER_CELL;
		const int64_t side = static_cast<int64_t>(scope.CellsPerSide) * SUBCELLS_PER_CELL;
		const Bounds owner{{minX, minY}, {minX + side, minY + side}};
		std::vector<SourceRow> rows;
		size_t vertices = 0;
		size_t indices = 0;
		size_t bytes = 0;
		try {
			rows.reserve(result.Placements.size() + result.Roads.size());
			const auto append = [&](SourceRow row) {
				if (!ValidStableName(row.Id) || row.Id.size() > 128)
					return Refuse(diagnostic, Status::InvalidInput, "geometry.id");
				if (!Spend(vertices, limits.MaximumVertices, row.Vertices) ||
					!Spend(indices, limits.MaximumIndices, row.Indices) ||
					!Spend(
						bytes,
						limits.MaximumBytes,
						sizeof(GeometryObject) + row.Id.size() + 16 +
							static_cast<size_t>(row.Vertices) * sizeof(GeometryVertex) +
							static_cast<size_t>(row.Indices) * sizeof(uint32_t)
					))
					return Refuse(diagnostic, Status::LimitExceeded, "geometry.budget");
				rows.push_back(row);
				return Status::Ok;
			};
			for (const Placement &placement : result.Placements) {
				if (!Contains(owner, placement.Position) || placement.RotationQuarterTurns > 3)
					return Refuse(diagnostic, Status::InvalidInput, "geometry.placement");
				SourceRow row{placement.Id, &placement, nullptr, 0, 0};
				switch (placement.Kind) {
				case PlacementKind::Tree:
					row.Vertices = row.Indices = 72;
					break;
				case PlacementKind::VillageLot:
					row.Vertices = row.Indices = 6;
					break;
				case PlacementKind::House:
					row.Vertices = row.Indices = 54;
					break;
				case PlacementKind::Entrance:
					row.Vertices = row.Indices = 108;
					break;
				default:
					return Refuse(diagnostic, Status::InvalidInput, "geometry.kind");
				}
				const Status status = append(row);
				if (status != Status::Ok) return status;
			}
			for (const RoadSpline &road : result.Roads) {
				if (!Contains(owner, road.Controls[0]) || !Near(road.Controls[1], road.Controls[0]) ||
					!Near(road.Controls[2], road.Controls[0]) || road.Controls[0] == road.Controls[2])
					return Refuse(diagnostic, Status::InvalidInput, "geometry.road");
				const Status status = append({road.Id, nullptr, &road, 34, 96});
				if (status != Status::Ok) return status;
			}
			std::sort(rows.begin(), rows.end(), [](const SourceRow &a, const SourceRow &b) {
				return StableNameLess(a.Id, b.Id);
			});
			for (size_t index = 1; index < rows.size(); ++index)
				if (rows[index - 1].Id == rows[index].Id)
					return Refuse(diagnostic, Status::InvalidInput, "geometry.duplicate-id");
			RegionGeometry built;
			built.Origin = owner.Min;
			std::array<float, 3> minimum{};
			std::array<float, 3> maximum{};
			bool firstRegionVertex = true;
			built.Objects.reserve(rows.size());
			for (const SourceRow &row : rows) {
				GeometryObject object;
				object.Id = row.Id;
				object.Vertices.reserve(row.Vertices);
				object.Indices.reserve(row.Indices);
				if (row.Placed != nullptr) {
					const Placement &placement = *row.Placed;
					object.Origin = placement.Position;
					object.Material = placement.Material;
					object.Variant = placement.Variant;
					switch (placement.Kind) {
					case PlacementKind::Tree:
						object.Kind = "tree";
						break;
					case PlacementKind::VillageLot:
						object.Kind = "village-lot";
						break;
					case PlacementKind::House:
						object.Kind = "house";
						break;
					case PlacementKind::Entrance:
						object.Kind = "entrance";
						break;
					}
					const bool success = placement.Kind == PlacementKind::Entrance
											 ? BuildEntrance(object, placement.RotationQuarterTurns)
											 : BuildPlacement(placement, object);
					if (!success) return Refuse(diagnostic, Status::InvalidInput, "geometry.vertex");
				} else {
					object.Kind = "road";
					object.Origin = row.Road->Controls[0];
					if (!BuildRoad(*row.Road, object))
						return Refuse(diagnostic, Status::InvalidInput, "geometry.road-vertex");
				}
				if (object.Vertices.size() != row.Vertices || object.Indices.size() != row.Indices)
					return Refuse(diagnostic, Status::InvalidInput, "geometry.count");
				object.Colour = GeometryColour(object.Kind, object.Material, object.Variant);
				std::array<float, 3> objectMinimum = object.Vertices.front().Position;
				std::array<float, 3> objectMaximum = objectMinimum;
				for (const auto &vertex : object.Vertices) {
					const auto regional = GeometryPositionRelativeTo(object, built.Origin, vertex.Position);
					for (size_t axis = 0; axis < 3; ++axis) {
						objectMinimum[axis] = std::min(objectMinimum[axis], vertex.Position[axis]);
						objectMaximum[axis] = std::max(objectMaximum[axis], vertex.Position[axis]);
						minimum[axis] =
							firstRegionVertex ? regional[axis] : std::min(minimum[axis], regional[axis]);
						maximum[axis] =
							firstRegionVertex ? regional[axis] : std::max(maximum[axis], regional[axis]);
					}
					firstRegionVertex = false;
				}
				for (size_t axis = 0; axis < 3; ++axis) {
					object.Centre[axis] = (objectMinimum[axis] + objectMaximum[axis]) * 0.5f;
					object.Size[axis] = objectMaximum[axis] - objectMinimum[axis];
				}
				built.Objects.push_back(std::move(object));
			}
			for (size_t axis = 0; axis < 3; ++axis) {
				built.Centre[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
				built.Size[axis] = maximum[axis] - minimum[axis];
			}
			built.VertexCount = static_cast<uint32_t>(vertices);
			built.IndexCount = static_cast<uint32_t>(indices);
			out = std::move(built);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, Status::AllocationFailed, "geometry.storage");
		}
	}
}
