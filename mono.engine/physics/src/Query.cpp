#include "ContactPairs.hpp"
#include "PipelineInternals.hpp"
#include "ShapeRay.hpp"
#include "ShapeSupport.hpp"
#include "WorldResource.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Query.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::physics {

	namespace {
		// One index and the records beside it, so the two halves of a world
		// are one loop rather than two copies of it.
		struct Index {
			const spatial::HashGrid *Grid = nullptr;
			const std::vector<ColliderRecord> *Records = nullptr;
		};

		// One collider, resolved from a candidate id.
		struct Candidate {
			ecs::Entity Owner;
			ShapeInstance Shape;
			bool Present = false;
		};

		Candidate ResolveCandidate(const ecs::Store &store, const Index &index, uint64_t id) {
			const std::vector<ColliderRecord> &records = *index.Records;
			const auto at = static_cast<size_t>(id);
			if (at >= records.size()) {
				return Candidate{};
			}

			const ecs::Entity owner = records[at].Owner;
			const scene::Transform *transform = store.Get<scene::Transform>(owner);
			const scene::Collider *collider = store.Get<scene::Collider>(owner);
			if (transform == nullptr || collider == nullptr) {
				return Candidate{};
			}
			return Candidate{owner, ShapeInstance{transform->Frame, collider->Extent, collider->Shape}, true};
		}

		bool Append(std::span<ecs::Entity> found, ecs::Entity entity, spatial::QueryResult &result) {
			if (result.Written == found.size()) {
				result.Overflowed = true;
				return false;
			}
			found[result.Written++] = entity;
			return true;
		}

		// Below this a sweep direction component counts as parallel to a pair
		// of slab planes. `spatial/src/RayBox.hpp` says why the branch has to
		// exist and where the value sits; this is the same number for the same
		// reason, restated here because that header is another module's `src/`.
		constexpr float SWEEP_PARALLEL_EPSILON = 1e-20f;

		// The stretch of a sweep during which the moving bound overlaps `box`.
		struct SweepWindow {
			bool Touched = false;

			// Both along the sweep in metres, already clamped to [0, distance].
			float Entry = 0.0f;
			float Exit = 0.0f;
		};

		// The slab test once more, but keeping the exit as well as the entry.
		//
		// `spatial`'s own slab prunes the same candidates and keeps only the
		// entry; `ShapeCast` needs the whole window, because the envelope of
		// the moving bound over exactly that window is what its exact test
		// runs against.
		SweepWindow WindowAlongSweep(
			const core::Vector3 &origin, const core::Vector3 &direction, float distance, const core::AABB &box
		) {
			const float from[3] = {origin.X, origin.Y, origin.Z};
			const float along[3] = {direction.X, direction.Y, direction.Z};
			const float minimum[3] = {box.Minimum.X, box.Minimum.Y, box.Minimum.Z};
			const float maximum[3] = {box.Maximum.X, box.Maximum.Y, box.Maximum.Z};

			float entry = 0.0f;
			float exit = distance;
			for (int axis = 0; axis < 3; axis++) {
				if (std::abs(along[axis]) < SWEEP_PARALLEL_EPSILON) {
					// Answered without arithmetic: zero times an infinite
					// reciprocal is a NaN that compares false both ways.
					if (from[axis] < minimum[axis] || from[axis] > maximum[axis]) {
						return SweepWindow{};
					}
					continue;
				}

				const float inverse = 1.0f / along[axis];
				float near = (minimum[axis] - from[axis]) * inverse;
				float far = (maximum[axis] - from[axis]) * inverse;
				if (near > far) {
					const float swapped = near;
					near = far;
					far = swapped;
				}

				entry = std::max(entry, near);
				exit = std::min(exit, far);
			}

			if (entry > exit) {
				return SweepWindow{};
			}
			return SweepWindow{true, entry, exit};
		}

		// The two indexes of a world, in a fixed order.
		//
		// Dynamic first and static second, always. A query's answer is a set,
		// but the *order* it is written into the caller's span is part of what
		// a recorded run reproduces - and a caller whose span overflows keeps a
		// prefix, so the order decides which colliders that prefix holds.
		struct Indexes {
			Index Entry[2];
			bool Valid = false;
		};

		Indexes IndexesOf(const ecs::Store &store) {
			// **Through `PreparedWorld`, never `Store::Resource` directly.** A
			// query is the one entry point a caller can reach before
			// `PreparePhysicsWorld` has run, and the typed lookup would
			// register the resource under the compiler's spelling in order to
			// report that it was missing. `WorldResource.hpp` has the whole of
			// it, including what that costs a snapshot.
			const PhysicsWorld *world = PreparedWorld(store);
			if (world == nullptr) {
				return Indexes{};
			}
			return Indexes{
				{
					Index{
						&PipelineInternals::DynamicIndex(*world), &PipelineInternals::DynamicRecords(*world)
					},
					Index{&PipelineInternals::StaticIndex(*world), &PipelineInternals::StaticRecords(*world)},
				},
				true,
			};
		}

		// Every collider whose exact shape meets `volume`, written to `found`.
		//
		// The volume is a `ShapeInstance` rather than an `AABB` so that
		// `OverlapSphere` is a sphere against the real shapes rather than a box
		// against them - the difference this module exists for.
		spatial::QueryResult OverlapExact(
			const ecs::Store &store,
			const ShapeInstance &volume,
			const core::AABB &bound,
			spatial::LayerMask mask,
			std::span<ecs::Entity> found
		) {
			spatial::QueryResult result;
			const Indexes indexes = IndexesOf(store);
			if (!indexes.Valid) {
				return result;
			}

			uint64_t candidates[QUERY_CANDIDATE_LIMIT];
			for (const Index &index : indexes.Entry) {
				const spatial::QueryResult found_ =
					spatial::OverlapBox(*index.Grid, bound, mask, std::span<uint64_t>{candidates});

				// The grid found more than the stack scratch holds. Saying so
				// beats answering from a prefix: a truncated overlap read as
				// "and nothing more" is a contact that never happens.
				result.Overflowed = result.Overflowed || found_.Overflowed;

				for (size_t at = 0; at < found_.Written; at++) {
					const Candidate candidate = ResolveCandidate(store, index, candidates[at]);
					if (!candidate.Present) {
						continue;
					}
					if (!ContactBetween(volume, candidate.Shape).Touching) {
						continue;
					}
					if (!Append(found, candidate.Owner, result)) {
						return result;
					}
				}
			}
			return result;
		}
	}

	std::optional<ColliderHit> Raycast(
		const ecs::Store &store,
		const core::Ray &ray,
		float maxDistance,
		spatial::LayerMask mask,
		ecs::Entity ignore
	) {
		const Indexes indexes = IndexesOf(store);
		if (!indexes.Valid) {
			return std::nullopt;
		}

		std::optional<ColliderHit> nearest;
		core::RayHit candidates[QUERY_CANDIDATE_LIMIT];

		for (const Index &index : indexes.Entry) {
			// Nearest first, which is what makes the early exit below sound: a
			// candidate's exact hit can never be nearer than the entry to its
			// own bounding box, so once an exact hit is in hand every box
			// starting beyond it is settled without being tested.
			const spatial::QueryResult found =
				spatial::RaycastAll(*index.Grid, ray, maxDistance, mask, std::span<core::RayHit>{candidates});

			for (size_t at = 0; at < found.Written; at++) {
				if (nearest && candidates[at].Distance >= nearest->Distance) {
					break;
				}

				const Candidate candidate = ResolveCandidate(store, index, candidates[at].Id);
				if (!candidate.Present) {
					continue;
				}

				// **Skipped rather than nearest-then-compared**, which is the
				// whole reason this parameter exists: a caster standing inside
				// its own collider is always its own nearest hit, so a caller
				// testing the result has already lost the answer it wanted.
				// Costs one integer compare per candidate and nothing at all
				// when nobody passes one.
				if (ignore != ecs::Entity{} && candidate.Owner == ignore) {
					continue;
				}

				const ShapeHit hit = IntersectRayShape(candidate.Shape, ray, maxDistance);
				if (!hit.Touched) {
					continue;
				}
				if (nearest && nearest->Distance <= hit.Distance) {
					continue;
				}
				nearest = ColliderHit{candidate.Owner, hit.Distance, ray.PointAt(hit.Distance), hit.Normal};
			}
		}

		return nearest;
	}

	std::optional<ColliderHit> RaycastThroughPortals(
		ecs::Store &store,
		const core::Ray &ray,
		float maxDistance,
		spatial::LayerMask mask,
		ecs::Entity ignore
	) {
		std::optional<ColliderHit> blocking = Raycast(store, ray, maxDistance, mask, ignore);

		if (maxDistance <= 0.0f) {
			return blocking;
		}

		// Where the whole ray would end, which is the segment `PortalCrossing`
		// tests - the same one a body's tick is tested as.
		const core::Vector3 finish = ray.PointAt(maxDistance);

		scene::PortalHop hop;
		if (!scene::PortalCrossing(store, ray.Origin, finish, hop)) {
			return blocking;
		}

		// **The pane is not a wall to a ray that goes through it**, and it is
		// the nearest thing in front of every hole. `OpenPortals` leaves the
		// collider in place as a trigger so contacts are still reported, so
		// without this line every portal ray stops on the glass and the
		// continuation below never runs once.
		//
		// **Only once a crossing is known**, so a ray that meets the pane
		// outside its rectangle - the frame, the edge, the wall the hole is cut
		// in - still stops on it, which is what a hole with a border means.
		if (blocking && blocking->Owner == hop.Pane) {
			blocking.reset();
		}

		const float reached = hop.Share * maxDistance;

		// **Anything else solid before the glass settles it.** A wall between the
		// caster and the pane is a wall, and continuing past it would let a ray
		// see through geometry - which is the one thing a hole must not make
		// true of everything else in the room.
		if (blocking && blocking->Distance <= reached) {
			return blocking;
		}

		const float remaining = maxDistance - reached;
		if (remaining <= 0.0f) {
			return blocking;
		}

		// **Both ends mapped, and forgetting the direction is the bug that looks
		// like the hole being crooked.** A pane pair that turns a corner sends
		// the remainder off along the axis it came in on, so the ray leaves the
		// far side aimed the way it entered the near one rather than the way the
		// far pane faces - see `CrossPortals`, which is the same two lines about
		// a body's placement and its velocity.
		//
		// **`Rotate` and not `Carry` for the direction**, because `core::Ray`
		// keeps a unit one and every distance it reports is measured along it. A
		// scaled direction would make the far side's hits come back at the wrong
		// range, which is a ground query that says a floor is twice as far away
		// as it is.
		const core::Vector3 at = ray.PointAt(reached);
		const core::Ray beyond(hop.Through.Point(at), hop.Through.Rotate(ray.Direction));

		// **And the reach it has left is a length, so it scales.** A ray with a
		// metre to run that enters the large end of a hole has that metre
		// stretched with everything else about it - anything else would give a
		// character walking into the big room a ground query that stops short of
		// a floor it is standing on.
		const float beyondDistance = hop.Through.Length(remaining);

		// **The far pane is ignored and the caster is not.** `ignore` is the
		// caster, and the caster is on this side of the glass by construction -
		// carrying it across would name whatever entity happens to share that
		// index over there, which is nothing in particular. What does have to go
		// is the pane the ray comes *out* of: the map takes the near pane's plane
		// onto the far one's, so the continuation starts at zero distance from it
		// and every portal ray would report the destination's own glass as the
		// first thing beyond the hole.
		std::optional<ColliderHit> far = Raycast(store, beyond, beyondDistance, mask, hop.Far);
		if (!far) {
			return blocking;
		}

		// Measured from the original origin, so a caller comparing against its
		// own reach never has to know a hole was involved - which means the far
		// side's distance comes back through the scale it went out by.
		far->Distance = reached + far->Distance / hop.Through.Scale;
		return far;
	}

	spatial::QueryResult OverlapBox(
		const ecs::Store &store, const core::AABB &box, spatial::LayerMask mask, std::span<ecs::Entity> found
	) {
		const ShapeInstance volume{core::CFrame{box.Centre()}, box.Size() * 0.5f, scene::ShapeKind::Box};
		return OverlapExact(store, volume, box, mask, found);
	}

	spatial::QueryResult OverlapSphere(
		const ecs::Store &store,
		const core::Vector3 &centre,
		float radius,
		spatial::LayerMask mask,
		std::span<ecs::Entity> found
	) {
		// `!(radius >= 0)` also refuses a NaN, which would otherwise build a
		// bound that matches nothing while the exact test matched everything.
		if (!(radius >= 0.0f)) {
			return spatial::QueryResult{};
		}

		const ShapeInstance volume{
			core::CFrame{centre}, core::Vector3{radius, radius, radius}, scene::ShapeKind::Sphere
		};
		return OverlapExact(
			store, volume, core::AABB::FromCentre(centre, core::Vector3{radius, radius, radius}), mask, found
		);
	}

	spatial::QueryResult ShapeCast(
		const ecs::Store &store,
		const scene::Collider &collider,
		const core::CFrame &from,
		const core::Vector3 &motion,
		spatial::LayerMask mask,
		std::span<ecs::Entity> found
	) {
		// Zero motion is an overlap, and answering it as one keeps a caller
		// that happened to pass a still frame from getting a different kind of
		// answer than the one it asked for.
		const core::AABB start = ShapeWorldBounds(collider, from);
		if (motion == core::Vector3::Zero) {
			const ShapeInstance volume{from, collider.Extent, collider.Shape};
			return OverlapExact(store, volume, start, mask, found);
		}

		spatial::QueryResult result;
		const Indexes indexes = IndexesOf(store);
		if (!indexes.Valid) {
			return result;
		}

		// `!(x > 0)` refuses a NaN motion here rather than letting it become a
		// NaN ray the grid walk compares false against everything.
		const float distance = motion.Magnitude();
		if (!(distance > 0.0f)) {
			return result;
		}
		const core::Vector3 direction = motion / distance;
		const core::Vector3 origin = start.Centre();
		const core::Vector3 halfExtent = start.Size() * 0.5f;

		// Candidates come from `spatial::ShapeCast`'s walk along the sweep
		// itself, not from an overlap of the start-to-end union box - over a
		// long sweep that box holds a whole quadrant the shape never enters.
		//
		// **De-duplication is the walk's, and it is enough.** The swept walk
		// reports a proxy once per grid however many cells it spans, and a
		// collider lives in exactly one of the two indexes, so no candidate is
		// seen twice here and no set is needed. The order is the walk's order,
		// dynamic index first - stable for a given world and input, which is
		// what makes the prefix a full span keeps reproducible.
		uint64_t candidates[QUERY_CANDIDATE_LIMIT];
		for (const Index &index : indexes.Entry) {
			const spatial::QueryResult found_ =
				spatial::ShapeCast(*index.Grid, start, motion, mask, std::span<uint64_t>{candidates});
			result.Overflowed = result.Overflowed || found_.Overflowed;

			for (size_t at = 0; at < found_.Written; at++) {
				const Candidate candidate = ResolveCandidate(store, index, candidates[at]);
				if (!candidate.Present) {
					continue;
				}

				// The window of the sweep during which the two *bounds* can
				// touch at all: the moving bound against a still bound is the
				// bound's centre line against the still bound grown by the
				// moving half-extent.
				scene::Collider candidateCollider;
				candidateCollider.Shape = candidate.Shape.Shape;
				candidateCollider.Extent = candidate.Shape.Extent;
				const core::AABB bound = ShapeWorldBounds(candidateCollider, candidate.Shape.Frame);
				const core::AABB expanded =
					core::AABB::FromCentre(bound.Centre(), bound.Size() * 0.5f + halfExtent);
				const SweepWindow window = WindowAlongSweep(origin, direction, distance, expanded);
				if (!window.Touched) {
					continue;
				}

				// The exact test is against the moving bound's envelope over
				// just that window, not the whole path: any real contact
				// happens inside the window, and the moving shape never leaves
				// its own bound, so nothing is missed - while a collider the
				// bounds merely pass beside is thrown out here exactly.
				const core::Vector3 nearOffset = direction * window.Entry;
				const core::Vector3 farOffset = direction * window.Exit;
				const core::AABB envelope =
					core::AABB{start.Minimum + nearOffset, start.Maximum + nearOffset}.Union(
						core::AABB{start.Minimum + farOffset, start.Maximum + farOffset}
					);
				const ShapeInstance sweptHere{
					core::CFrame{envelope.Centre()}, envelope.Size() * 0.5f, scene::ShapeKind::Box
				};
				if (!ContactBetween(sweptHere, candidate.Shape).Touching) {
					continue;
				}
				if (!Append(found, candidate.Owner, result)) {
					return result;
				}
			}
		}
		return result;
	}
}
