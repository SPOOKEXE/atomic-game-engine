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
			(view.Started && view.ArrivedFrom.size() <= 256 &&
			 view.ArrivedFrom.find('\0') == std::string::npos && Finite(view.ArrivalPoint) &&
			 Finite(view.ArrivalNormal) && std::abs(view.ArrivalNormal.Dot(view.ArrivalNormal) - 1) < .001f &&
			 std::isfinite(view.ArrivalTolerance) && view.ArrivalTolerance > 0);
		return arrival && Valid(view.FromInput) &&
			   (!view.Started || (!view.World.empty() && view.World.size() <= 256 &&
								  view.World.find('\0') == std::string::npos && Rigid(view.Previous)));
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
					scene::SeamDistance(seam, view.ArrivalPoint) > view.ArrivalTolerance)
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
			if (leavingArrival) view.ArrivedFrom.clear();
			return CameraPortalStep::Settled;
		}
		const auto through = SeamMapping(*nearest);
		const auto fromInput = Compose(through, view.FromInput);
		if (!Valid(fromInput)) return CameraPortalStep::Invalid;
		const auto point = nearestBefore + (eye.Position - nearestBefore) * first;
		view.FromInput = fromInput;
		view.ArrivedFrom = view.World;
		view.ArrivalPoint = through.Point(point);
		view.ArrivalNormal =
			through.Rotate(nearest->Normal).Unit() * (SeamOffset(*nearest, eye.Position) < 0 ? -1.f : 1.f);
		view.ArrivalTolerance = std::sqrt(3.f) * WIRE_POSITION_ERROR_METRES * (1 + through.Scale);
		view.World = nearest->DestinationWorld.Text();
		view.Previous = through.Place(core::CFrame(point, eye.Rotation()));
		return CameraPortalStep::Crossed;
	}

	bool RebaseCameraPortalView(CameraPortalView &view, const SeamTransform &bodyThrough) {
		if (!Valid(bodyThrough) || !ValidCameraPortalView(view)) return false;
		if (!view.Started) return true;
		const auto rigidInverse = bodyThrough.Frame.Inverse();
		const SeamTransform inverse{
			core::CFrame(
				rigidInverse.Position / bodyThrough.Scale + bodyThrough.Origin * (1 - 1 / bodyThrough.Scale),
				rigidInverse.Rotation()
			),
			{},
			1 / bodyThrough.Scale
		};
		const auto rebased = Compose(view.FromInput, inverse);
		if (!Valid(rebased)) return false;
		view.FromInput = rebased;
		return true;
	}
}
