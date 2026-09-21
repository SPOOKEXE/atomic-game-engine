#pragma once

#include <engine/scene/SurfaceCameras.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace engine::scene {
	inline constexpr size_t MAX_CAMERA_PORTAL_ROUTE_HOPS = 8;

	// One exact crossing in the eye's route. World names and full pane paths
	// disambiguate two links between the same pair of rooms.
	struct CameraPortalRoute {
		// World containing the pane through which this hop entered.
		std::string SourceWorld;
		// World containing the paired destination pane.
		std::string DestinationWorld;
		// Full instance path of the source pane.
		std::string PanePath;
		// Full instance path of the paired destination pane.
		std::string FarPath;
		// Cumulative input map before this hop was applied.
		SeamTransform BeforeFromInput;
		// Authored crossing plane and travel direction. The virtual destination
		// can have another nearby mouth, but it is not a return until the source
		// eye crosses this plane in the opposite direction.
		core::Vector3 EntryInputPoint;
		// Authored direction that crossed the source pane.
		core::Vector3 EntryInputDirection;
	};

	// Presentation history in the eye's world. Camera transforms and controls
	// remain in their subject's world; admission rebases FromInput only.
	struct CameraPortalView {
		// Current world of the eye's presentation history.
		std::string World;
		// Cumulative body seam map rebased into World.
		SeamTransform FromInput;
		// Ordered crossings from the authored input world to `World`.
		std::vector<CameraPortalRoute> Route;
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
		// Stable full path of the receiving pane. Worlds may contain several portals
		// with the same destination world, so that name disambiguates the return.
		std::string ArrivalPanePath;
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
