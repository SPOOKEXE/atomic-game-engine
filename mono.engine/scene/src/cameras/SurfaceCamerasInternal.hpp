// Helpers shared by the surface camera sources: face lookup, seam gathering and
// crossing tests. Private to src/cameras.
#pragma once

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <vector>

namespace engine::scene::surface_camera_detail {
	using core::CFrame;
	using core::Vector3;
	using ecs::Entity;
	using ecs::Store;

	// How close to a mirror's own plane the viewer may be before its surface
	// stops drawing.
	//
	// **A mirror's, and not a portal's.** The discontinuity below is
	// `facing` changing sign with nothing else changing, which is true of a
	// reflection and false of a hole: a portal's eye is carried through the
	// pane on the same frame, so the two flips cancel. `AimSurfaceCameras`
	// is where the exemption is applied and argued.
	//
	// **A pane seen edge-on subtends no pixels, and its reflection is
	// degenerate before it gets there.** This is the fix for D00027 and it is
	// a decision about what a surface view *means* rather than a correction
	// to the arithmetic: at the crossing there is no continuous answer to aim
	// for. `facing` is +1 on one side of the plane and -1 on the other, both
	// are right, and no path connects them - so the reflected camera turns
	// 180 degrees between two frames, which is the flash.
	//
	// **Skipping the crossing and holding the last transform was tried and
	// does not work**: it moves the flip one frame later rather than removing
	// it. What removes it is that nothing is drawn across the band, so the
	// two orientations either side are never seen in consecutive frames.
	//
	// The same 0.3 as the near-plane margin below, and deliberately the same
	// number: inside it the reflected camera is nearer its own glass than the
	// margin that exists to stop the near plane z-fighting the pane, so every
	// part of this construction is already degenerate there.
	//
	// **What it does not cover, stated rather than discovered later:** a
	// viewer crossing the plane fast enough to step over the whole band
	// between two frames still sees the flip. The band is a distance and the
	// step is a speed, so no width closes that for every speed.
	constexpr float EDGE_ON_MARGIN = 0.3f;

	// Half a turn, for the rotation that makes a portal a hole.
	constexpr float PI = 3.14159265358979323846f;

	// The face a surface camera projects off, in the world.
	//
	// **One derivation, because there are two readers of it.** The aim pass
	// needs the plane to mirror the eye through and the marker pass needs
	// the same plane to lay a bar on; two walks that each worked out where
	// a face is would be `ReachOf`'s complaint one level up - a marker
	// drawn on a face the camera was not actually projecting off is a
	// debugging aid that lies, which is worse than none.
	struct Face {
		// The part the face belongs to, which is what gets told which
		// surface it shows.
		Entity Part;

		// The part's own placement, for the marker's rotation.
		CFrame Placement;

		// The face's outward normal, rotated into the world. Unit.
		Vector3 Normal;

		// The middle of the face, in the world.
		Vector3 Centre;

		// The part's half-extent, in its own axes.
		Vector3 HalfExtent;
	};

	// Defined in SurfaceCameras.cpp, where each is documented.
	float ReachOf(const Bounds &bounds, NormalId face);
	Vector3 UpFor(const Vector3 &forward);
	CFrame FaceFrame(const CFrame &part, const Vector3 &centre, const Vector3 &localNormal);
	float ScaleBetween(
		const Vector3 &first, const Vector3 &second, const Vector3 &farFirst, const Vector3 &farSecond
	);
	void FaceAxes(
		const CFrame &placement, const Vector3 &local, const Vector3 &half, Vector3 &first, Vector3 &second
	);
	bool FaceOf(Store &store, Entity camera, NormalId face, Face &out);
	const Portal *LinkedPortalOf(Store &store, Entity camera);

	// Defined in SurfaceCameraSeams.cpp, where each is documented.
	std::vector<PortalSeam> &Seams();
	void GatherSeams(Store &store, std::vector<PortalSeam> &seams, bool includePaths = true);
	bool CrossingOf(
		const PortalSeam &hole, const Vector3 &was, const Vector3 &now, SeamTransform &through, float &share
	);
	const PortalSeam *NearestCrossing(
		const std::vector<PortalSeam> &holes, const Vector3 &from, const Vector3 &to, PortalHop &hop
	);
	void
	SetCutPlanes(SeamCut &cut, const PortalSeam &seam, const SeamTransform &through, const Vector3 &normal);
}
