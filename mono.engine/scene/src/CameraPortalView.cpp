#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Wire.hpp>

#include <cmath>

namespace engine::scene {
	namespace {
		bool Finite(const core::Vector3 &v) {
			return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
		}
		bool Rigid(const core::CFrame &frame) {
			const auto rotation = frame.Rotation();
			return Finite(frame.Position) && std::isfinite(glm::dot(rotation, rotation)) &&
				   std::abs(glm::dot(rotation, rotation) - 1.0f) < .001f;
		}
		bool Valid(const SeamTransform &through) {
			return Rigid(through.Frame) && Finite(through.Origin) && std::isfinite(through.Scale) &&
				   through.Scale > 0;
		}
		bool RouteText(std::string_view text) {
			return !text.empty() && text.size() <= 256 && text.find('\0') == std::string_view::npos;
		}
		bool Valid(const CameraPortalRoute &route) {
			return RouteText(route.SourceWorld) && RouteText(route.DestinationWorld) &&
				   RouteText(route.PanePath) && RouteText(route.FarPath) && Valid(route.BeforeFromInput) &&
				   Finite(route.EntryInputPoint) && Finite(route.EntryInputDirection) &&
				   route.EntryInputDirection.Dot(route.EntryInputDirection) > .00000001f;
		}
		core::Vector3 UnmapPoint(const SeamTransform &map, const core::Vector3 &point) {
			const auto rigid = map.Frame.Inverse().PointToWorldSpace(point);
			return map.Origin + (rigid - map.Origin) / map.Scale;
		}
		SeamTransform Compose(const SeamTransform &outer, const SeamTransform &inner) {
			return {
				core::CFrame(
					outer.Point(inner.Point(core::Vector3::Zero)),
					outer.Frame.Rotation() * inner.Frame.Rotation()
				),
				{},
				outer.Scale * inner.Scale
			};
		}
	}

	bool ValidCameraPortalView(const CameraPortalView &view) {
		const bool arrival =
			view.ArrivedFrom.empty() ||
			(view.Started && RouteText(view.ArrivedFrom) && RouteText(view.ArrivalPanePath) &&
			 Finite(view.ArrivalPoint) && Finite(view.ArrivalNormal) &&
			 std::abs(view.ArrivalNormal.Dot(view.ArrivalNormal) - 1) < .001f &&
			 std::isfinite(view.ArrivalTolerance) && view.ArrivalTolerance > 0);
		if (!arrival || !Valid(view.FromInput) || view.Route.size() > MAX_CAMERA_PORTAL_ROUTE_HOPS)
			return false;
		for (const CameraPortalRoute &route : view.Route)
			if (!Valid(route)) return false;
		return !view.Started || (RouteText(view.World) && Rigid(view.Previous));
	}

	CameraPortalStep StepCameraPortalView(
		CameraPortalView &view,
		std::string_view initialWorld,
		const core::CFrame &input,
		std::span<const PortalSeam> seams
	) {
		if (!Rigid(input) || !ValidCameraPortalView(view) || initialWorld.empty() ||
			initialWorld.size() > 256 || initialWorld.find('\0') != std::string_view::npos)
			return CameraPortalStep::Invalid;
		if (!view.Started) {
			view.World = initialWorld;
			view.Previous = input;
			view.FromInput = {};
			view.Route.clear();
			view.Started = true;
			return CameraPortalStep::Settled;
		}
		if (!Rigid(view.Previous) || view.World.empty()) return CameraPortalStep::Invalid;
		const auto eye = view.FromInput.Place(input);
		if (!Rigid(eye)) return CameraPortalStep::Invalid;
		const PortalSeam *receiving = nullptr;
		if (!view.ArrivedFrom.empty()) {
			for (const auto &seam : seams) {
				if (!seam.Crosses || seam.DestinationWorld.Text() != view.ArrivedFrom ||
					scene::SeamDistance(seam, view.ArrivalPoint) > view.ArrivalTolerance ||
					(!seam.PanePath.empty() && seam.PanePath != view.ArrivalPanePath))
					continue;
				if (receiving != nullptr) return CameraPortalStep::Invalid;
				receiving = &seam;
			}
		}
		const float arrivalDepth = (eye.Position - view.ArrivalPoint).Dot(view.ArrivalNormal);
		const bool leavingArrival = arrivalDepth > view.ArrivalTolerance;
		const bool reversingArrival = arrivalDepth < -view.ArrivalTolerance;
		const PortalSeam *nearest = nullptr;
		core::Vector3 nearestBefore = view.Previous.Position;
		bool onAperture = false;
		float first = 1;
		for (const auto &seam : seams) {
			if (!seam.Crosses || !seam.DestinationWorld.IsValid()) continue;
			const std::string panePath =
				seam.PanePath.empty() ? std::string(seam.DestinationWorld.Text()) : seam.PanePath;
			const std::string farPath = seam.FarPath.empty() ? view.World : seam.FarPath;
			if (!view.Route.empty()) {
				const CameraPortalRoute &route = view.Route.back();
				// A mapped eye can coincide with another mouth in this same world.
				// Until it returns, only the recorded inverse represents authored motion.
				if (view.World != route.DestinationWorld ||
					seam.DestinationWorld.Text() != route.SourceWorld || panePath != route.FarPath ||
					farPath != route.PanePath ||
					(input.Position - route.EntryInputPoint).Dot(route.EntryInputDirection) > 0)
					continue;
			}
			if (&seam == receiving && !reversingArrival) continue;
			// A slow reversal may spend several frames inside the rounding band.
			// Its crossing starts on the known arrival side, not the last jittered sample.
			const auto beforePoint = &seam == receiving
										 ? view.ArrivalPoint + view.ArrivalNormal * view.ArrivalTolerance
										 : view.Previous.Position;
			const float before = SeamOffset(seam, beforePoint);
			const float after = SeamOffset(seam, eye.Position);
			if (!std::isfinite(before) || !std::isfinite(after) || (!seam.Bidirectional && before < 0))
				continue;
			if (after == 0 && before != 0 && SeamDistance(seam, eye.Position) == 0) {
				onAperture = true;
				continue;
			}
			if (before * after >= 0) continue;
			const float fraction = before / (before - after);
			if (!(fraction >= 0 && fraction < first)) continue;
			const auto point = beforePoint + (eye.Position - beforePoint) * fraction;
			const auto offset = point - seam.Centre;
			const float firstSquared = seam.First.Dot(seam.First);
			const float secondSquared = seam.Second.Dot(seam.Second);
			if (!(firstSquared > 0 && secondSquared > 0) || std::abs(offset.Dot(seam.First)) > firstSquared ||
				std::abs(offset.Dot(seam.Second)) > secondSquared)
				continue;
			first = fraction;
			nearest = &seam;
			nearestBefore = beforePoint;
		}
		if (nearest == nullptr) {
			if (!onAperture) view.Previous = eye;
			if (leavingArrival) {
				view.ArrivedFrom.clear();
				view.ArrivalPanePath.clear();
			}
			return CameraPortalStep::Settled;
		}
		const auto through = SeamMapping(*nearest);
		const std::string panePath =
			nearest->PanePath.empty() ? std::string(nearest->DestinationWorld.Text()) : nearest->PanePath;
		const std::string farPath = nearest->FarPath.empty() ? view.World : nearest->FarPath;
		const bool returns = !view.Route.empty() && view.World == view.Route.back().DestinationWorld &&
							 nearest->DestinationWorld.Text() == view.Route.back().SourceWorld &&
							 panePath == view.Route.back().FarPath && farPath == view.Route.back().PanePath;
		const auto fromInput = returns ? view.Route.back().BeforeFromInput : Compose(through, view.FromInput);
		if (!Valid(fromInput)) return CameraPortalStep::Invalid;
		if (!returns &&
			(view.Route.size() == MAX_CAMERA_PORTAL_ROUTE_HOPS || !RouteText(view.World) ||
			 !RouteText(nearest->DestinationWorld.Text()) || !RouteText(panePath) || !RouteText(farPath)))
			return CameraPortalStep::Invalid;
		const auto point = nearestBefore + (eye.Position - nearestBefore) * first;
		if (returns)
			view.Route.pop_back();
		else {
			CameraPortalRoute route;
			route.SourceWorld = view.World;
			route.DestinationWorld = nearest->DestinationWorld.Text();
			route.PanePath = panePath;
			route.FarPath = farPath;
			route.BeforeFromInput = view.FromInput;
			const core::Vector3 previousInput = UnmapPoint(view.FromInput, view.Previous.Position);
			route.EntryInputPoint = previousInput + (input.Position - previousInput) * first;
			route.EntryInputDirection = input.Position - previousInput;
			view.Route.push_back(std::move(route));
		}
		view.FromInput = fromInput;
		view.ArrivedFrom = view.World;
		view.ArrivalPanePath = farPath;
		view.ArrivalPoint = through.Point(point);
		view.ArrivalNormal =
			through.Rotate(nearest->Normal).Unit() * (SeamOffset(*nearest, eye.Position) < 0 ? -1.f : 1.f);
		// The eye is authored locally. A decoded portal may be coarse, but giving
		// its position-grid error back to the route turns a 0.1-stud walk into a
		// multi-frame arrival band that can consume the return crossing.
		view.ArrivalTolerance =
			std::min(0.01f, std::sqrt(3.f) * WIRE_POSITION_ERROR_METRES * (1 + through.Scale));
		view.World = nearest->DestinationWorld.Text();
		view.Previous = through.Place(core::CFrame(point, eye.Rotation()));
		return CameraPortalStep::Crossed;
	}

	bool RebaseCameraPortalView(CameraPortalView &view, const SeamTransform &bodyThrough) {
		if (!Valid(bodyThrough) || !ValidCameraPortalView(view)) return false;
		if (!view.Started) return true;
		CameraPortalView rebased = view;
		const auto rigidInverse = bodyThrough.Frame.Inverse();
		const SeamTransform inverse{
			core::CFrame(
				rigidInverse.Position / bodyThrough.Scale + bodyThrough.Origin * (1 - 1 / bodyThrough.Scale),
				rigidInverse.Rotation()
			),
			{},
			1 / bodyThrough.Scale
		};
		const auto map = Compose(rebased.FromInput, inverse);
		if (!Valid(map)) return false;
		rebased.FromInput = map;
		for (CameraPortalRoute &route : rebased.Route) {
			const auto before = Compose(route.BeforeFromInput, inverse);
			if (!Valid(before)) return false;
			route.BeforeFromInput = before;
			route.EntryInputPoint = bodyThrough.Point(route.EntryInputPoint);
			route.EntryInputDirection = bodyThrough.Carry(route.EntryInputDirection);
		}
		view = std::move(rebased);
		return true;
	}
}
