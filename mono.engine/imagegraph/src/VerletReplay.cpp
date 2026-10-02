#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/VerletReplay.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace engine::imagegraph {
	namespace {
		Status Refuse(Diagnostic &diagnostic, Status status, std::string message) {
			diagnostic = {};
			diagnostic.Code = status;
			diagnostic.Message = std::move(message);
			return status;
		}
		bool Finite(Vector2 vector) {
			return std::isfinite(vector.X) && std::isfinite(vector.Y);
		}
		Status ValidateMesh(
			std::span<const VerletPoint> points,
			std::span<const VerletEdge> edges,
			uint64_t maximumBytes,
			Diagnostic &diagnostic
		) {
			if (points.size() > Limits::MaximumArrayElements || edges.size() > Limits::MaximumLinks)
				return Refuse(diagnostic, Status::LimitExceeded, "verlet mesh exceeds point or edge budget");
			const uint64_t bytes = points.size() * sizeof(VerletPoint) + edges.size() * sizeof(VerletEdge);
			if (bytes > maximumBytes || bytes > Limits::MaximumEvaluationBytes)
				return Refuse(diagnostic, Status::LimitExceeded, "verlet snapshot exceeds byte budget");
			for (const auto &point : points)
				if (!Finite(point.Position) || !Finite(point.Previous) || !Finite(point.BeforePrevious) ||
					point.SourceIndex >= Limits::MaximumArrayElements || !std::isfinite(point.Drag) ||
					!Finite(point.UV) || !Finite(point.Original) || !Finite(point.VelocityReference) ||
					(point.DrawPosition && !Finite(*point.DrawPosition)))
					return Refuse(diagnostic, Status::InvalidValue, "verlet point must be finite");
			for (const auto &edge : edges) {
				if (edge.First >= points.size() || edge.Second >= points.size() || edge.PreviousEdge < -1 ||
					edge.NextEdge < -1 ||
					(edge.PreviousEdge >= 0 && uint64_t(edge.PreviousEdge) >= edges.size()) ||
					(edge.NextEdge >= 0 && uint64_t(edge.NextEdge) >= edges.size()))
					return Refuse(diagnostic, Status::InvalidValue, "verlet edge endpoint is outside mesh");
				if (!std::isfinite(edge.Distance) || !std::isfinite(edge.Flexibility) ||
					!std::isfinite(edge.DirectionDegrees) || !std::isfinite(edge.AngularDrag))
					return Refuse(diagnostic, Status::InvalidValue, "verlet edge must be finite");
			}
			return Status::Ok;
		}
		void Propagate(VerletMesh &mesh, const VerletStepSettings &settings, bool inverse) {
			const double divisor = double(settings.Substeps) * (settings.Simple ? 1 : 10);
			const Vector2 gravity{settings.Gravity.X / divisor, settings.Gravity.Y / divisor};
			for (size_t order = 0; order < mesh.Points.size(); ++order) {
				auto &point = mesh.Points[inverse ? mesh.Points.size() - order - 1 : order];
				if (!settings.Simple && (point.Rest || point.Pin)) continue;
				const Vector2 velocity{
					point.Position.X - point.Previous.X + gravity.X,
					point.Position.Y - point.Previous.Y + gravity.Y
				};
				if (!settings.Simple) point.BeforePrevious = point.Previous;
				point.Previous = point.Position;
				if (point.Pin) continue;
				point.Position.X += velocity.X;
				point.Position.Y += velocity.Y;
				if (!settings.Simple) {
					const double damping = 1 - std::pow(point.Drag, 4);
					point.Position.X = point.Previous.X + (point.Position.X - point.Previous.X) * damping;
					point.Position.Y = point.Previous.Y + (point.Position.Y - point.Previous.Y) * damping;
				}
			}
		}
		void Constrain(VerletMesh &mesh, bool simple, bool inverse) {
			constexpr double DEGREES_PER_RADIAN = 180 / std::numbers::pi;
			for (size_t order = 0; order < mesh.Edges.size(); ++order) {
				auto &edge = mesh.Edges[inverse ? mesh.Edges.size() - order - 1 : order];
				if (!simple && !edge.Active) continue;
				auto &first = mesh.Points[edge.First];
				auto &second = mesh.Points[edge.Second];
				if (!simple && (!first.Active || !second.Active)) {
					edge.Active = false;
					continue;
				}
				const bool fixedFirst = first.Pin || (!simple && first.Rest);
				const bool fixedSecond = second.Pin || (!simple && second.Rest);
				if (fixedFirst && fixedSecond) {
					first.Position = first.Previous;
					second.Position = second.Previous;
					continue;
				}
				const double deltaX = second.Position.X - first.Position.X;
				const double deltaY = second.Position.Y - first.Position.Y;
				const double currentDistance = std::hypot(deltaX, deltaY);
				const double targetDistance =
					edge.Distance + (currentDistance - edge.Distance) * edge.Flexibility;
				double direction = std::atan2(-deltaY, deltaX) * DEGREES_PER_RADIAN;
				if (direction < 0) direction += 360;
				double constrainedDirection = direction;
				if (!simple) {
					double difference = std::fmod(direction - edge.DirectionDegrees, 360);
					if (difference < -180) difference += 360;
					if (difference >= 180) difference -= 360;
					edge.DirectionDegrees += difference * (1 - edge.AngularDrag);
					constrainedDirection = edge.DirectionDegrees;
				}
				const double radians = constrainedDirection / DEGREES_PER_RADIAN;
				const Vector2 offset{targetDistance * std::cos(radians), -targetDistance * std::sin(radians)};
				if (fixedFirst) {
					first.Position = first.Previous;
					second.Position = {first.Position.X + offset.X, first.Position.Y + offset.Y};
				} else if (fixedSecond) {
					// grug preserve source order: first uses second position before second resets.
					first.Position = {second.Position.X - offset.X, second.Position.Y - offset.Y};
					second.Position = second.Previous;
				} else {
					const Vector2 center{
						(first.Position.X + second.Position.X) / 2, (first.Position.Y + second.Position.Y) / 2
					};
					first.Position = {center.X - offset.X / 2, center.Y - offset.Y / 2};
					second.Position = {center.X + offset.X / 2, center.Y + offset.Y / 2};
				}
			}
		}
	}
	namespace {
		void CollideWall(VerletMesh &mesh, const VerletStepSettings &settings) {
			for (auto &point : mesh.Points) {
				if (point.Rest) continue;
				if (settings.Wall == 1 && point.Position.Y < 0) {
					point.Rest = true;
					point.Position.Y = 0;
				} else if (settings.Wall == 2 && point.Position.Y > settings.Dimension.Y) {
					point.Rest = true;
					point.Position.Y = settings.Dimension.Y;
				} else if (settings.Wall == 4 && point.Position.X < 0) {
					point.Rest = true;
					point.Position.X = 0;
				} else if (settings.Wall == 8 && point.Position.X > settings.Dimension.X) {
					point.Rest = true;
					point.Position.X = settings.Dimension.X;
				}
			}
		}
	}
	Status ResetVerletReplay(
		std::span<const VerletPoint> points,
		std::span<const VerletEdge> edges,
		uint64_t tick,
		uint64_t authoringRevision,
		uint64_t maximumBytes,
		VerletReplayState &state,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.verlet.reset");
		diagnostic = {};
		if (tick > Limits::MaximumTick)
			return Refuse(diagnostic, Status::LimitExceeded, "verlet tick exceeds timeline budget");
		const auto status = ValidateMesh(points, edges, maximumBytes, diagnostic);
		if (status != Status::Ok) return status;
		VerletReplayState fresh;
		fresh.Mesh.Points.assign(points.begin(), points.end());
		fresh.Mesh.Edges.assign(edges.begin(), edges.end());
		fresh.Tick = tick;
		fresh.AuthoringRevision = authoringRevision;
		fresh.Initialized = true;
		state = std::move(fresh);
		return Status::Ok;
	}
	Status StepVerletReplay(
		const VerletReplayState &previous,
		uint64_t tick,
		uint64_t authoringRevision,
		const VerletStepSettings &settings,
		VerletReplayState &next,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.verlet.step");
		diagnostic = {};
		if (!previous.Initialized || previous.AuthoringRevision != authoringRevision ||
			previous.Tick >= Limits::MaximumTick || tick != previous.Tick + 1)
			return Refuse(
				diagnostic, Status::InvalidValue, "verlet requires contiguous tick and matching revision"
			);
		if (!Finite(settings.Gravity))
			return Refuse(diagnostic, Status::InvalidValue, "verlet gravity must be finite");
		if (!Finite(settings.Dimension) || settings.Dimension.X < 0 || settings.Dimension.Y < 0 ||
			settings.Wall > 15)
			return Refuse(
				diagnostic, Status::InvalidValue, "verlet wall domain must be finite and nonnegative"
			);
		if (settings.Wall && ((settings.Wall & (settings.Wall - 1)) != 0 || settings.Simple))
			return Refuse(
				diagnostic,
				Status::UnsupportedExecution,
				"source combined wall cursor behavior requires a reference capture"
			);
		const uint64_t units = previous.Mesh.Points.size() + previous.Mesh.Edges.size();
		if (settings.Substeps > Limits::MaximumRangeFrames ||
			(units != 0 && settings.Substeps > settings.MaximumWork / units))
			return Refuse(diagnostic, Status::LimitExceeded, "verlet step exceeds work budget");
		const auto status =
			ValidateMesh(previous.Mesh.Points, previous.Mesh.Edges, settings.MaximumBytes, diagnostic);
		if (status != Status::Ok) return status;
		VerletReplayState candidate = previous;
		bool inverse = false;
		for (uint32_t step = 0; step < settings.Substeps; ++step) {
			Propagate(candidate.Mesh, settings, inverse);
			if (!settings.Simple) CollideWall(candidate.Mesh, settings);
			Constrain(candidate.Mesh, settings.Simple, inverse);
			if (!settings.Simple) CollideWall(candidate.Mesh, settings);
			if (!settings.Simple) inverse = !inverse;
		}
		const auto valid =
			ValidateMesh(candidate.Mesh.Points, candidate.Mesh.Edges, settings.MaximumBytes, diagnostic);
		if (valid != Status::Ok) return valid;
		candidate.Tick = tick;
		next = std::move(candidate);
		return Status::Ok;
	}
}
