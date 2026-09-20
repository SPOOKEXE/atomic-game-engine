#pragma once

#include <engine/scene/SurfaceCameras.hpp>

#include <span>
#include <string>

namespace engine::scene {
	// Presentation history in the eye's world. Camera transforms and controls
	// remain in their subject's world; admission rebases FromInput only.
	struct CameraPortalView {
		// Current world of the eye's presentation history.
		std::string World;
		// Cumulative body seam map rebased into World.
		SeamTransform FromInput;
		// Input camera frame before the most recent crossing.
		core::CFrame Previous;
		// Whether portal presentation history has been initialized.
		bool Started = false;
		// The receiving mouth can differ from the mapped crossing point by wire
		// rounding. Keep its arrival side until the eye clears that uncertainty.
		std::string ArrivedFrom;
		// Destination-side point at which the eye arrived.
		core::Vector3 ArrivalPoint;
		// Destination-side portal normal at arrival.
		core::Vector3 ArrivalNormal;
		// Arrival-side clearance distance in metres.
		float ArrivalTolerance = 0;
	};

	enum class CameraPortalStep { Settled, Crossed, Invalid };
	bool ValidCameraPortalView(const CameraPortalView &view);

	// Consumes at most one foreign seam. After Crossed, the caller supplies the
	// named destination's seams and repeats against the same input camera pose.
	CameraPortalStep StepCameraPortalView(
		CameraPortalView &view,
		std::string_view initialWorld,
		const core::CFrame &input,
		std::span<const PortalSeam> seams
	);
	// The body changes coordinate systems while the eye remains in its own room.
	bool RebaseCameraPortalView(CameraPortalView &view, const SeamTransform &bodyThrough);
}
