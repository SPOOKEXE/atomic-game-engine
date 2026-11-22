// Portal seam geometry: gathering linked panes as seams, the map through a
// seam, distances and near planes against seams, crossings and cut planes.

#include "cameras/SurfaceCamerasInternal.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/CameraContinuation.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>
#include <engine/scene/cameras/ViewEye.hpp>
#include <engine/scene/characters/Characters.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/environment/Sunlight.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Vector3;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::Store;
		using surface_camera_detail::CrossingOf;
		using surface_camera_detail::EDGE_ON_MARGIN;
		using surface_camera_detail::Face;
		using surface_camera_detail::FaceAxes;
		using surface_camera_detail::FaceFrame;
		using surface_camera_detail::FaceOf;
		using surface_camera_detail::GatherSeams;
		using surface_camera_detail::PI;
		using surface_camera_detail::ReachOf;
		using surface_camera_detail::ScaleBetween;
		using surface_camera_detail::Seams;
		using surface_camera_detail::SetCutPlanes;
		using surface_camera_detail::UpFor;
	}

	namespace surface_camera_detail {
		// Scratch for the passes that gather seams every tick, kept between them
		// so a scene with a portal in it stops allocating. Thread-local for
		// `Ordered()`'s reason.
		std::vector<PortalSeam> &Seams() {
			static thread_local std::vector<PortalSeam> seams;
			return seams;
		}

		// Every linked portal in the world, as a rectangle and a destination
		// frame. The public `GatherPortalSeams` is this into a caller's vector.
		void GatherSeams(Store &store, std::vector<PortalSeam> &seams, bool includePaths) {
			seams.clear();

			store.Each<const Portal, const SurfaceCamera>([&](Entity entity,
															  const Portal &portal,
															  const SurfaceCamera &camera) {
				if (!portal.Enabled || portal.Destination == NULL_ENTITY ||
					!store.Alive(portal.Destination)) {
					// An unlinked portal falls back to a mirror, and a mirror
					// is a wall. Walking into one is walking into a wall.
					return;
				}

				// **The pane is the camera's parent, not the camera.** A
				// `SurfaceCamera` is a `PVInstance` - it has a placement and
				// no size at all - so a pass that read the camera's own
				// `Bounds` found none and every portal in the world quietly
				// had no hole in it. `FaceOf` is what `AimSurfaceCameras`
				// uses and is the one answer to "which rectangle is this
				// camera projecting off".
				Face face;
				if (!FaceOf(store, entity, camera.Face, face)) {
					return;
				}

				const Transform *far = store.Get<Transform>(portal.Destination);
				const Bounds *farBounds = store.Get<Bounds>(portal.Destination);
				if (far == nullptr || farBounds == nullptr) {
					return;
				}

				// The far pane's own face, chosen the way the source's was:
				// `SurfaceCamera::Face` names one side of a part, and the far
				// end of a hole is the matching side of the part it leads to.
				const Vector3 local = NormalOf(camera.Face);
				const Vector3 farNormal = far->Frame.VectorToWorldSpace(local).Unit();
				const Vector3 farCentre = far->Frame.Position + farNormal * ReachOf(*farBounds, camera.Face);

				PortalSeam seam;
				seam.Centre = face.Centre;
				seam.Normal = face.Normal;
				FaceAxes(face.Placement, local, face.HalfExtent, seam.First, seam.Second);
				seam.RimThickness = portal.RimThickness;
				const float firstLength = seam.First.Magnitude();
				const float secondLength = seam.Second.Magnitude();
				if (!std::isfinite(seam.RimThickness) || seam.RimThickness < 0 ||
					seam.RimThickness >= firstLength || seam.RimThickness >= secondLength) {
					return;
				}
				if (seam.RimThickness > 0) {
					seam.First = seam.First * ((firstLength - seam.RimThickness) / firstLength);
					seam.Second = seam.Second * ((secondLength - seam.RimThickness) / secondLength);
				}
				seam.Up = face.Placement.VectorToWorldSpace(UpFor(local));
				seam.Destination = FaceFrame(far->Frame, farCentre, local);

				// **How much bigger the far end is, from the two rectangles
				// themselves.** The destination's face is measured exactly
				// the way the source's was - same face id, same `FaceAxes` -
				// so a pair that is the same size cannot come out as
				// anything but one, and nothing has to be authored.
				Vector3 farFirst;
				Vector3 farSecond;
				FaceAxes(far->Frame, local, farBounds->HalfExtent, farFirst, farSecond);
				seam.Scale = ScaleBetween(seam.First, seam.Second, farFirst, farSecond);

				// **The pane, which is the camera's parent and not the
				// camera.** Everything that reads a seam back has to be able
				// to leave the surface itself alone, and only the walk that
				// found it knows which entity that was.
				seam.Pane = store.ParentOf(entity);
				seam.Camera = entity;
				seam.Far = portal.Destination;
				if (includePaths) {
					seam.PanePath = store.GetFullName(seam.Pane);
					seam.FarPath = store.GetFullName(seam.Far);
				}
				seam.Surface = camera.Surface;
				seam.TagFilter = camera.TagFilter;
				seam.Crosses = portal.DestinationWorld.IsValid();
				seam.DestinationWorld = portal.DestinationWorld;
				seam.Bidirectional = portal.Bidirectional;

				seams.push_back(seam);
			});
		}

		// Whether a segment goes through one hole, and what carries it there.
		//
		// @param hole    The pane to test against.
		// @param was     Where the segment starts.
		// @param now     Where it ends.
		// @param through The map from this side to the far side, written only
		//                when the answer is true.
		bool CrossingOf(
			const PortalSeam &hole,
			const Vector3 &was,
			const Vector3 &now,
			SeamTransform &through,
			float &share
		) {
			const auto finite = [](const Vector3 &value) {
				return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
			};
			if (!finite(was) || !finite(now) || !finite(hole.Centre) || !finite(hole.Normal) ||
				!finite(hole.First) || !finite(hole.Second) || hole.First.Dot(hole.First) <= 0.0f ||
				hole.Second.Dot(hole.Second) <= 0.0f || hole.Normal.Dot(hole.Normal) <= 0.0f ||
				!std::isfinite(hole.Scale) || hole.Scale <= 0.0f) {
				return false;
			}
			// Signed distance either side of the pane's plane. **A crossing is a
			// change of side and not a place**, which is what makes the test
			// work at speed: a character walks a quarter of a metre a tick, so
			// "is it inside the pane now" misses the tick it was on either side
			// of.
			//
			// **Either direction.** A pane is a hole rather than a one-way door,
			// and which side is "outward" is a question about the crosser -
			// `AimSurfaceCameras` answers the same question about the viewer, in
			// the same way, in the same file.
			const float from = (was - hole.Centre).Dot(hole.Normal);
			const float to = (now - hole.Centre).Dot(hole.Normal);
			if ((from > 0.0f) == (to > 0.0f)) {
				return false;
			}

			// Where the segment met the plane. The denominator cannot be zero:
			// the two signs differ, so they differ by something.
			share = from / (from - to);
			const Vector3 at = was + (now - was) * share;

			// Inside the rectangle, measured along its own half-axes.
			// **Normalised by the square of each axis' length**, which is the
			// projection without a square root - `a·b / b·b` is how far along
			// `b` the point is, in units of `b`.
			const Vector3 offset = at - hole.Centre;
			const float alongFirst = offset.Dot(hole.First) / hole.First.Dot(hole.First);
			const float alongSecond = offset.Dot(hole.Second) / hole.Second.Dot(hole.Second);
			if (std::abs(alongFirst) > 1.0f || std::abs(alongSecond) > 1.0f) {
				return false;
			}

			// The crosser's own side, which is what `facing` is in
			// `AimSurfaceCameras` - a pane walked into from behind maps through
			// the frame that faces backwards, and whatever went in comes out of
			// the destination the same way round.
			through = SeamMapping(hole);
			return true;
		}

		// Which hole a segment goes through, out of all of them.
		//
		// **The nearest, and not the first one gathered.** A segment that meets
		// two panes has to go through the one it reaches first, and the gather
		// order is archetype order - which moves the moment anything in the
		// world changes a component set. So "the first that matches" is not a
		// rule at all: it is whichever pane the store happened to list first
		// this frame, and it can differ between two frames in which nothing
		// moved.
		//
		// **One function because two disagreeing is what a flipped camera is.**
		// The body's crossing and the third-person camera arm's are the same
		// question asked about two ends of the same rig, and they used to be two
		// loops - this one picking the nearest and `CrossPortals` picking the
		// first. On any frame the two chose differently, the body went through
		// one hole and the eye through another: the view snapped somewhere the
		// character was not, on some crossings and not others, with nothing
		// about the crossing to distinguish them. Intermittent by construction,
		// because the thing that decided it was archetype order.
		//
		// @param holes The gathered seams.
		// @param from  Where the segment starts.
		// @param to    Where it ends.
		// @param hop   Filled when one is found, untouched otherwise.
		// @return The seam that was crossed, or null. Returned rather than
		//         looked up again from `PortalHop::Pane`, because a caller that
		//         needs the rectangle - a body does, to be put down clear of it
		//         - would otherwise search for the answer this already had.
		const PortalSeam *NearestCrossing(
			const std::vector<PortalSeam> &holes, const Vector3 &from, const Vector3 &to, PortalHop &hop
		) {
			size_t index = 0;
			return NearestPortalCrossing(holes, from, to, false, hop, index) ? &holes[index] : nullptr;
		}

		void SetCutPlanes(
			SeamCut &cut, const PortalSeam &seam, const SeamTransform &through, const Vector3 &normal
		) {
			cut.NearNormal = normal;
			cut.NearOffset = seam.Centre.Dot(normal);
			cut.FarNormal = through.Rotate(normal) * -1.0f;
			cut.FarOffset = through.Point(seam.Centre).Dot(cut.FarNormal);
		}
	}

	bool NearestPortalCrossing(
		std::span<const PortalSeam> seams,
		const Vector3 &from,
		const Vector3 &to,
		bool includeForeign,
		PortalHop &hop,
		size_t &seamIndex
	) {
		bool found = false;
		float nearest = 1.0f;
		for (size_t index = 0; index < seams.size(); ++index) {
			const PortalSeam &seam = seams[index];
			if ((!includeForeign && seam.Crosses) || (!seam.Bidirectional && SeamOffset(seam, from) < 0.0f)) {
				continue;
			}
			SeamTransform through;
			float share = 1.0f;
			if (!CrossingOf(seam, from, to, through, share) || (found && share >= nearest)) {
				continue;
			}
			found = true;
			nearest = share;
			seamIndex = index;
			hop = PortalHop{through, share, seam.Pane, seam.Far};
		}
		return found;
	}

	SeamTransform SeamMapping(const PortalSeam &seam) {
		const Vector3 up = seam.Up.Dot(seam.Up) > 0 ? seam.Up : UpFor(seam.Normal);
		const CFrame source = CFrame::LookAt(seam.Centre, seam.Centre + seam.Normal, up);

		// `destination · half-turn · source⁻¹`, the same product the camera goes
		// through - see `AimSurfaceCameras`' `linked` branch for why the
		// half-turn is what makes it a hole rather than a window onto a copy.
		//
		// **One map, and picking it by which side the crosser is on was a bug.**
		// The source frame used to flip with the side while the destination
		// stayed put, which makes two maps that are not each other's inverse:
		// both of them send a crosser to the *same* side of the far pane, so a
		// body that walks in the back of a hole comes out where one that walked
		// in the front does, and walking back through returns it to the front.
		// A round trip that starts from behind therefore lands somewhere else,
		// turned by whatever angle the pair turns through - which is the report
		// about a character snapping to a heading it never entered from.
		//
		// With the source fixed, the product is one rigid map that carries this
		// pane's front hemisphere to the far pane's back one and its back to the
		// far pane's front, and the far pane's own map is exactly its inverse.
		// CodeParade's `Portal::Connect` writes one `delta` per side and they
		// are the same matrix, for this reason; `Portal::Draw` re-picks `front`
		// or `back` only to know which hole to skip.
		//
		// **The scale is about the source pane's centre**, which this rigid
		// product already sends to the destination's centre - so scaling before
		// the map and scaling after it are the same operation, and only the
		// centre this pass already has is needed to state it.
		SeamTransform through;
		through.Frame = seam.Destination * CFrame::Angles(0.0f, PI, 0.0f) * source.Inverse();
		through.Origin = seam.Centre;
		through.Scale = seam.Scale;
		return through;
	}

	float SeamOffset(const PortalSeam &seam, const Vector3 &at) {
		return (at - seam.Centre).Dot(seam.Normal);
	}

	float
	RectangleDistance(const Vector3 &centre, const Vector3 &first, const Vector3 &second, const Vector3 &at) {
		const Vector3 offset = at - centre;

		// How far along each half-axis, in units of that axis, clamped to the
		// edge. **`a·b / b·b` rather than a normalise**, which is the projection
		// without a square root and is what makes the clamp read as `-1..1`.
		const float firstSquared = first.Dot(first);
		const float secondSquared = second.Dot(second);
		const float alongFirst =
			firstSquared > 0.0f ? std::clamp(offset.Dot(first) / firstSquared, -1.0f, 1.0f) : 0.0f;
		const float alongSecond =
			secondSquared > 0.0f ? std::clamp(offset.Dot(second) / secondSquared, -1.0f, 1.0f) : 0.0f;

		const Vector3 closest = first * alongFirst + second * alongSecond;
		return (offset - closest).Magnitude();
	}

	float SeamDistance(const PortalSeam &seam, const Vector3 &at) {
		return RectangleDistance(seam.Centre, seam.First, seam.Second, at);
	}

	float NearestSeamDistance(ecs::Store &store, const Vector3 &at) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);

		float nearest = std::numeric_limits<float>::infinity();
		for (const PortalSeam &seam : seams) {
			nearest = std::min(nearest, SeamDistance(seam, at));
		}
		return nearest;
	}

	float PortalNearPlane(float authored, float nearestSeam) {
		if (!(authored > 0.0f)) {
			// A camera with no usable near plane is not one this can rescue, and
			// substituting one would hide the authoring mistake behind a portal.
			return authored;
		}
		if (!(nearestSeam < std::numeric_limits<float>::infinity())) {
			// No holes in this world, which is nearly every world.
			return authored;
		}

		// A fixed floor clips script cameras that approach closer than that floor.
		// Keep half the positive distance; a coplanar eye needs a nonzero fallback.
		const float halfDistance = nearestSeam * 0.5f;
		return std::min(authored, halfDistance > 0.0f ? halfDistance : PORTAL_NEAR_MIN);
	}

	float PortalClipBias(float nearestSeam) {
		// The same halving as the near plane, and capped by the same margin that
		// says how wide a band around a pane is degenerate. Beyond that the
		// camera is far enough away that a fixed slab is invisible.
		if (!(nearestSeam < std::numeric_limits<float>::infinity())) {
			return 0.0f;
		}
		return std::min(nearestSeam * 0.5f, EDGE_ON_MARGIN);
	}

	bool SeamCarries(const PortalSeam &seam, const Vector3 &at) {
		if (SeamOffset(seam, at) >= 0.0f) {
			return false;
		}

		const float firstLength = std::sqrt(seam.First.Dot(seam.First));
		const float secondLength = std::sqrt(seam.Second.Dot(seam.Second));
		if (firstLength <= 0.0f || secondLength <= 0.0f) {
			return false;
		}

		const Vector3 offset = at - seam.Centre;

		return std::abs(offset.Dot(seam.First) / firstLength) <= firstLength &&
			   std::abs(offset.Dot(seam.Second) / secondLength) <= secondLength;
	}

	bool SeamStraddled(const PortalSeam &seam, const Vector3 &at, float reach) {
		const float firstLength = std::sqrt(seam.First.Dot(seam.First));
		const float secondLength = std::sqrt(seam.Second.Dot(seam.Second));
		if (firstLength <= 0.0f || secondLength <= 0.0f) {
			return false;
		}

		// **No size rule here, and the one that was here was a bug.** It refused
		// anything whose reach exceeded the pane's shorter half-axis, which
		// sounds like "bigger than the hole" and is not: a person is very nearly
		// as big as the doorway they walk through. A five-stud character has a
		// reach of about three and a four-by-five doorway a shorter half-axis of
		// two, so every character in every hole was refused - no clone in the
		// far room, no half-body in the picture, which is exactly the artefact
		// the pass exists to remove.
		//
		// What that rule was standing in for is "is this the room rather than a
		// thing in it", and the answer to that is which query found it:
		// `CloneThroughSeams` walks bodies that can move and character limbs, so
		// a floor is never a candidate. `AppendPortalGhosts` reads a draw list
		// and cannot ask, so it keeps a size guard of its own - a much looser
		// one, stated against the pane's diagonal.
		if (std::abs(SeamOffset(seam, at)) >= reach) {
			return false;
		}

		// Inside the rectangle, measured along its own half-axes and widened by
		// the body's own reach - the same projection `CrossingOf` makes, plus
		// the slack a body has that a point does not. **Widened rather than
		// exact**, because a clone that appears a moment early is invisible
		// (it is behind the pane) and one that appears a moment late is a body
		// visibly cut in half, which is the artefact this exists to remove.
		const Vector3 offset = at - seam.Centre;

		const float alongFirst = std::abs(offset.Dot(seam.First) / firstLength);
		const float alongSecond = std::abs(offset.Dot(seam.Second) / secondLength);

		return alongFirst < firstLength + reach && alongSecond < secondLength + reach;
	}
	SeamCut CutOfSeam(
		const PortalSeam &seam, const SeamTransform &through, const CFrame &frame, const Vector3 &halfExtent
	) {
		SeamCut cut;

		// The original owns the hemisphere containing its body. Mapping its
		// complement gives the clone's hemisphere for either entrance face.
		const float side = (frame.Position - seam.Centre).Dot(seam.Normal) < 0 ? -1.0f : 1.0f;
		SetCutPlanes(cut, seam, through, seam.Normal * side);

		// How far the body reaches across the hole, on the pane's own axes.
		const float firstLength = std::sqrt(seam.First.Dot(seam.First));
		const float secondLength = std::sqrt(seam.Second.Dot(seam.Second));
		if (firstLength <= 0.0f || secondLength <= 0.0f) {
			return cut;
		}

		const Vector3 first = seam.First / firstLength;
		const Vector3 second = seam.Second / secondLength;

		// The support of an oriented box along a unit direction: the three
		// half-extents, each weighted by how much of that axis points along it.
		const Vector3 right = frame.RightVector();
		const Vector3 up = frame.UpVector();
		const Vector3 back = frame.LookVector() * -1.0f;

		const auto span = [&](const Vector3 &along) {
			return std::abs(halfExtent.X * along.Dot(right)) + std::abs(halfExtent.Y * along.Dot(up)) +
				   std::abs(halfExtent.Z * along.Dot(back));
		};

		const Vector3 offset = frame.Position - seam.Centre;

		// **The rim is part of the aperture.** A character standing in the
		// opening puts its lowest limbs exactly on the bottom edge, and an inward
		// margin rejects only those limbs while the rest of the rig crosses. The
		// small outward tolerance absorbs transform round-off; a real overhang is
		// still refused.
		//
		// A row filling both axes is the pane itself in a world whose camera has
		// not assigned surface slots yet. It is the one equality that must not
		// cross: its copy lands on the partner pane and z-fights it. Contact on
		// only one rim is ordinary doorway geometry and remains valid.
		constexpr float SNUG = 1.0e-3f;

		// **And it has to have two halves to be cut into**, which is the same
		// support measured along the normal instead of across it. A face marker
		// is a bar three hundredths of a stud thick lying on the pane's own
		// face: it straddles the plane by the loose sphere test, fits the
		// rectangle easily, and has no far half at all - cutting it in two
		// yields nothing and a copy of nothing. `SeamStraddled` asks the same
		// question of a bounding sphere, which is the conservative form; this is
		// the exact one, and it is free here because the support is already
		// built.
		const float firstReach = std::abs(offset.Dot(first)) + span(first);
		const float secondReach = std::abs(offset.Dot(second)) + span(second);
		const bool fillsPane =
			std::abs(firstReach - firstLength) <= SNUG && std::abs(secondReach - secondLength) <= SNUG;

		// **Of the whole body rather than of this row, when the two differ.**
		// The caller passes a rig's box for a limb - see `FitBoxOf` - because
		// this is decided per drawn row and a character is a dozen of them: a
		// rig standing in an opening has its root and its head comfortably
		// inside the rectangle and its feet sitting on the bottom edge, where
		// the box overhangs by a few centimetres. Asked of each row, that
		// answers yes for most of the rig and no for the parts on the rim - and
		// a row that answers no is drawn *whole* rather than cut, so the body is
		// clipped at the plane from the waist up and pushed through the wall
		// from the ankles down. That is "character split in half", and it is a
		// question about the body that the row cannot answer.
		//
		// The rule itself is unchanged and has to be. What it refuses is the
		// room: a floor slab or a wall beside the pane straddles the plane by
		// the sphere test and reaches metres past the rectangle, and a copy of
		// one landing in the room next door is the artefact it exists for.
		cut.Fits = span(seam.Normal) > std::abs(offset.Dot(seam.Normal)) + SNUG &&
				   firstReach <= firstLength + SNUG && secondReach <= secondLength + SNUG && !fillsPane;

		return cut;
	}
	size_t GatherPortalSeams(Store &store, std::vector<PortalSeam> &seams) {
		GatherSeams(store, seams);
		return seams.size();
	}
}
