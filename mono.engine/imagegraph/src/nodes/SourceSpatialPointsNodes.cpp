#include "../SourceRandom.hpp"
#include "Families.hpp"
#include "Processor.hpp"

#include <engine/imagegraph/SourceCamera3D.hpp>

namespace engine::imagegraph::detail {
	namespace {
		bool SpatialNumber(const ElementValue &value, double &number) {
			if (const auto *v = std::get_if<double>(&value))
				number = *v;
			else if (const auto *v = std::get_if<int64_t>(&value))
				number = double(*v);
			else if (const auto *v = std::get_if<bool>(&value))
				number = *v ? 1 : 0;
			else if (const auto *v = std::get_if<EnumValue>(&value))
				number = double(v->Value);
			else
				return false;
			return std::isfinite(number);
		}
		bool SpatialFinite(Vector3 v) {
			return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
		}
		Vector3 SpatialControl(const NodeContext &context, std::string_view id, Vector3 fallback = {}) {
			if (const auto *v = context.Find(id))
				if (const auto *p = std::get_if<Vector3>(v)) return *p;
			return fallback;
		}
		// Node_Scatter_Points_3D uses a normalized cube direction and sqrt radius for spheres.
		bool SpatialScatter(NodeContext &context) {
			const int64_t count = context.Integer("amount", 8);
			if (count < 0 || uint64_t(count) > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "3D scatter amount exceeds bounded array limits", "amount"
				);
			const Vector3 center = SpatialControl(context, "center"),
						  half = SpatialControl(context, "half_size", {1, 1, 1});
			const double seed = context.Scalar("seed");
			if (!SpatialFinite(center) || !SpatialFinite(half) || !std::isfinite(seed))
				return context.Fail(Status::InvalidValue, "3D scatter controls must be finite");
			const int64_t shape = context.Integer("shape");
			// random_set_seed performs JavaScript's signed 32-bit argument coercion.
			double wrapped = std::fmod(std::trunc(seed), 4294967296.0);
			if (wrapped < 0) wrapped += 4294967296.0;
			SourceRandom random{uint32_t(wrapped)};
			if (!context.ReserveOutput(
					uint64_t(count) * sizeof(ElementValue) + std::string{}.capacity(), "points"
				))
				return false;
			ArrayValue output;
			output.ElementType = ValueType::Vector3;
			output.Elements.reserve(size_t(count));
			for (int64_t i = 0; i < count; i++) {
				Vector3 point{};
				if (shape == 0) {
					point = {
						random.Range(center.X - half.X, center.X + half.X),
						random.Range(center.Y - half.Y, center.Y + half.Y),
						random.Range(center.Z - half.Z, center.Z + half.Z)
					};
				} else if (shape == 1) {
					double x = random.Range(-1, 1), y = random.Range(-1, 1), z = random.Range(-1, 1);
					const double length = std::sqrt(x * x + y * y + z * z);
					if (length != 0) {
						x /= length;
						y /= length;
						z /= length;
						const double distance = std::sqrt(random.Unit());
						point = {
							center.X + x * distance * half.X,
							center.Y + y * distance * half.Y,
							center.Z + z * distance * half.Z
						};
					}
				}
				if (!SpatialFinite(point))
					return context.Fail(
						Status::InvalidValue, "3D scatter produced nonfinite coordinates", "points"
					);
				output.Elements.emplace_back(point);
			}
			context.SetValue("points", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		// Packed vectors and explicit source coordinate rows are both source arrays.
		bool SpatialPoint(const ArrayValue &array, size_t index, Vector3 &point, bool &skip) {
			skip = false;
			if (!array.Items.empty()) {
				const auto &item = array.Items[index];
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (const auto *vector = std::get_if<Vector3>(leaf)) {
						point = *vector;
						return SpatialFinite(point);
					}
					skip = true;
					return true;
				}
				const auto &row = std::get<std::vector<SourceArrayItem>>(item.Data);
				if (row.size() < 3) return false;
				const auto *x = std::get_if<ElementValue>(&row[0].Data),
						   *y = std::get_if<ElementValue>(&row[1].Data),
						   *z = std::get_if<ElementValue>(&row[2].Data);
				return x && y && z && SpatialNumber(*x, point.X) && SpatialNumber(*y, point.Y) &&
					   SpatialNumber(*z, point.Z);
			}
			if (!array.Nested.empty()) {
				const auto &row = array.Nested[index];
				return row.size() >= 3 && SpatialNumber(row[0], point.X) && SpatialNumber(row[1], point.Y) &&
					   SpatialNumber(row[2], point.Z);
			}
			if (const auto *vector = std::get_if<Vector3>(&array.Elements[index])) {
				point = *vector;
				return SpatialFinite(point);
			}
			skip = true;
			return true;
		}
		std::array<double, 4>
		SpatialMultiply(const std::array<double, 16> &matrix, const std::array<double, 4> &point) {
			std::array<double, 4> result{};
			for (size_t row = 0; row < 4; row++)
				for (size_t column = 0; column < 4; column++)
					result[row] += matrix[column * 4 + row] * point[column];
			return result;
		}
		Vector2 SpatialRange(
			const NodeContext &context, std::string_view id, Vector2 fallback, uint32_t width, uint32_t height
		) {
			Vector2 value = context.Vec2(id, fallback);
			if (context.Integer(std::string(id) + "_unit", 0) == 1) {
				value.X *= width;
				value.Y *= height;
			}
			return value;
		}
		// Node_Point_3D_Camera projects to camera view coordinates before its range remap.
		bool SpatialPointCamera(NodeContext &context) {
			const Value *input = context.Find("points");
			const auto *points = input ? std::get_if<ArrayValue>(input) : nullptr;
			if (!points || !ValidRuntimeValue(*input))
				return context.Fail(
					Status::TypeMismatch, "3D point camera requires an owned array of points", "points"
				);
			const size_t count = !points->Items.empty()	   ? points->Items.size()
								 : !points->Nested.empty() ? points->Nested.size()
														   : points->Elements.size();
			uint32_t width = 0, height = 0;
			if (!ResolveDimension(context, "dimension", width, height)) return false;
			const Vector2 rangeFrom = SpatialRange(context, "range_from", {-1, 1}, width, height),
						  rangeTo = SpatialRange(context, "range_to", {0, 1}, width, height);
			const Vector2 depthFrom = SpatialRange(context, "depth_from", {0, 1}, width, height),
						  depthTo = SpatialRange(context, "depth_to", {0, 1}, width, height);
			if (!std::isfinite(rangeFrom.X) || !std::isfinite(rangeFrom.Y) || !std::isfinite(rangeTo.X) ||
				!std::isfinite(rangeTo.Y) || !std::isfinite(depthFrom.X) || !std::isfinite(depthFrom.Y) ||
				!std::isfinite(depthTo.X) || !std::isfinite(depthTo.Y) || rangeFrom.X == rangeFrom.Y ||
				depthFrom.X == depthFrom.Y)
				return context.Fail(
					Status::InvalidValue, "point camera remap ranges must be finite and distinct"
				);
			auto workspace = context.ReserveWorkspace(4096, "points");
			if (!workspace) return false;
			constexpr std::array<std::string_view, 10> controls{
				"position",
				"rotation",
				"postioning_mode",
				"lookat_position",
				"horizontal_angle",
				"vertical_angle",
				"distance",
				"projection",
				"fov",
				"orthographic_scale"
			};
			std::array<EvaluationInputValue, 11> values{};
			size_t valueCount = 0;
			for (const auto id : controls)
				if (const Value *value = context.Find(id))
					values[valueCount++] = {std::string(id), *value, false, {}};
			values[valueCount++] = {"clipping_distance", depthFrom, false, {}};
			SourceCameraPose pose;
			Diagnostic diagnostic;
			if (ResolveSourceCameraPose(
					std::span(values).first(valueCount), width, height, pose, diagnostic
				) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, diagnostic.Port);
			if (pose.Projection == SourceCameraProjection::Custom)
				return context.Fail(
					Status::UnsupportedExecution,
					"point camera has no custom projection source case",
					"projection"
				);
			// The point camera uses width/height here; the surface camera uses
			// height/width.
			if (pose.Projection == SourceCameraProjection::Orthographic) {
				const double scale = context.Scalar("orthographic_scale", .5);
				pose.OrthographicViewSize = {1 / scale, double(width) / height / scale};
			}
			std::array<double, 16> view{}, projection{};
			if (ResolveSourceCameraMatrices(pose, width, height, view, projection, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, diagnostic.Port);
			const double viewWidth =
				pose.Projection == SourceCameraProjection::Orthographic ? pose.OrthographicViewSize.X : width;
			const double viewHeight = pose.Projection == SourceCameraProjection::Orthographic
										  ? pose.OrthographicViewSize.Y
										  : height;
			bool hasSkippedRow = false;
			for (size_t i = 0; i < count; ++i) {
				Vector3 point;
				bool skip = false;
				if (!SpatialPoint(*points, i, point, skip))
					return context.Fail(
						Status::InvalidValue, "point camera needs three finite coordinates per row", "points"
					);
				hasSkippedRow |= skip;
			}
			const uint64_t rowBytes = hasSkippedRow ? sizeof(SourceArrayItem) : sizeof(ElementValue);
			if (!context.ReserveOutput(count * rowBytes + std::string{}.capacity(), "points")) return false;
			ArrayValue output;
			output.ElementType = hasSkippedRow ? ValueType::Any : ValueType::Vector3;
			if (hasSkippedRow)
				output.Items.reserve(count);
			else
				output.Elements.reserve(count);
			for (size_t i = 0; i < count; i++) {
				Vector3 point;
				bool skip = false;
				if (!SpatialPoint(*points, i, point, skip))
					return context.Fail(
						Status::InvalidValue, "point camera needs three finite coordinates per row", "points"
					);
				if (skip) {
					output.Items.push_back({ElementValue{UndefinedValue{}}});
					continue;
				}
				auto projected =
					SpatialMultiply(projection, SpatialMultiply(view, {point.X, point.Y, point.Z, 1}));
				if (projected[3] == 0)
					return context.Fail(
						Status::InvalidValue, "point camera perspective divide is zero", "points"
					);
				const double divisor = projected[3];
				for (double &value : projected)
					value /= divisor;
				const double x = viewWidth / 2 + projected[0] * viewWidth / 2,
							 y = viewHeight / 2 + projected[1] * viewHeight / 2;
				Vector3 result{
					rangeTo.X + (rangeTo.Y - rangeTo.X) * ((x - rangeFrom.X) / (rangeFrom.Y - rangeFrom.X)),
					rangeTo.X + (rangeTo.Y - rangeTo.X) * ((y - rangeFrom.X) / (rangeFrom.Y - rangeFrom.X)),
					depthTo.X +
						(depthTo.Y - depthTo.X) * ((projected[2] - depthFrom.X) / (depthFrom.Y - depthFrom.X))
				};
				if (!SpatialFinite(result))
					return context.Fail(
						Status::InvalidValue, "point camera projected coordinates are nonfinite", "points"
					);
				if (hasSkippedRow)
					output.Items.push_back({ElementValue{result}});
				else
					output.Elements.emplace_back(result);
			}
			context.SetValue("points", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceSpatialPointsExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.scatter_points_3_d", SpatialScatter, true},
			ExecutorEntry{"pc.point_3_d_camera", SpatialPointCamera, true}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
