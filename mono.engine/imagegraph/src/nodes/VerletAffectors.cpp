#include "../SourceRandom.hpp"
#include "Curve.hpp"
#include "Processor.hpp"
#include "VerletNodes.hpp"

#include <array>
#include <numbers>

namespace engine::imagegraph::detail {
	bool VerletPinMesh(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *source = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!source || !source->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveCapturedVerlet(context, *source)) return *preserved;
		if (!ValidMesh2DPayload(*source))
			return context.Fail(Status::InvalidValue, "pin mesh input is invalid", "mesh");
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*source), "mesh")) return false;
		MeshValue2D output = *source;
		if (!context.Boolean("active", true) || !source->Data->Verlet)
			return PublishVerletMesh(context, std::move(output));
		const int64_t mode = context.Integer("mode"), target = context.Integer("source");
		if (mode < 0 || mode > 2 || target < 0 || target > 2)
			return context.Fail(Status::InvalidValue, "pin mesh choice is invalid", "mode");
		if (context.FailureCode != Status::Ok) return false;
		auto &points = output.Data->Simulation.Points;
		const auto &edges = output.Data->Simulation.Edges;
		const auto apply = [&](VerletPoint &point, bool selected) {
			if (mode == 0)
				point.Pin = selected;
			else if (mode == 1)
				point.Pin = point.Pin || selected;
			else
				point.Pin = point.Pin && selected;
		};
		if (target == 2) {
			const int64_t index = context.Integer("edge_index");
			if (index < 0 || uint64_t(index) >= edges.size())
				return PublishVerletMesh(context, std::move(output));
			for (auto &point : points)
				point.Pin = false;
			const auto follow = [&](int64_t edgeIndex, bool backward) {
				for (size_t visited = 0; edgeIndex >= 0; ++visited) {
					if (visited >= edges.size())
						return context.Fail(
							Status::InvalidValue, "pin edge loop contains a cycle", "edge_index"
						);
					const auto &edge = edges[size_t(edgeIndex)];
					apply(points[backward ? edge.First : edge.Second], true);
					edgeIndex = backward ? edge.PreviousEdge : edge.NextEdge;
				}
				return true;
			};
			const auto &edge = edges[size_t(index)];
			if (!follow(edge.PreviousEdge, true) || !follow(edge.NextEdge, false)) return false;
			apply(points[edge.First], true);
			apply(points[edge.Second], true);
			return PublishVerletMesh(context, std::move(output));
		}
		const Area area = VerletMeshArea(context);
		const Image *mask = target == 1 ? context.Input("surface") : nullptr;
		if (target == 1 && !mask) return PublishVerletMesh(context, std::move(output));
		if (mask && (mask->Format != SurfaceFormat::RGBA8Unorm ||
					 !ValidSurfaceLayout(*mask, Limits::MaximumDimension, Limits::MaximumOutputBytes)))
			return context.Fail(
				Status::UnsupportedExecution,
				"pin source bitwise floating surface conversion requires a reference capture",
				"surface"
			);
		const auto sourceRound = [](double coordinate) {
			const double floor = std::floor(coordinate), fraction = coordinate - floor;
			return fraction < .5			  ? floor
				   : fraction > .5			  ? floor + 1
				   : std::fmod(floor, 2) == 0 ? floor
											  : floor + 1;
		};
		for (auto &point : points) {
			bool selected;
			if (mask) {
				const uint32_t x =
					uint32_t(std::clamp(sourceRound(point.Position.X), 0.0, double(mask->Width - 1)));
				const uint32_t y =
					uint32_t(std::clamp(sourceRound(point.Position.Y), 0.0, double(mask->Height - 1)));
				const auto pixel = ReadPixel(*mask, x, y);
				selected = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
			} else {
				selected = point.Position.X >= area.CenterX - area.HalfWidth &&
						   point.Position.X <= area.CenterX + area.HalfWidth &&
						   point.Position.Y >= area.CenterY - area.HalfHeight &&
						   point.Position.Y <= area.CenterY + area.HalfHeight;
			}
			apply(point, selected);
		}
		return PublishVerletMesh(context, std::move(output));
	}

	bool VerletPushMesh(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveCapturedVerlet(context, *mesh)) return *preserved;
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "force input mesh is invalid", "mesh");
		if (!context.Boolean("active", true) || !mesh->Data->Verlet) return PublishVerletMesh(context, *mesh);
		const Value *curveValue = context.Find("falloff_curve");
		const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (!curve || !ValidRuntimeValue(*curveValue) || curve->Header[1] == 0 ||
			(curve->Header[2] != 0 && curve->Header[2] != 1))
			return context.Fail(
				Status::InvalidValue, "force falloff requires a finite source curve", "falloff_curve"
			);
		const Area area = VerletMeshArea(context);
		const double falloff = context.Scalar("falloff");
		const Vector2 push = context.Vec2("push");
		const double strength = context.Scalar("strength", 1);
		if (context.FailureCode != Status::Ok) return false;
		if (area.Shape > 1)
			return context.Fail(
				Status::UnsupportedExecution, "source force area shape is unresolved", "area"
			);
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
		MeshValue2D output = *mesh;
		const auto segmentDistance = [](Vector2 p, Vector2 a, Vector2 b) {
			const double dx = b.X - a.X, dy = b.Y - a.Y;
			const double denominator = dx * dx + dy * dy;
			const double t = denominator == 0
								 ? 0
								 : std::clamp(((p.X - a.X) * dx + (p.Y - a.Y) * dy) / denominator, 0.0, 1.0);
			return std::hypot(p.X - a.X - t * dx, p.Y - a.Y - t * dy);
		};
		for (auto &point : output.Data->Simulation.Points) {
			if (point.Pin) continue;
			const auto p = point.Position;
			const double x0 = area.CenterX - area.HalfWidth, x1 = area.CenterX + area.HalfWidth;
			const double y0 = area.CenterY - area.HalfHeight, y1 = area.CenterY + area.HalfHeight;
			bool inside;
			double distance;
			if (area.Shape == 0) {
				inside = p.X >= x0 && p.X <= x1 && p.Y >= y0 && p.Y <= y1;
				distance = std::min(
					{segmentDistance(p, {x0, y0}, {x1, y0}),
					 segmentDistance(p, {x0, y1}, {x1, y1}),
					 segmentDistance(p, {x0, y0}, {x0, y1}),
					 segmentDistance(p, {x1, y0}, {x1, y1})}
				);
			} else {
				const double direction = std::atan2(p.Y - area.CenterY, p.X - area.CenterX);
				const Vector2 boundary{
					area.CenterX + area.HalfWidth * std::cos(direction),
					area.CenterY + area.HalfHeight * std::sin(direction)
				};
				inside = std::hypot(p.X - area.CenterX, p.Y - area.CenterY) <
						 std::hypot(boundary.X - area.CenterX, boundary.Y - area.CenterY);
				distance = std::hypot(p.X - boundary.X, p.Y - boundary.Y);
			}
			double influence = inside ? 1.0 : 0.0;
			if (falloff != 0 && distance <= falloff)
				influence = std::clamp(
					inside ? .5 + distance / falloff * .5 : .5 - distance / falloff * .5, 0.0, 1.0
				);
			influence = EvalCurveX(*curve, influence);
			point.Position.X += influence * push.X * strength / 10;
			point.Position.Y += influence * push.Y * strength / 10;
		}
		return PublishVerletMesh(context, std::move(output));
	}

	bool VerletBloat(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveCapturedVerlet(context, *mesh)) return *preserved;
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "bloat input mesh is invalid", "mesh");
		if (!context.Boolean("active", true) || !mesh->Data->Verlet) return PublishVerletMesh(context, *mesh);
		const Value *curveValue = context.Find("falloff_curve");
		const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (!curve || !ValidRuntimeValue(*curveValue) || curve->Header[1] == 0 ||
			(curve->Header[2] != 0 && curve->Header[2] != 1))
			return context.Fail(
				Status::InvalidValue, "bloat falloff requires a finite source curve", "falloff_curve"
			);
		const Area area = VerletMeshArea(context);
		const double falloff = context.Scalar("falloff", 4), strength = context.Scalar("strength", 1);
		const bool origin = context.Boolean("use_origin", true);
		if (context.FailureCode != Status::Ok) return false;
		if (area.Shape > 1)
			return context.Fail(Status::UnsupportedExecution, "bloat area shape is unresolved", "area");
		std::array<double, 33> map;
		for (size_t index = 0; index < map.size(); ++index) {
			map[index] = EvalCurveX(*curve, double(index) / 32, .00001);
			if (!std::isfinite(map[index]))
				return context.Fail(Status::InvalidValue, "bloat curve map is nonfinite", "falloff_curve");
		}
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
		MeshValue2D output = *mesh;
		const auto segmentDistance = [](Vector2 p, Vector2 a, Vector2 b) {
			const double dx = b.X - a.X, dy = b.Y - a.Y, square = dx * dx + dy * dy;
			const double ratio =
				square == 0 ? 0 : std::clamp(((p.X - a.X) * dx + (p.Y - a.Y) * dy) / square, 0.0, 1.0);
			return std::hypot(p.X - a.X - ratio * dx, p.Y - a.Y - ratio * dy);
		};
		for (auto &point : output.Data->Simulation.Points) {
			if (point.Pin) continue;
			const Vector2 p = origin ? point.Original : point.Position;
			const double dx = p.X - area.CenterX, dy = p.Y - area.CenterY;
			const double radius = std::hypot(dx, dy);
			const double x0 = area.CenterX - area.HalfWidth, x1 = area.CenterX + area.HalfWidth,
						 y0 = area.CenterY - area.HalfHeight, y1 = area.CenterY + area.HalfHeight;
			bool inside;
			double distance;
			if (area.Shape == 0) {
				inside = p.X >= x0 && p.X <= x1 && p.Y >= y0 && p.Y <= y1;
				distance = std::min(
					{segmentDistance(p, {x0, y0}, {x1, y0}),
					 segmentDistance(p, {x0, y1}, {x1, y1}),
					 segmentDistance(p, {x0, y0}, {x0, y1}),
					 segmentDistance(p, {x1, y0}, {x1, y1})}
				);
			} else {
				const double direction = std::atan2(dy, dx);
				const Vector2 boundary{
					area.CenterX + area.HalfWidth * std::cos(direction),
					area.CenterY + area.HalfHeight * std::sin(direction)
				};
				inside = radius < std::hypot(boundary.X - area.CenterX, boundary.Y - area.CenterY);
				distance = std::hypot(p.X - boundary.X, p.Y - boundary.Y);
			}
			double influence = inside ? .5 + distance / falloff : .5 - distance / falloff;
			if (std::isnan(influence))
				influence = 0;
			else {
				const double index = std::clamp(influence, 0.0, 1.0) * 32;
				const size_t low = size_t(std::floor(index)), high = size_t(std::ceil(index));
				influence = CurveLerp(map[low], map[high], index - low);
			}
			if (influence <= 0 || radius <= 0) continue;
			const double direction = std::atan2(dy, dx), bloat = influence * strength;
			point.Position.X += bloat * std::cos(direction);
			point.Position.Y += bloat * std::sin(direction);
		}
		return PublishVerletMesh(context, std::move(output));
	}
	bool VerletTear(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveCapturedVerlet(context, *mesh)) return *preserved;
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "tear input mesh is invalid", "mesh");
		if (!context.Boolean("active", true) || !mesh->Data->Verlet) return PublishVerletMesh(context, *mesh);
		const double chance = context.Scalar("chance", 1),
					 seed = context.Scalar("seed", double(uint32_t(context.Request.Seed)));
		const double mode = context.SourceChoice("source");
		const bool breakMesh = context.Boolean("break_mesh");
		if (context.FailureCode != Status::Ok) return false;
		if (mode != 0 && mode != 1 && mode != 2)
			return context.Fail(
				Status::UnsupportedExecution, "tear source selection is unresolved", "source"
			);
		const Image *mask = mode == 1 ? context.Input("surface") : nullptr;
		if (mode == 1 && !mask) return PublishVerletMesh(context, *mesh);
		if (mask && (mask->Format != SurfaceFormat::RGBA8Unorm ||
					 !ValidSurfaceLayout(*mask, Limits::MaximumDimension, Limits::MaximumOutputBytes)))
			return context.Fail(
				Status::UnsupportedExecution,
				"tear source surface bit conversion requires a reference capture",
				"surface"
			);
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
		MeshValue2D output = *mesh;
		auto &points = output.Data->Simulation.Points;
		auto &edges = output.Data->Simulation.Edges;
		double wrappedSeed = std::fmod(std::trunc(seed), 4294967296.0);
		if (wrappedSeed < 0) wrappedSeed += 4294967296.0;
		SourceRandom random{uint32_t(wrappedSeed)};
		const auto tear = [&](VerletEdge &edge) {
			edge.Active = false;
			if (breakMesh) {
				points[edge.First].Active = false;
				points[edge.Second].Active = false;
			}
		};
		if (mode == 2) {
			const int64_t index = context.Integer("edge_index");
			if (index < 0 || uint64_t(index) >= edges.size())
				return PublishVerletMesh(context, std::move(output));
			const auto draw = [&](int64_t edgeIndex, bool previous) {
				for (size_t visits = 0; edgeIndex >= 0; ++visits) {
					if (visits >= edges.size())
						return context.Fail(
							Status::InvalidValue, "tear edge loop contains a cycle", "edge_index"
						);
					auto &edge = edges[size_t(edgeIndex)];
					if (random.Unit() < chance) tear(edge);
					edgeIndex = previous ? edge.PreviousEdge : edge.NextEdge;
				}
				return true;
			};
			auto &edge = edges[size_t(index)];
			if (random.Unit() < chance) tear(edge);
			if (!draw(edge.PreviousEdge, true) || !draw(edge.NextEdge, false)) return false;
		} else {
			const Area area = VerletMeshArea(context);
			const auto round = [](double coordinate) {
				const double floor = std::floor(coordinate), fraction = coordinate - floor;
				return fraction < .5			  ? floor
					   : fraction > .5			  ? floor + 1
					   : std::fmod(floor, 2) == 0 ? floor
												  : floor + 1;
			};
			for (auto &edge : edges) {
				if (random.Unit() > chance || !edge.Active) continue;
				const Vector2 center{
					(points[edge.First].Position.X + points[edge.Second].Position.X) / 2,
					(points[edge.First].Position.Y + points[edge.Second].Position.Y) / 2
				};
				bool selected =
					center.X >= area.CenterX - area.HalfWidth && center.X <= area.CenterX + area.HalfWidth &&
					center.Y >= area.CenterY - area.HalfHeight && center.Y <= area.CenterY + area.HalfHeight;
				if (mask) {
					const uint32_t x = uint32_t(std::clamp(round(center.X), 0.0, double(mask->Width - 1)));
					const uint32_t y = uint32_t(std::clamp(round(center.Y), 0.0, double(mask->Height - 1)));
					const auto pixel = ReadPixel(*mask, x, y);
					selected = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
				}
				if (selected) tear(edge);
			}
		}
		return PublishVerletMesh(context, std::move(output));
	}
	bool VerletWind(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveCapturedVerlet(context, *mesh)) return *preserved;
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "wind input mesh is invalid", "mesh");
		if (!context.Boolean("active", true) || !mesh->Data->Verlet) return PublishVerletMesh(context, *mesh);
		const Value *curveValue = context.Find("falloff_curve");
		const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (!curve || !ValidRuntimeValue(*curveValue) || curve->Header[1] == 0 ||
			(curve->Header[2] != 0 && curve->Header[2] != 1))
			return context.Fail(
				Status::InvalidValue, "wind falloff requires a finite source curve", "falloff_curve"
			);
		Vector2 center = context.Vec2("center", {.5, .5});
		if (!context.IsLinked("center") && context.Integer("center_unit", 1) == 1) {
			const auto dimension = VerletScopeDimension(context);
			center.X *= dimension.X;
			center.Y *= dimension.Y;
		}
		const double direction = context.Scalar("direction") * std::numbers::pi / 180;
		const double width = context.Scalar("width", 8), falloff = context.Scalar("falloff", 4),
					 strength = context.Scalar("strength", 1);
		if (context.FailureCode != Status::Ok) return false;
		if (falloff == 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"source zero wind falloff division requires a reference capture",
				"falloff"
			);
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
		MeshValue2D output = *mesh;
		const double vx = strength * std::cos(direction) / 10, vy = -strength * std::sin(direction) / 10;
		for (auto &point : output.Data->Simulation.Points) {
			if (point.Pin) continue;
			const double dx = point.Position.X - center.X, dy = point.Position.Y - center.Y;
			const double pointDirection = std::atan2(-dy, dx);
			double angle = std::fmod(pointDirection - direction, 2 * std::numbers::pi);
			if (angle < -std::numbers::pi) angle += 2 * std::numbers::pi;
			if (angle >= std::numbers::pi) angle -= 2 * std::numbers::pi;
			const double distance = std::abs(std::hypot(dx, dy) * std::sin(std::abs(angle)));
			double influence = 1 - (distance - (width / 2 - falloff)) / (falloff * 2);
			influence = EvalCurveX(*curve, std::clamp(influence, 0.0, 1.0));
			point.Position.X += influence * vx;
			point.Position.Y += influence * vy;
		}
		return PublishVerletMesh(context, std::move(output));
	}
}
