// Portal crossing: carrying bodies and cameras through seams each tick and
// resizing what goes through a hole between panes of different sizes.

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
		using surface_camera_detail::GatherSeams;
		using surface_camera_detail::NearestCrossing;
		using surface_camera_detail::Seams;

		// How far past the destination's plane a crosser is put down, in studs.
		//
		// **A body that lands on a plane is a body that can cross it again.** The
		// map takes the source pane onto the destination exactly, so a crosser
		// arrives at whatever depth past the far plane its own step happened to
		// end at - which can be a float's worth of nothing. One tick of jitter
		// then changes its side back and it is returned through the hole it came
		// out of, once per tick, which reads as the portal spitting people back
		// and forth rather than as a rounding error.
		//
		// **This is the hysteresis, and it is the only one.** CodeParade's demo
		// bumps twice - the landing *and* the plane the test is made against -
		// and the second half is not taken here on purpose. Offsetting the test
		// plane means a body that begins a tick inside the offset never sees a
		// sign change at all, and at 500 Hz with a 2 mm offset nothing can start
		// there; at 60 Hz, where a character covers a quarter of a stud a tick, a
		// body can begin a tick anywhere and the offset becomes a band you walk
		// through without crossing. A landing that is always clear gives the same
		// guarantee - nothing can come to rest within the bump of a plane because
		// it crossed one - with no band to fall into.
		//
		// Twenty times the solver's own penetration slop, and a small fraction of
		// any tick's travel or any body's reach, so it can neither be lost to
		// float error nor take a crosser outside the seam its clone is drawn
		// through - see `SeamStraddled`, whose reach is a whole body's radius.
		constexpr float LANDING_CLEARANCE = 0.01f;

		// Carries both ends of one tick through a seam and leaves them on the
		// destination side. Bodies and scriptable cameras use the same operation;
		// keeping it here prevents the eye and the thing it watches disagreeing
		// about the landing plane.
		void CarryCrossingFrames(
			const PortalSeam &hole, const SeamTransform &through, CFrame &placement, CFrame &previous
		) {
			const float side = SeamOffset(hole, previous.Position) > 0.0f ? -1.0f : 1.0f;
			const float depth = std::abs(SeamOffset(hole, placement.Position));
			const Vector3 clear = hole.Normal * (side * std::max(LANDING_CLEARANCE - depth, 0.0f));

			placement = through.Place(CFrame{placement.Position + clear, placement.Rotation()});

			const float leaving = SeamOffset(hole, previous.Position) * side;
			if (leaving < LANDING_CLEARANCE) {
				previous = CFrame{
					previous.Position + hole.Normal * (side * (LANDING_CLEARANCE - leaving)),
					previous.Rotation()
				};
			}
			previous = through.Place(previous);
		}
	}

	// Grows or shrinks everything about one body that is a length.
	//
	// **What a hole between panes of different sizes does to what goes through
	// it, and it is a list rather than a multiply because a body is not one
	// row.** A crate is its `Bounds` and its `Collider`. A character is those on
	// a root nobody can see, a `Humanoid` on a *third* entity holding every
	// figure that decides how it moves, and five limbs that are their own rows
	// with their own boxes and their own rest offsets. Missing any one of them
	// is visible immediately: a character whose limbs did not scale is a body
	// standing inside a rig the size it used to be.
	//
	// **Multiplied rather than assigned, so the pair is its own inverse.** The
	// scale of a seam is the ratio of two measurements that a crossing does not
	// change, so going back through the other way multiplies by the reciprocal
	// and lands on the number it started with - which is what makes a corridor
	// of mismatched holes a place you can walk around in rather than a ratchet.
	//
	// **Not the mass, and not because it is forgotten.** `PhysicsProperties`
	// derives mass from the volume and the density, so a body whose box has
	// changed already weighs what a body that size weighs.
	//
	// @param store The world.
	// @param body  The row the solver moves, which for a character is its root.
	// @param scale What to multiply by.
	static void ResizeCrosser(Store &store, Entity body, float scale) {
		if (const Bounds *bounds = store.Get<Bounds>(body)) {
			store.Set(body, Bounds{bounds->HalfExtent * scale});
		}

		// **Its own extent and not the bounds'.** A collider may be a different
		// shape and a different size from the box a part draws as, which is
		// exactly what `Collider::Extent` is for - scaling one and not the other
		// is a body that looks right and collides at its old size.
		if (const Collider *collider = store.Get<Collider>(body)) {
			Collider resized = *collider;
			resized.Extent = resized.Extent * scale;
			store.Set(body, resized);
		}

		// The humanoid steering it, which is on the model rather than on the
		// part - `Character` exists to spare everything this a walk by name.
		// A scripted character carries both on one row and is found by the
		// fallback below.
		Entity steering = NULL_ENTITY;
		store.Each<const Character>([&](Entity, const Character &character) {
			if (character.Root == body) {
				steering = character.Humanoid;
			}
		});
		if (steering == NULL_ENTITY && store.Has<Humanoid>(body)) {
			steering = body;
		}

		if (const Humanoid *humanoid = steering == NULL_ENTITY ? nullptr : store.Get<Humanoid>(steering)) {
			Humanoid resized = *humanoid;

			// **Every figure here is a length or a length per second**, and the
			// two that are easy to leave out are the ones that show. A ground
			// tolerance that did not shrink is a character hovering a tenth of
			// its own height off the floor; a walk speed that did not is a body
			// a tenth the size crossing the room at the same rate, which reads
			// as the portal having made it fast rather than small.
			resized.Height *= scale;
			resized.WalkSpeed *= scale;
			resized.JumpSpeed *= scale;
			resized.GroundTolerance *= scale;
			store.Set(steering, resized);
		}

		// **Gathered before writing**, because adding nothing and changing a
		// component are different costs and a walk that wrote inside itself
		// would queue a heap copy per limb. The same two phases every other pass
		// in this file uses, for the same reason.
		std::vector<Entity> limbs;
		store.Each<const CharacterLimb>([&](Entity limb, const CharacterLimb &carried) {
			if (carried.Root == body) {
				limbs.push_back(limb);
			}
		});

		// Attachment offsets are authored in their parent's units. Carried
		// accessories rebuild their limb offset from these points next frame.
		std::vector<Entity> attachments;
		store.Each<const Attachment>([&](Entity point, const Attachment &) {
			const Entity parent = store.ParentOf(point);
			if (parent == body || std::find(limbs.begin(), limbs.end(), parent) != limbs.end())
				attachments.push_back(point);
		});
		for (const Entity point : attachments) {
			auto *attachment = store.GetMutable<Attachment>(point);
			attachment->Frame.Position = attachment->Frame.Position * scale;
		}

		for (const Entity limb : limbs) {
			// **The rest offset as well as the box.** `PoseCharacterLimbs` puts
			// a limb at `root · Offset` every tick, so a rig whose offsets kept
			// their old lengths is a character shrunk in the middle of a skeleton
			// that did not - arms at the old distance, boxes at the new size.
			if (const CharacterLimb *carried = store.Get<CharacterLimb>(limb)) {
				CharacterLimb moved = *carried;
				moved.Offset = CFrame{moved.Offset.Position * scale, moved.Offset.Rotation()};
				store.Set(limb, moved);
			}

			if (const Bounds *bounds = store.Get<Bounds>(limb)) {
				store.Set(limb, Bounds{bounds->HalfExtent * scale});
			}

			if (const Collider *collider = store.Get<Collider>(limb)) {
				Collider resized = *collider;
				resized.Extent = resized.Extent * scale;
				store.Set(limb, resized);
			}
		}
	}

	namespace {
		// Bodies per worker for the crossing search: one segment against every
		// seam, far heavier than the three adds `EachParallel`'s crossover uses.
		constexpr size_t CROSSING_GRAIN = 512;
	}

	size_t CrossPortals(ecs::Store &store) {
		// **The portals are gathered before anything is moved.** A body pushed
		// through one lands somewhere another might also claim, and a single
		// pass that moved as it walked would let one crossing feed the next
		// within a tick - a character could be bounced through three holes on
		// one step, which is neither what the author drew nor reproducible.
		std::vector<PortalSeam> &holes = Seams();
		GatherSeams(store, holes);

		if (holes.empty()) {
			return 0;
		}

		size_t crossed = 0;

		// What went through a hole that changes size, and by how much. Empty in
		// every world whose panes match, which is every world with a mirror in
		// it and most with a portal.
		struct Resize {
			ecs::Entity Body;
			float Scale;
		};
		std::vector<Resize> resized;

		// Bodies whose frame and velocity the walk rewrote through its references,
		// which set no change bit; reported after the walk.
		std::vector<ecs::Entity> carried;

		// Whose walk intent has to turn with them, and by what.
		//
		// **`Humanoid::MoveDirection` is world-space and nothing maps it**,
		// which is the whole of "normal objects are fine but the character".
		// A crate keeps the velocity this pass carries through the seam; a
		// character has its horizontal velocity reassigned from
		// `MoveDirection` on the very next tick, so the carry survives zero
		// ticks and the body walks out of the far pane aimed the way it was
		// aimed in the room it left. `PortalTransit::Frame` is written below and
		// only ever reaches a *viewer's* camera - on a server-simulated
		// character that is a different machine.
		struct TurnedIntent {
			ecs::Entity Steering;
			SeamTransform Through;
		};
		std::vector<TurnedIntent> turnedIntents;

		// Which humanoid steers which body, gathered once.
		//
		// The same question is asked by `ResizeCrosser`, which may walk for it
		// because a crossing that resizes is rare. Asked inside the walk below
		// it would be an `Each` opened inside an `Each` on every crossing.
		std::vector<std::pair<ecs::Entity, ecs::Entity>> steering;
		store.Each<const Character>([&](ecs::Entity, const Character &character) {
			steering.push_back({character.Root, character.Humanoid});
		});

		// **Anything with a velocity, not only a character.** A portal that
		// swallowed people and refused a thrown crate would be a portal with a
		// footnote, and the arithmetic does not care which it is. Anchored
		// scenery carries no `Motion` and is therefore never a candidate, which
		// is the archetype doing the filtering rather than a branch.
		//
		// **Search, then apply.** Which hole each body met is a read of its own
		// two positions against the gathered seams, so every row asks on a
		// worker and keeps its answer in its own slot. The walk below then runs
		// in the same row order and only does work for the rows that crossed.
		struct Crossing {
			PortalHop Hop;
			size_t Seam = 0;
			bool Met = false;
		};
		static thread_local std::vector<Crossing> ownerCrossings;
		std::vector<Crossing> &crossings = ownerCrossings;
		crossings.resize(store.Query<const Transform, const Motion, const PreviousTransform>().Count());
		const std::span<const PortalSeam> seams = holes;
		store.Query<const Transform, const Motion, const PreviousTransform>().EachBatchEntitiesParallel(
			[&crossings, seams](
				size_t first,
				size_t rows,
				const ecs::Entity *,
				const Transform *placements,
				const Motion *,
				const PreviousTransform *befores
			) {
				for (size_t row = 0; row < rows; row++) {
					Crossing &crossing = crossings[first + row];
					crossing.Met = NearestPortalCrossing(
						seams,
						befores[row].Frame.Position,
						placements[row].Frame.Position,
						true,
						crossing.Hop,
						crossing.Seam
					);
				}
			},
			CROSSING_GRAIN
		);
		size_t row = 0;
		store.Each<Transform, Motion, PreviousTransform>(
			[&](ecs::Entity entity, Transform &placement, Motion &motion, PreviousTransform &before) {
				const Crossing &crossing = crossings[row++];
				if (!crossing.Met) return;

				// **The nearest hole this step met, by the same rule and the
				// same code as the camera's.** This used to take the first seam
				// the gather happened to list, which is archetype order and
				// therefore not a rule - and `PlaceCamera` already took the
				// nearest for its arm. On any frame the two chose differently
				// the body went through one hole and the eye through another,
				// and what that looks like is the view flipping on some
				// crossings and not others with nothing to tell them apart.
				//
				// **One hole per body per tick**, which the single answer now
				// says rather than a `break` at the bottom of a loop: a body
				// bounced through three holes on one step is neither what the
				// author drew nor reproducible.
				const PortalHop &hop = crossing.Hop;
				const size_t seamIndex = crossing.Seam;
				if (holes[seamIndex].Crosses) {
					return;
				}

				const PortalSeam &hole = holes[seamIndex];
				const SeamTransform &through = hop.Through;

				{
					// **Put down clear of the plane it is about to be on the far
					// side of.** Pushed before the map rather than after, so the
					// only normal involved is the source pane's - which this pass
					// already has - and the rigid map carries the clearance to the
					// destination as the same distance out of the far pane. See
					// `LANDING_CLEARANCE`.
					//
					// **A floor on the depth rather than a push, and the
					// difference is a drift you can see.** Adding the clearance
					// unconditionally moves every crossing by it, so a body that
					// walks through a hole and back comes out beside where it
					// started, and again on the next round trip. What is wanted
					// is that nothing *rests* within the clearance of a plane,
					// which is a minimum and not an offset: a body that ended
					// its step a metre past the pane already has it, and gets
					// nothing added.
					//
					// Away from the side it came in on, which is the direction it
					// was already travelling.

					// **The placement and the velocity, by the same transform.**
					// Forgetting the second is the bug that looks like physics:
					// the body arrives aimed the way it was aimed in the frame it
					// left, so it walks out of the destination sideways.
					//
					// **`Carry` for the velocity, because a speed is a length.**
					// A body that comes out of the small end of a hole at the
					// speed it went into the large end crosses the far room in a
					// fraction of the time, which reads as the portal firing you
					// out rather than as a change of scale.
					CarryCrossingFrames(hole, through, placement.Frame, before.Frame);
					motion.Linear = through.Carry(motion.Linear);

					// **And the spin, by the same rotation but not by the same
					// scale.** `Motion` is a `Linear` and an `Angular`, the
					// solver writes both and `physics::Advanced` integrates
					// both, so a pair that turns the room turns the axis a body
					// is tumbling about exactly as it turns the direction the
					// body is travelling in. Mapping one and not the other is
					// the bug that reads as physics: a crate goes through a hole
					// in a floor still spinning about the wall's up.
					//
					// **`Rotate` and not `Carry`, because radians per second are
					// not a length.** A hole that halves a body halves the
					// radius it spins at and halves the speed of every point on
					// it, and the rate those two divide to is unchanged - which
					// is why this is the one thing crossing a scaled seam that
					// the scale must not touch. `Carry` would spin a body down
					// to nothing over a few crossings of a shrinking pair.
					//
					// An angular velocity is a pseudovector and would need the
					// determinant as well if a seam could reflect. None can:
					// `SeamTransform::Frame` is a `CFrame`, whose rotation is
					// proper by construction, and `Scale` is a square root and
					// therefore positive. A mirror is the only thing here with a
					// reflection in it, and nothing crosses a mirror.
					motion.Angular = through.Rotate(motion.Angular);
					carried.push_back(entity);

					// **And where it started the tick, or the frames between now
					// and the next tick draw it halfway between two rooms.**
					// `CapturePreviousTransforms` records where a body was when
					// the tick began and the renderer blends the two by the
					// tick's alpha - so a body teleported after that capture is
					// interpolated *across the teleport*, and at three frames to
					// a tick it is drawn once or twice somewhere in the hundred
					// units between the two panes. What that looks like is the
					// character snapping, with a streak where it went.
					//
					// **Mapped rather than collapsed onto the new placement.**
					// CodeParade's demo assigns `prev_pos = pos`, which removes
					// the streak by removing the motion - the body stands still
					// for the rest of the tick and then jumps. Putting the old
					// position through the same map keeps the whole tick, just
					// expressed in the room the body is now in, so it walks out
					// of the far pane at the speed it walked into the near one.
					// **Clamped to the side it is leaving from, first.** The
					// map is rigid, so a previous frame on the entry side of
					// this pane arrives on the *far* side of the destination
					// pane - behind it, by construction. The renderer wants
					// that; the next tick does not. `NearestCrossing` re-runs
					// from scratch every tick against the segment from here to
					// there, so a `was` behind the far pane makes the next
					// segment straddle it and the body crosses straight back,
					// and then again, forever. That is the reported "sits in
					// the middle of a portal and teleports between both sides",
					// and it survives even with nothing steering the body.
					//
					// Only the component along the normal moves. The tick's
					// sideways motion is kept, so what the renderer draws is a
					// body emerging from the pane rather than one standing
					// still - which is the whole reason this frame is mapped
					// instead of collapsed onto the new placement.
					// **And the body itself, when the two ends are not the same
					// size.** Gathered rather than applied here for the reason
					// every pass in this file gives about its own two phases: this
					// touches other entities - a character's limbs are their own
					// rows - and an archetype the walk is not standing on is still
					// a deferred command with a heap copy behind it.
					if (through.Scale != 1.0f) {
						resized.push_back(Resize{entity, through.Scale});
					}

					// **And the walk intent, for the reason the velocity above
					// is mapped.** A rig keeps its humanoid on the model beside
					// the parts; a scripted character keeps it on the part
					// itself, which is the fallback. Deferred with the rest:
					// this writes a component on an entity the walk is not
					// standing on.
					ecs::Entity steered = ecs::NULL_ENTITY;
					for (const auto &[root, humanoid] : steering) {
						if (root == entity) {
							steered = humanoid;
							break;
						}
					}
					if (steered == ecs::NULL_ENTITY && store.Has<Humanoid>(entity)) {
						steered = entity;
					}
					if (steered != ecs::NULL_ENTITY) {
						turnedIntents.push_back(TurnedIntent{steered, through});
					}

					// **And the turn is written down, because the eye that has
					// to follow it is on another machine.** This is the same
					// bug as the velocity one wearing a different coat: a
					// player walks west through a hole whose pair turns a
					// corner, the body comes out walking north, and the view
					// keeps pointing west - so the character reads as spinning
					// on the spot and W stops meaning forward, because
					// `ReadMoveIntent` is relative to the camera's yaw.
					//
					// It cannot be fixed here by reaching for the camera.
					// `CameraController` is a resource on whichever host is
					// *looking*, and the host running this is whichever one is
					// *simulating* - in a studio Play or against a real server
					// those are two different worlds, and the authority's
					// controller is not the one the player sees through.
					// `scene::PortalTransit` is the fact; `FollowPortalTransit`
					// is the client end of it.
					//
					// Retain the complete map so a viewer can consume several crossings
					// between presentations without reducing their composition to yaw.
					PortalTransit went;
					if (const PortalTransit *previous = store.Get<PortalTransit>(entity)) went = *previous;
					went.Frame = through.Place(went.Frame).Orthonormalize();
					went.Scale *= through.Scale;
					++went.Serial;

					store.Set(entity, went);

					// **Taken here, so the snap below is a no-op on the machine
					// that did the crossing.** This pass has just mapped
					// `before.Frame` through the seam - which keeps the whole
					// tick's motion, expressed in the room the body is now in -
					// and a snap would throw that away and stand the body still
					// for the rest of the tick. A replica never reaches this
					// line, so its own counter stays behind and
					// `SnapPortalTransit` fires there exactly once.
					store.Set(entity, PortalTransitSeen{went.Serial});

					crossed++;
				}
			}
		);

		// **After the walk, where the defer depth is back to zero.** A resize
		// touches a character's limbs, which are rows the walk above never
		// visits, and `ResizeCrosser` walks the world twice to find them - an
		// `Each` opened inside another one is a great deal of machinery to run
		// per crossing when a crossing is already over.
		for (const ecs::Entity body : carried) {
			store.MarkChanged<Transform>(body);
			store.MarkChanged<Motion>(body);
		}
		for (const Resize &change : resized) {
			ResizeCrosser(store, change.Body, change.Scale);
		}

		// **`Rotate`, not `Carry`.** A walk direction is a unit vector and a
		// scaled hole would otherwise hand the controller an intent longer or
		// shorter than one, which `StepCharacters` multiplies by `WalkSpeed` -
		// so a shrinking pair would make a character sprint. The speed is a
		// length and is scaled by `ResizeCrosser` above, which is where that
		// belongs.
		for (const TurnedIntent &turn : turnedIntents) {
			if (Humanoid *humanoid = store.GetMutable<Humanoid>(turn.Steering)) {
				humanoid->MoveDirection = turn.Through.Rotate(humanoid->MoveDirection);
			}
		}

		// A free or scriptable camera is its own moving viewpoint. It has no
		// `Motion`, so the body walk above cannot see it, while a subject camera
		// is placed later by `PlaceCamera` and already maps its arm through a seam.
		// Requiring the controller distinguishes a presented world from a store
		// used only to aim portal and mirror cameras.
		const ActiveCamera *active = store.Resource<ActiveCamera>();
		const CameraController *controller = store.Resource<CameraController>();
		const bool independentlyPlaced =
			controller != nullptr &&
			((active != nullptr && CameraSubjectRoot(store, active->Entity) == NULL_ENTITY) ||
			 controller->Mode == CameraMode::Scriptable);
		if (active != nullptr && independentlyPlaced && active->Entity != NULL_ENTITY &&
			store.Alive(active->Entity) && !store.Has<Motion>(active->Entity)) {
			Transform *placement = store.GetMutable<Transform>(active->Entity);
			PreviousTransform *before = store.GetMutable<PreviousTransform>(active->Entity);
			if (placement != nullptr && before != nullptr) {
				PortalHop hop;
				const PortalSeam *met =
					NearestCrossing(holes, before->Frame.Position, placement->Frame.Position, hop);
				if (met != nullptr) {
					CarryCrossingFrames(*met, hop.Through, placement->Frame, before->Frame);
					crossed++;
				}
			}
		}

		return crossed;
	}
}
