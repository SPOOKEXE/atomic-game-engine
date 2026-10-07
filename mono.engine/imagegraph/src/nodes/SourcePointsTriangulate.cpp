#include "SourcePointsTriangulate.hpp"

#include "SourcePolygon2D.hpp"

#include <cmath>
#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_points_triangulate {
		constexpr uint64_t WORK_LIMIT = 64 * 1024 * 1024;
		bool Number(const ElementValue &value, double &number) {
			if (const auto *scalar = std::get_if<double>(&value))
				number = *scalar;
			else if (const auto *integer = std::get_if<int64_t>(&value))
				number = double(*integer);
			else
				return false;
			return std::isfinite(number);
		}
		bool Point(const ElementValue &value, Vector2 &point) {
			if (const auto *vector = std::get_if<Vector2>(&value))
				point = *vector;
			else if (const auto *vector = std::get_if<Vector3>(&value))
				point = {vector->X, vector->Y};
			else
				return false;
			return std::isfinite(point.X) && std::isfinite(point.Y);
		}
		bool Row(const std::vector<ElementValue> &row, Vector2 &point) {
			return row.size() >= 2 && Number(row[0], point.X) && Number(row[1], point.Y);
		}
		bool Item(const SourceArrayItem &item, Vector2 &point) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) return Point(*leaf, point);
			const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
			if (!row || row->size() < 2) return false;
			const auto *x = std::get_if<ElementValue>(&(*row)[0].Data);
			const auto *y = std::get_if<ElementValue>(&(*row)[1].Data);
			return x && y && Number(*x, point.X) && Number(*y, point.Y);
		}
		bool Execute(NodeContext &context) try {
			ENGINE_PROFILE("imagegraph.source.points_triangulate");
			const Value *input = context.Find("points");
			const auto *array = input ? std::get_if<ArrayValue>(input) : nullptr;
			const bool single =
				!array || (array->Nested.empty() && array->Items.empty() && !array->Elements.empty() &&
						   (std::holds_alternative<double>(array->Elements.front()) ||
							std::holds_alternative<int64_t>(array->Elements.front())));
			const size_t count = single					  ? 1
								 : !array->Items.empty()  ? array->Items.size()
								 : !array->Nested.empty() ? array->Nested.size()
														  : array->Elements.size();
			if (count > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Triangulate point count exceeds bounds", "points"
				);
			// Each insertion scans triangles; removing each bad triangle scans them again.
			// Edge hash collisions may add another quadratic scan. Admit the cubic whole batch.
			const uint64_t topology = uint64_t(count) * 6 + 1;
			const uint64_t work = uint64_t(count) * topology * topology * 16;
			if (work > WORK_LIMIT / std::max(size_t{1}, context.ProcessorCount))
				return context.Fail(
					Status::LimitExceeded, "Triangulate whole batch work exceeds bounds", "points"
				);
			// Bounded helper topology, hash nodes and overlapping edge/bad-triangle buffers.
			auto scratch =
				context.ReserveWorkspace(uint64_t(count) * sizeof(Vector2) + topology * 2048 + 256, "points");
			if (!scratch) return false;
			std::vector<Vector2> points;
			points.reserve(count);
			for (size_t index = 0; index < count; ++index) {
				Vector2 point;
				bool valid = false;
				if (!array) {
					if (input) {
						if (const auto *vector = std::get_if<Vector2>(input)) {
							point = *vector;
							valid = std::isfinite(point.X) && std::isfinite(point.Y);
						}
					}
				} else if (single)
					valid = Row(array->Elements, point);
				else if (!array->Items.empty())
					valid = Item(array->Items[index], point);
				else if (!array->Nested.empty())
					valid = Row(array->Nested[index], point);
				else
					valid = Point(array->Elements[index], point);
				if (!valid)
					return context.Fail(
						Status::InvalidValue, "Triangulate requires finite coordinate rows", "points"
					);
				points.push_back(point);
			}
			std::vector<std::array<uint32_t, 3>> triangles;
			if (!polygon2d::Delaunay(points, triangles, {}, false, size_t(topology)))
				return context.Fail(
					Status::LimitExceeded, "Triangulate source topology exceeds native bounds", "points"
				);
			if (triangles.size() > Limits::MaximumArrayElements / 13)
				return context.Fail(
					Status::LimitExceeded, "Triangulate output shape exceeds bounds", "triangles"
				);
			const uint64_t outputBytes = sizeof(ArrayValue) + triangles.size() * 13 * sizeof(SourceArrayItem);
			auto staging = context.ReserveWorkspace(outputBytes, "triangles");
			if (!staging) return false;
			ArrayValue output{ValueType::Any, {}};
			output.Items.reserve(triangles.size());
			for (const auto &triangle : triangles) {
				std::vector<SourceArrayItem> corners;
				corners.reserve(3);
				for (uint32_t index : triangle) {
					const auto &point = points[index];
					std::vector<SourceArrayItem> coordinates;
					coordinates.reserve(3);
					coordinates.push_back({ElementValue{point.X}});
					coordinates.push_back({ElementValue{point.Y}});
					coordinates.push_back({ElementValue{1.}});
					corners.push_back({std::move(coordinates)});
				}
				output.Items.push_back({std::move(corners)});
			}
			if (!context.ReserveOutput(RetainedPayloadBytes(output), "triangles")) return false;
			if (!context.SetOutputDomain(
					"triangles", {ValueType::Scalar, SourceValueDisplay::Vector, SourceSocketKind::Float}
				))
				return false;
			context.SetValue("triangles", std::move(output));
			return context.FailureCode == Status::Ok;
		} catch (const std::bad_alloc &) {
			return context.Fail(Status::LimitExceeded, "Triangulate allocation failed", "points");
		} catch (const std::length_error &) {
			return context.Fail(Status::LimitExceeded, "Triangulate allocation length exceeded", "points");
		}
	}
	std::span<const ExecutorEntry> SourcePointsTriangulateExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.points_triangulate", source_points_triangulate::Execute, true}
		};
		return entries;
	}
}
