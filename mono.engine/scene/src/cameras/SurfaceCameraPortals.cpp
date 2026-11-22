// Portal drawing: far-side clones of rows standing in a seam, cutting rows at
// seams, opening portals and the portal body view.

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
		using surface_camera_detail::GatherSeams;
		using surface_camera_detail::LinkedPortalOf;
		using surface_camera_detail::NearestCrossing;
		using surface_camera_detail::Seams;
		using surface_camera_detail::SetCutPlanes;

		// Controller clearance keeps the eye off the oblique plane. Script cameras
		// may approach closer; PortalNearPlane adapts to their actual distance.
		constexpr float SEAM_TOUCH = 2.0f * PORTAL_NEAR_MIN;

		// How close a far-side copy may land to its own original before it is
		// not worth drawing, in studs.
		//
		// **A copy on top of its original is a duplicate rather than a far
		// half**, and two coplanar surfaces at one depth is a stripe of
		// flickering colour. That happens when the map is the identity for the
		// thing being copied - a pane paired with itself, or a pair arranged so
		// the map fixes whatever stands beside them.
		//
		// **Tiny rather than the body's own reach, which is what this was
		// first.** Measuring against the reach reads as "the copy overlaps the
		// original" and is far too strong: two rooms laid out next to each other
		// with a hole between them move a body a few studs, which is a real
		// crossing into a real other room, and refusing that clone puts the
		// artefact back. What has to be caught is the degenerate map, and a
		// degenerate map moves nothing at all.
		//
		// **A question about one list, and asking it of two was a feature
		// lost.** A pair laid out the same way at both ends - every authored
		// pair, and what makes a hole read as an opening - has a map that is a
		// half-turn about the pane's own axis, so a body standing dead centre in
		// the doorway comes out at its own coordinates, turned right around.
		// Inside one world that copy is on top of its original and must go;
		// across two worlds it is in a different space and is the most important
		// crossing there is. `AppendPortalClones` therefore does not ask, and
		// while it did, a character in the middle of a cross-world hole was cut
		// off at the plane with nothing beyond it.
		constexpr float COINCIDENT_COPY = 0.05f;

		// Whether one rig may be cut by one hole.
		//
		// **Because "does this fit the aperture" is a question about a body**,
		// and a character is a dozen drawn rows. See `DrawInstance::Rig`. A rig
		// standing in an opening has its root and its head comfortably inside
		// the rectangle and its feet sitting on the bottom edge, where the box
		// overhangs by a few centimetres. Asked of each row, that answers yes
		// for most of the rig and no for the parts on the rim - and a row that
		// answers no is drawn *whole* rather than cut, so the body is clipped at
		// the plane from the waist up and pushed through the wall from the
		// ankles down. That is "character split in half".
		//
		// **Any row, not every row.** The union of a rig's boxes is bigger than
		// every one of them, so a rule stated against it refuses the whole rig
		// the moment one toe crosses the rim - which ends the split by drawing
		// nothing through the hole at all. What is wanted is the opposite: a rig
		// with a part inside the opening is a body walking through it, and the
		// few centimetres of foot that hang past the rim are worth less than the
		// body being in one piece.
		//
		// Nothing else is affected. A row that names no rig is one thing, keeps
		// its own answer, and the room-sized slab this rule exists to refuse is
		// not a character.
		struct RigFit {
			uint64_t Rig = 0;
			core::Name World;
			Vector3 Owner;
			bool Fits = false;
			Vector3 EntryNormal{};
			bool Continuing = false;
		};

		// Collects, for one hole, which rigs in a draw list may be cut by it.
		void GatherRigFits(
			const Store &store,
			const PortalSeam &seam,
			const SeamTransform &through,
			std::span<const DrawInstance> rows,
			std::vector<RigFit> &fits
		) {
			fits.clear();
			for (const DrawInstance &row : rows) {
				if (row.Rig == 0) {
					continue;
				}

				const bool fitted = CutOfSeam(seam, through, row.Frame, row.HalfExtent).Fits;

				RigFit *found = nullptr;
				for (RigFit &candidate : fits) {
					if (candidate.Rig == row.Rig && candidate.World == row.SourceWorld) {
						found = &candidate;
						break;
					}
				}
				if (found == nullptr) {
					Vector3 owner = row.Frame.Position;
					const bool local = !row.SourceWorld.IsValid() || row.SourceWorld.Text() == store.Name();
					const auto *view = store.Resource<PortalBodyView>();
					// The presented root owns the rig's cut side when its rows were
					// predicted ahead of the authoritative Transform. Foreign ids do
					// not resolve in this store.
					if (local) {
						if (view && view->Root.Id == row.Rig)
							owner = view->Previous;
						else if (const Transform *root = store.Get<Transform>(Entity{row.Rig}))
							owner = root->Frame.Position;
					}
					RigFit fit{row.Rig, row.SourceWorld, owner, fitted};
					if (local && view && view->Root.Id == row.Rig && view->Crossing &&
						view->Crossing->Pane == seam.Pane && view->Crossing->Camera == seam.Camera &&
						view->Crossing->DestinationWorld == seam.DestinationWorld) {
						fit.Fits = true;
						fit.EntryNormal = view->EntryNormal;
						fit.Continuing = true;
					}
					fits.push_back(fit);
					continue;
				}
				found->Fits = found->Fits || fitted;
			}
		}

		// The rig's answer for this row, or null when the row is its own body.
		//
		// A vector and a linear find rather than a map: a world has a handful of
		// characters in it, this runs once a frame, and hashing an entity id per
		// row costs more than reading five of them.
		const RigFit *RigAnswer(const std::vector<RigFit> &fits, const DrawInstance &row) {
			if (row.Rig == 0) {
				return nullptr;
			}
			for (const RigFit &fit : fits) {
				if (fit.Rig == row.Rig && fit.World == row.SourceWorld) {
					return &fit;
				}
			}
			return nullptr;
		}

		// The far half of one drawn row, cut to the far side of the hole.
		//
		// **One rule for both sides of the file**, which is the whole reason it
		// is a function. The same-world pass reads a draw list and cuts the
		// original as well; the cross-world pass appends into another world's
		// list and cannot touch the original. Everything *between* those two
		// facts - what may cross, what it becomes, and where it is cut - is
		// identical, and was not: the cross-world copy used to be built by hand
		// from an entity walk, uncut, un-lit and restricted to bodies carrying
		// `Motion` or `CharacterLimb`. So an anchored crate resting in a
		// cross-world seam had no far half at all, a mesh had no far half, and
		// what did get copied was drawn whole on both sides.
		//
		// @param seam  The hole.
		// @param through The seam's map, resolved once by the caller.
		// @param row   The row as it will be drawn.
		// @param light The world's sun direction, already carried through the
		//        seam, so the two halves shade alike.
		// @param cut   Filled with both planes when this returns true.
		// @param ghost Filled with the far half when this returns true.
		// @return Whether this row has a far half through this seam.
		bool FarHalfOfRow(
			const PortalSeam &seam,
			const SeamTransform &through,
			const DrawInstance &row,
			const Vector3 &light,
			const RigFit *rigFits,
			SeamCut &cut,
			DrawInstance &ghost
		) {
			// **A pane is the hole rather than a thing in it.** A copy of it
			// mapped through its own pairing lands exactly on the far pane -
			// z-fighting a wall with a picture on it - and it is the one row
			// that always passes the fit below, since it *is* the rectangle.
			if (row.Surface >= 0) {
				return false;
			}

			// **Nothing invisible without graph work has a far half worth drawing**,
			// and a cross-world pair puts exactly such a row in the seam: the
			// destination stand-in is pane-sized, centred on the plane and
			// authored invisible, so it fits the hole perfectly and would be
			// copied every frame for nothing. An invisible effect carrier still
			// crosses because its graph work can produce visible pixels.
			if (row.Transparency >= 1.0f && row.Effects.Count == 0) {
				return false;
			}

			// **One hole per body, carried across passes.** A row already cut by
			// some *other* pane is a half of something, and half a body copied
			// through a second hole is a quarter nobody asked for. A row cut by
			// this one is exactly what this is here for - which is what the far
			// world's own rows arrive as.
			const float already = row.SeamNormal.Dot(row.SeamNormal);
			if (already > 0.0f && (std::abs(row.SeamNormal.Dot(seam.Normal)) < .999f ||
								   std::abs(row.SeamOffset - seam.Centre.Dot(row.SeamNormal)) > .001f)) {
				return false;
			}

			// A sphere around the row rather than its oriented box, which is
			// conservative in the only direction that costs nothing: a false
			// positive is a copy the oblique clip throws away, and a false
			// negative is the half a body this whole pass exists to draw.
			if (!(rigFits && rigFits->Continuing) &&
				!SeamStraddled(seam, row.Frame.Position, row.HalfExtent.Magnitude())) {
				return false;
			}

			// **What may be copied is what fits through the hole.** A body that
			// does not fit cannot be cut by a single plane without slicing the
			// part of it that hangs past the rim, where the hole is not - such a
			// body is drawn whole on both sides instead.
			// **The planes come from the seam and the answer may come from the
			// rig.** `CutOfSeam` derives both from one call, and for a limb the
			// `Fits` half of it is replaced by the body's - see `RigFit`, which
			// is why a character is admitted or refused in one piece.
			cut = CutOfSeam(seam, through, row.Frame, row.HalfExtent);
			if (rigFits != nullptr) {
				cut.Fits = rigFits->Fits;
				const float side = (rigFits->Owner - seam.Centre).Dot(seam.Normal) < 0 ? -1.0f : 1.0f;
				SetCutPlanes(
					cut, seam, through, rigFits->Continuing ? rigFits->EntryNormal : seam.Normal * side
				);
			}
			if (already > 0) {
				SetCutPlanes(cut, seam, through, row.SeamNormal);
			}
			if (!cut.Fits) {
				return false;
			}

			ghost = row;
			ghost.Variant = PortalVariant(row.Variant, seam.Pane.Id);
			ghost.Frame = through.Place(row.Frame);
			ghost.HalfExtent = row.HalfExtent * through.Scale;

			// **Never a surface**, because a copy that sampled a surface slot
			// would put the far room's picture on a copy of a character.
			ghost.Surface = -1;

			// **The two halves, and this is the whole of the cut.** Each keeps
			// its owning side of the pane; `CutOfSeam` derives the far plane by
			// mapping the complement of the near one, so their union is the body
			// and their intersection is empty.
			ghost.SeamNormal = cut.FarNormal;
			ghost.SeamOffset = cut.FarOffset;
			ghost.SeamFirst = through.Rotate(seam.First) * through.Scale;
			ghost.SeamSecond = through.Rotate(seam.Second) * through.Scale;
			ghost.SeamCentre = through.Point(seam.Centre);
			ghost.SeamMask = 2;
			if (seam.Crosses && rigFits && rigFits->Continuing) {
				const float support =
					std::abs(cut.NearNormal.Dot(row.Frame.RightVector())) * row.HalfExtent.X +
					std::abs(cut.NearNormal.Dot(row.Frame.UpVector())) * row.HalfExtent.Y +
					std::abs(cut.NearNormal.Dot(row.Frame.LookVector())) * row.HalfExtent.Z;
				const float maximumDepth = row.Frame.Position.Dot(cut.NearNormal) - cut.NearOffset + support;
				// A verified crossing owns the whole far row after it clears the
				// plane. Its later movement is no longer bounded by the aperture.
				if (maximumDepth < 0.0f) ghost.SeamMask = 0;
			}

			// **And it is lit by the sun this side of the hole sees.** The
			// copy's normals are the original's turned by the seam's rotation,
			// so shading them with the far world's own light gives one body lit
			// by two suns a quarter apart - a bright face meeting an olive one
			// down the middle of a crate. `R · L` makes `dot(R n, R L)` equal
			// `dot(n, L)` for every normal, so the two halves shade identically
			// and the join stops being visible.
			ghost.SeamLight = light;
			return true;
		}
	}

	// The far half of every drawn row standing in one named pane, appended to a
	// list this store does not own.
	//
	// **A draw list in and a draw list out, which is what put the two halves of
	// this file back in step.** This used to be an entity walk over bodies
	// carrying `Motion` or `CharacterLimb`, because "what goes through a portal
	// is what can move" - and that rule was already retired on the same-world
	// side, where an anchored crate resting in a seam is as much a thing
	// standing in the hole as anything that walked there. So an anchored part in
	// a cross-world seam had no far half, and neither did anything drawn from a
	// row rather than an entity.
	//
	// **And the copy is cut now.** The note here used to argue that a
	// cross-world pane is a window rather than a hole, so nothing straddles it -
	// which is exactly the report: a body in the doorway drawn whole on this
	// side and whole again on the far one, joined nowhere. `FarHalfOfRow` gives
	// it the same two complementary planes a same-world copy gets.
	//
	// The original is not cut here and does not need to be: it is a row in
	// *this* world's list, and `CutAndCloneSeams` cuts it there - including for
	// a crossing seam, which is the half of that pass this one relies on.
	static size_t CloneThroughSeams(
		Store &store,
		std::span<const PortalSeam> seams,
		std::span<const DrawInstance> source,
		std::vector<DrawInstance> &out
	) {
		ENGINE_PROFILE("clone portal seams");

		if (seams.empty() || source.empty()) {
			return 0;
		}

		const Vector3 sun = SunOf(store).Direction;
		size_t appended = 0;

		// One answer per rig per hole, taken before the walk. Inside it this
		// would be a pass over the whole list per row. See `RigFit`.
		// Each seam's map and the sun through it, once per seam rather than
		// once per row and seam: they depend on the seam alone.
		std::vector<std::vector<RigFit>> fits(seams.size());
		std::vector<SeamTransform> throughs(seams.size());
		std::vector<Vector3> lights(seams.size());
		for (size_t at = 0; at < seams.size(); at++) {
			throughs[at] = SeamMapping(seams[at]);
			lights[at] = throughs[at].Rotate(sun);
			GatherRigFits(store, seams[at], throughs[at], source, fits[at]);
		}

		for (const DrawInstance &row : source) {
			for (size_t at = 0; at < seams.size(); at++) {
				const PortalSeam &seam = seams[at];
				const SeamTransform &through = throughs[at];

				SeamCut cut;
				DrawInstance ghost;
				if (!FarHalfOfRow(seam, through, row, lights[at], RigAnswer(fits[at], row), cut, ghost)) {
					continue;
				}

				out.push_back(ghost);
				appended++;

				// **One seam per body.** A body inside two panes at once is a
				// body in a corner case nobody has drawn, and cloning it through
				// both would put two of it on two far sides - the same rule, for
				// the same reason, that `CrossPortals` breaks after one hole.
				break;
			}
		}

		return appended;
	}

	size_t AppendPortalClones(
		Store &store, int16_t surface, std::span<const DrawInstance> source, std::vector<DrawInstance> &out
	) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);
		std::erase_if(seams, [surface](const PortalSeam &seam) { return seam.Surface != surface; });
		return CloneThroughSeams(store, seams, source, out);
	}

	size_t AppendPortalClones(
		Store &store,
		const PortalSeam &seam,
		std::span<const DrawInstance> source,
		std::vector<DrawInstance> &out
	) {
		return CloneThroughSeams(store, std::span(&seam, 1), source, out);
	}

	bool ClearOfPanes(ecs::Store &store, Vector3 &at) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);

		for (const PortalSeam &seam : seams) {
			// Both local captures and cross-world images use mapped perspective
			// cameras. Only the oblique plane's small clearance is needed here.
			constexpr float clearance = SEAM_TOUCH;

			const float offset = SeamOffset(seam, at);
			if (std::abs(offset) >= clearance) {
				continue;
			}

			// **Inside the rectangle and not merely the plane**, which is
			// `CrossingOf`'s distinction: a plane is infinite and a pane is not,
			// and an eye level with a doorway but well to the side of it is
			// standing in a wall rather than in a hole.
			const Vector3 along = at - seam.Centre;
			const float first = along.Dot(seam.First) / seam.First.Dot(seam.First);
			const float second = along.Dot(seam.Second) / seam.Second.Dot(seam.Second);
			if (std::abs(first) > 1.0f || std::abs(second) > 1.0f) {
				continue;
			}

			// **The side it is nearer, and zero counts as behind** - the same
			// tie-break `SeamMapping` makes, so a viewpoint pushed out of a pane
			// and a body carried through one never disagree about which room
			// they are in.
			const float side = offset > 0.0f ? 1.0f : -1.0f;
			at = at + seam.Normal * (side * clearance - offset);
			return true;
		}

		return false;
	}

	size_t OpenPortals(ecs::Store &store) {
		// **Gathered before anything is written**, so one pane shared by several
		// portal cameras receives one decision and no deferred component writes.
		// A pane is open when any valid, enabled mouth on it is open. A disabled
		// mouth closes a pane that has no other live mouth, keeping collision in
		// step with rendering and crossing.
		struct Opening {
			ecs::Entity Pane;
			bool Open = false;
		};
		static thread_local std::vector<Opening> openings;
		openings.clear();

		store.Each<const Portal, const SurfaceCamera>(
			[&](ecs::Entity camera, const Portal &, const SurfaceCamera &) {
				// **The pane is the camera's parent**, which is the same
				// resolution `CrossPortals` and `AimSurfaceCameras` make - a
				// `SurfaceCamera` is a `PVInstance` with a placement and no size,
				// so it is not the thing anybody can walk into.
				const ecs::Entity pane = store.ParentOf(camera);
				if (pane == NULL_ENTITY) {
					return;
				}

				openings.push_back(Opening{pane, LinkedPortalOf(store, camera) != nullptr});
			}
		);

		std::sort(openings.begin(), openings.end(), [](const Opening &left, const Opening &right) {
			return left.Pane.Id < right.Pane.Id;
		});

		size_t opened = 0;
		std::vector<PortalSeam> seams;
		GatherSeams(store, seams);
		for (size_t first = 0; first < openings.size();) {
			size_t after = first + 1;
			bool open = openings[first].Open;
			while (after < openings.size() && openings[after].Pane == openings[first].Pane) {
				open = open || openings[after].Open;
				after++;
			}

			const ecs::Entity pane = openings[first].Pane;
			const Collider *collider = store.Get<Collider>(pane);
			if (collider == nullptr) {
				first = after;
				continue;
			}
			const Collider authored = *collider;
			const auto *rim = store.Get<PortalRim>(pane);
			if (!open && rim != nullptr) {
				for (const ecs::Entity part : rim->Parts)
					if (store.Alive(part)) store.Destroy(part);
				store.Remove<PortalRim>(pane);
			}
			if (open && !collider->Trigger && rim == nullptr) {
				const auto seam = std::find_if(seams.begin(), seams.end(), [&](const auto &candidate) {
					return candidate.Pane == pane;
				});
				if (seam != seams.end()) {
					const float apertureFirst = seam->First.Magnitude();
					const float apertureSecond = seam->Second.Magnitude();
					const float thickness = seam->RimThickness;
					if (thickness > 0) {
						const float outerFirst = apertureFirst + thickness;
						const float outerSecond = apertureSecond + thickness;
						const auto frame =
							core::CFrame::LookAt(seam->Centre, seam->Centre + seam->Normal, seam->Up);
						PortalRim created;
						const std::array<core::Vector3, 4> centres{
							core::Vector3{-outerFirst + thickness * .5f, 0, 0},
							core::Vector3{outerFirst - thickness * .5f, 0, 0},
							core::Vector3{0, -outerSecond + thickness * .5f, 0},
							core::Vector3{0, outerSecond - thickness * .5f, 0},
						};
						const std::array<core::Vector3, 4> extents{
							core::Vector3{thickness * .5f, outerSecond, collider->Extent.Z},
							core::Vector3{thickness * .5f, outerSecond, collider->Extent.Z},
							core::Vector3{apertureFirst, thickness * .5f, collider->Extent.Z},
							core::Vector3{apertureFirst, thickness * .5f, collider->Extent.Z},
						};
						for (size_t index = 0; index < created.Parts.size(); ++index) {
							created.Parts[index] = store.Create();
							store.Set(
								created.Parts[index], scene::Transform{frame * core::CFrame(centres[index])}
							);
							scene::Collider edge = authored;
							edge.Extent = extents[index];
							edge.Trigger = false;
							store.Set(created.Parts[index], edge);
						}
						store.Set(pane, created);
					}
				}
			}
			if (collider->Trigger == open) {
				first = after;
				continue;
			}

			// **`Set` rather than a write through the reference, because this
			// is a change the broad phase has to see.** `SyncBroadphase` reads
			// the row's stamp to decide whether static geometry moved, and a
			// collider that became a trigger without one would keep being solved
			// against until something else happened to touch the row. It stamps
			// only when the authored activation changes, not once per tick.
			Collider opened_ = authored;
			opened_.Trigger = open;
			store.Set(pane, opened_);
			opened += open ? 1u : 0u;
			first = after;
		}

		return opened;
	}

	void UpdatePortalBodyView(
		Store &store, Entity root, const Vector3 &position, std::span<const DrawInstance> rows
	) {
		ENGINE_PROFILE("update portal body view");
		const auto *held = store.Resource<CameraCharacterHold>();
		const bool retained = held && held->Active && held->SourceRoot == root && store.Alive(held->Root);
		if ((!store.Alive(root) && !retained) || !std::isfinite(position.X) || !std::isfinite(position.Y) ||
			!std::isfinite(position.Z)) {
			store.RemoveResource<PortalBodyView>();
			return;
		}
		auto *view = store.ResourceMutable<PortalBodyView>();
		if (!view || view->Root != root) {
			store.SetResource(PortalBodyView{root, position});
			return;
		}
		if (!view->Crossing && view->Previous == position) return;
		auto &seams = Seams();
		GatherPortalSeams(store, seams);
		std::erase_if(seams, [](const auto &seam) { return !seam.Crosses; });
		if (view->Crossing) {
			const auto &before = *view->Crossing;
			const auto current = std::find_if(seams.begin(), seams.end(), [&](const auto &seam) {
				return seam.Pane == before.Pane && seam.Camera == before.Camera && seam.Far == before.Far &&
					   seam.DestinationWorld == before.DestinationWorld && seam.Centre == before.Centre &&
					   seam.Normal == before.Normal && seam.First == before.First &&
					   seam.Second == before.Second && seam.Up == before.Up && seam.Scale == before.Scale &&
					   seam.Bidirectional == before.Bidirectional &&
					   std::memcmp(&seam.Destination, &before.Destination, sizeof(CFrame)) == 0;
			});
			if (current == seams.end() || (position - before.Centre).Dot(view->EntryNormal) >= 0)
				view->Crossing.reset();
			view->Previous = position;
			return;
		}
		PortalHop hop;
		size_t crossed = 0;
		if (NearestPortalCrossing(seams, view->Previous, position, true, hop, crossed)) {
			const auto &seam = seams[crossed];
			const Vector3 crossingPosition = view->Previous + (position - view->Previous) * hop.Share;
			const bool fits = std::any_of(rows.begin(), rows.end(), [&](const auto &row) {
				if (row.Rig != root.Id || row.Variant != 0 || row.Surface >= 0 || row.Transparency >= 1 ||
					(row.SourceWorld.IsValid() && row.SourceWorld.Text() != store.Name()))
					return false;
				auto atPlane = row.Frame;
				atPlane.Position = atPlane.Position + crossingPosition - position;
				return CutOfSeam(seam, hop.Through, atPlane, row.HalfExtent).Fits;
			});
			if (fits) {
				view->Crossing = seam;
				view->EntryNormal =
					seam.Normal * ((view->Previous - seam.Centre).Dot(seam.Normal) < 0 ? -1.f : 1.f);
			}
		}
		view->Previous = position;
	}

	bool PortalCrossing(ecs::Store &store, const Vector3 &from, const Vector3 &to, PortalHop &hop) {
		std::vector<PortalSeam> &seams = Seams();
		// A crossing returns entities and geometry, never the presentation paths.
		GatherSeams(store, seams, false);
		return NearestCrossing(seams, from, to, hop) != nullptr;
	}

	bool PortalCrossing(ecs::Store &store, const Vector3 &from, const Vector3 &to, SeamTransform &through) {
		PortalHop hop;
		if (!PortalCrossing(store, from, to, hop)) {
			return false;
		}
		through = hop.Through;
		return true;
	}

	size_t CutAndCloneSeams(ecs::Store &store, std::vector<DrawInstance> &out) {
		ENGINE_PROFILE("cut and clone seams");

		if (out.empty()) {
			return 0;
		}

		// **The same seams the crossing uses**, so a body is copied through
		// exactly the rectangle it will be moved through - two answers to "which
		// pane is this" is the kind of disagreement that shows up as a far half
		// standing somewhere its body never goes.
		std::vector<PortalSeam> seams;
		GatherSeams(store, seams);
		if (seams.empty()) {
			return 0;
		}

		const Vector3 sun = SunOf(store).Direction;
		size_t appended = 0;

		// **The list as it stands, not as it grows.** Every far half is itself a
		// draw instance in `out`, and a walk that saw its own output would copy
		// the copies - a body near two facing panes would fill the buffer.
		const size_t drawn = out.size();

		// One answer per rig per hole, taken before the walk. See `RigFit`.
		// Each seam's map and the sun through it are taken here too: they
		// depend on the seam alone, and per row they were most of the cost.
		std::vector<std::vector<RigFit>> fits(seams.size());
		std::vector<SeamTransform> throughs(seams.size());
		std::vector<Vector3> lights(seams.size());
		for (size_t at = 0; at < seams.size(); at++) {
			throughs[at] = SeamMapping(seams[at]);
			lights[at] = throughs[at].Rotate(sun);
			GatherRigFits(
				store, seams[at], throughs[at], std::span<const DrawInstance>(out.data(), drawn), fits[at]
			);
		}

		for (size_t index = 0; index < drawn; index++) {
			for (size_t at = 0; at < seams.size(); at++) {
				const PortalSeam &seam = seams[at];
				const SeamTransform &through = throughs[at];
				SeamCut cut;
				DrawInstance ghost;

				// **By index and not by reference**, because `push_back` below
				// may reallocate and because the original's row is written as
				// well as read. A reference taken here would dangle on the first
				// growth.
				if (!FarHalfOfRow(
						seam, through, out[index], lights[at], RigAnswer(fits[at], out[index]), cut, ghost
					)) {
					continue;
				}

				// Coincident copies overlap only in the same world. A foreign half
				// still needs the source cut, even when both rooms share coordinates.
				if (!seam.Crosses &&
					(ghost.Frame.Position - out[index].Frame.Position).Magnitude() < COINCIDENT_COPY) {
					continue;
				}

				// A simulated body crosses on its next fixed tick, so complementary
				// halves fill the two rooms only while it is in the doorway. Static
				// scenery has no ownership transfer. A scripted anchored part can still
				// move by writing Transform, so its previous transform keeps it on the
				// moving path as well. Cutting only a fixed prop by its centre makes a
				// portal-facing view clip the rest of it at the far mouth.
				const ecs::Entity body{out[index].Rig != 0 ? out[index].Rig : out[index].Source};
				const Transform *const current = body ? store.Get<Transform>(body) : nullptr;
				const PreviousTransform *const previous = body ? store.Get<PreviousTransform>(body) : nullptr;
				const bool scriptMoving = current != nullptr && previous != nullptr &&
										  (current->Frame.Position != previous->Frame.Position ||
										   current->Frame.QuaternionX != previous->Frame.QuaternionX ||
										   current->Frame.QuaternionY != previous->Frame.QuaternionY ||
										   current->Frame.QuaternionZ != previous->Frame.QuaternionZ ||
										   current->Frame.QuaternionW != previous->Frame.QuaternionW);
				const bool staticLocal = !seam.Crosses && out[index].Rig == 0 && body && store.Alive(body) &&
										 !store.Has<Simulated>(body) && !scriptMoving;
				const Transform *const pane = store.Get<Transform>(seam.Pane);
				constexpr float PANE_TOUCH = 1.0e-3f;
				const float centreDepth = (out[index].Frame.Position - seam.Centre).Dot(seam.Normal);
				// The seam is the pane's chosen face, so the opposite face is twice the
				// pane-centre depth away. This works for every face and rotated pane.
				const float backDepth =
					pane ? 2.0f * (pane->Frame.Position - seam.Centre).Dot(seam.Normal) : 0.0f;
				const bool tangentAtPane =
					!seam.Crosses && out[index].Rig == 0 && body && !store.Has<Simulated>(body) &&
					scriptMoving && pane != nullptr && centreDepth < 0.0f &&
					centreDepth >= backDepth - PANE_TOUCH &&
					std::abs((current->Frame.Position - previous->Frame.Position).Dot(seam.Normal)) <=
						PANE_TOUCH;
				if (tangentAtPane) {
					// A part following the mouth sideways can sit just behind its front face.
					// Keep the authored front as its source side until it moves through.
					SetCutPlanes(cut, seam, through, seam.Normal);
					ghost.SeamNormal = cut.FarNormal;
					ghost.SeamOffset = cut.FarOffset;
				}
				if (!staticLocal) {
					out[index].SeamNormal = cut.NearNormal;
					out[index].SeamOffset = cut.NearOffset;
					out[index].SeamFirst = seam.First;
					out[index].SeamSecond = seam.Second;
					out[index].SeamCentre = seam.Centre;
					out[index].SeamMask = ghost.SeamMask == 0 ? 0 : 1;
				} else {
					ghost.SeamNormal = {};
					ghost.SeamOffset = 0.0f;
				}

				if (!seam.Crosses) {
					out.push_back(ghost);
					appended++;
				}

				// One copy per body. A body in two holes at once is a body at
				// the cone point where two panes meet, and the second copy would
				// land on top of the first.
				break;
			}
		}

		return appended;
	}
}
