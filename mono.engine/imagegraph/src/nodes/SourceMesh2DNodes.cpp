#include "../Mesh2DPayload.hpp"
#include "Families.hpp"
#include "Path.hpp"
#include "SourcePolygon2D.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace engine::imagegraph::detail {
	bool SourceShapePolygon(NodeContext &context);
	bool SourceMeshWarp(NodeContext &context);
	namespace {
		void Recalculate(MeshData2D &mesh) {
			mesh.Center = {};
			if (mesh.Triangles.empty()) return;
			mesh.Bounds = {INFINITY, INFINITY, -INFINITY, -INFINITY};
			for (const auto &triangle : mesh.Triangles)
				for (uint32_t index : triangle) {
					const auto point = mesh.Simulation.Points[index].Position;
					mesh.Center.X += point.X;
					mesh.Center.Y += point.Y;
					mesh.Bounds[0] = std::min(mesh.Bounds[0], point.X);
					mesh.Bounds[1] = std::min(mesh.Bounds[1], point.Y);
					mesh.Bounds[2] = std::max(mesh.Bounds[2], point.X);
					mesh.Bounds[3] = std::max(mesh.Bounds[3], point.Y);
				}
			mesh.Center.X /= mesh.Triangles.size() * 3;
			mesh.Center.Y /= mesh.Triangles.size() * 3;
		}
		bool Publish(NodeContext &context, MeshValue2D mesh) {
			if (!ValidMesh2DPayload(mesh))
				return context.Fail(Status::InvalidValue, "2D mesh payload is invalid", "mesh");
			context.SetValue("mesh", std::move(mesh));
			return context.FailureCode == Status::Ok;
		}
		bool Lattice(NodeContext &context) {
			Area area = context.Get<Area>("area", {});
			if (!context.IsLinked("area") && context.Integer("area_unit", 1) == 1) {
				area.CenterX *= context.Project.SurfaceWidth;
				area.HalfWidth *= context.Project.SurfaceWidth;
				area.CenterY *= context.Project.SurfaceHeight;
				area.HalfHeight *= context.Project.SurfaceHeight;
			}
			const auto samples = context.Get<Vector2>("sample", {8, 8});
			if (!MeshFinite(samples))
				return context.Fail(Status::InvalidValue, "mesh samples must be finite", "sample");
			const double sx = std::max(1.0, std::trunc(samples.X)), sy = std::max(1.0, std::trunc(samples.Y));
			if (sx > Limits::MaximumArrayElements || sy > Limits::MaximumArrayElements ||
				(sx + 1) * (sy + 1) > Limits::MaximumArrayElements || 2 * sx * sy > Limits::MaximumLinks ||
				(sx + 1) * sy + (sy + 1) * sx > Limits::MaximumLinks)
				return context.Fail(Status::LimitExceeded, "mesh lattice exceeds topology limits", "sample");
			const uint32_t width = static_cast<uint32_t>(sx), height = static_cast<uint32_t>(sy);
			const bool quads = context.Boolean("quad");
			if (!context.ReserveOutput(
					sizeof(MeshData2D) + (width + 1) * (height + 1) * sizeof(VerletPoint) +
						((width + 1) * height + (height + 1) * width) * sizeof(VerletEdge) +
						2 * width * height * sizeof(std::array<uint32_t, 3>) +
						(quads ? width * height * sizeof(std::array<uint32_t, 2>) : 0),
					"mesh"
				))
				return false;
			MeshValue2D result;
			auto &mesh = result.Data.emplace();
			mesh.Bounds = {0, 0, 1, 1};
			mesh.Simulation.Points.reserve((width + 1) * (height + 1));
			mesh.Simulation.Edges.reserve((width + 1) * height + (height + 1) * width);
			mesh.Triangles.reserve(width * height * 2);
			if (quads) mesh.Quads.reserve(width * height);
			for (uint32_t y = 0; y <= height; ++y)
				for (uint32_t x = 0; x <= width; ++x) {
					const double u = double(x) / width, v = double(y) / height;
					VerletPoint point;
					point.Position = {
						area.CenterX - area.HalfWidth + 2 * area.HalfWidth * u,
						area.CenterY - area.HalfHeight + 2 * area.HalfHeight * v
					};
					point.UV = {u, v};
					mesh.Simulation.Points.push_back(point);
				}
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const uint32_t p0 = y * (width + 1) + x, p1 = p0 + 1, p2 = p0 + width + 1, p3 = p2 + 1;
					mesh.Triangles.push_back({p0, p1, p2});
					mesh.Triangles.push_back({p2, p1, p3});
					if (quads)
						mesh.Quads.push_back(
							{uint32_t(mesh.Triangles.size() - 2), uint32_t(mesh.Triangles.size() - 1)}
						);
				}
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x <= width; ++x)
					mesh.Simulation.Edges.push_back({y * (width + 1) + x, (y + 1) * (width + 1) + x});
			for (uint32_t y = 0; y <= height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					mesh.Simulation.Edges.push_back({y * (width + 1) + x, y * (width + 1) + x + 1});
			Recalculate(mesh);
			return Publish(context, std::move(result));
		}
		bool FromPath(NodeContext &context) {
			const auto *value = context.Find("path");
			const auto *path = value ? std::get_if<Path2D>(value) : nullptr;
			if (!path) return true;
			PathRuntime runtime;
			if (!runtime.Init(context, *path)) return false;
			if (runtime.SegmentCount() < 1) return true;
			const int64_t quality = std::max<int64_t>(1, context.Integer("sample", 8));
			if (uint64_t(quality) > Limits::MaximumArrayElements / runtime.SegmentCount())
				return context.Fail(Status::LimitExceeded, "path mesh exceeds point limit", "sample");
			const size_t count = quality * runtime.SegmentCount();
			const int64_t algorithm = context.Integer("algorithm");
			if (algorithm < 0 || algorithm > 2)
				return context.Fail(
					Status::InvalidValue, "path triangulation algorithm is outside its range", "algorithm"
				);
			if (count + (algorithm == 1) > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "path fan exceeds point limit", "sample");
			if (!context.ReserveOutput(
					sizeof(MeshData2D) + (count + 1) * sizeof(VerletPoint) +
						Limits::MaximumLinks * sizeof(std::array<uint32_t, 3>),
					"mesh"
				))
				return false;
			auto workspace = context.ReserveWorkspace(count * 1024 + Limits::MaximumLinks * 1024, "path");
			if (!workspace) return false;
			std::vector<Vector2> points;
			points.reserve(count + 1);
			for (size_t i = 0; i < count; ++i) {
				const auto point = runtime.PointRatio(double(i) / count);
				points.push_back({point.X, point.Y});
			}
			MeshValue2D result;
			auto &mesh = result.Data.emplace();
			mesh.Bounds = {0, 0, 1, 1};
			if (algorithm == 0)
				mesh.Triangles = polygon2d::Ear(points);
			else if (algorithm == 1) {
				Vector2 center{};
				for (auto point : points) {
					center.X += point.X;
					center.Y += point.Y;
				}
				center.X /= count;
				center.Y /= count;
				points.push_back(center);
				mesh.Triangles.reserve(count);
				for (uint32_t i = 0; i < count; ++i)
					mesh.Triangles.push_back({i, uint32_t((i + 1) % count), uint32_t(count)});
			} else if (!polygon2d::Delaunay(points, mesh.Triangles))
				return context.Fail(
					Status::LimitExceeded,
					"source Delaunay triangulation exceeds native coordinate or topology limits",
					"path"
				);
			for (auto &triangle : mesh.Triangles)
				if (polygon2d::Cross(points[triangle[0]], points[triangle[1]], points[triangle[2]]) > 0)
					std::swap(triangle[1], triangle[2]);
			mesh.Simulation.Points.reserve(points.size());
			for (auto position : points) {
				VerletPoint point;
				point.Position = position;
				mesh.Simulation.Points.push_back(point);
			}
			Recalculate(mesh);
			return Publish(context, std::move(result));
		}

		bool Transform(NodeContext &context) {
			const Value *value = context.Find("mesh");
			const auto *input = value ? std::get_if<MeshValue2D>(value) : nullptr;
			if (!input || !input->Data) return true;
			if (!ValidMesh2DPayload(*input))
				return context.Fail(Status::InvalidValue, "mesh transform input is invalid", "mesh");
			if (!context.ReserveOutput(Mesh2DStorageBytes<true>(*input), "mesh")) return false;
			MeshValue2D result = *input;
			auto &mesh = *result.Data;
			mesh.Verlet = false;
			mesh.VerletQuads = false;
			mesh.OriginNodeId.clear();
			mesh.OriginProcessorRow = 0;
			mesh.Quads.clear();
			mesh.SparseQuads.clear();
			mesh.Bounds = {0, 0, 1, 1};
			const auto position = context.Get<Vector2>("position", {}),
					   scale = context.Get<Vector2>("scale", {1, 1}),
					   anchor = context.Get<Vector2>("anchor", {});
			const double rotation = context.Scalar("rotation"), angle = -rotation * std::numbers::pi / 180;
			const Vector2 pivot = {input->Data->Center.X + anchor.X, input->Data->Center.Y + anchor.Y};
			for (auto &point : mesh.Simulation.Points) {
				const Vector2 original = point.Position, uv = point.UV;
				point = {};
				if (mesh.Warp) point.UV = uv;
				point.Position = original;
				const double x = (point.Position.X - pivot.X) * scale.X,
							 y = (point.Position.Y - pivot.Y) * scale.Y;
				point.Position = {
					pivot.X + x * std::cos(angle) - y * std::sin(angle) + position.X,
					pivot.Y + x * std::sin(angle) + y * std::cos(angle) + position.Y
				};
			}
			Recalculate(mesh);
			return Publish(context, std::move(result));
		}
	}
	std::span<const ExecutorEntry> SourceMesh2DExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.mesh_warp", SourceMeshWarp, true},
			{"pc.mesh_create_lattice", Lattice, true},
			{"pc.mesh_create_path", FromPath, true},
			{"pc.mesh_transform", Transform, true},
			{"pc.shape_polygon", SourceShapePolygon, true}
		};
		return entries;
	}
}
