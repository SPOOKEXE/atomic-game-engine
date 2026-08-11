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
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/spatial/Query.hpp>

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

		// The two indexes of a world, in a fixed order.
		//
		// Dynamic first and static second, always. A query's answer is a set,
		// but the *order* it is written into the caller's span is part of what
		// a recorded run reproduces — and a caller whose span overflows keeps a
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
		// against them — the difference this module exists for.
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

		// The path's envelope: the shape's own bound at both ends, unioned. The
		// exact test then runs the candidates against *that box* rather than
		// against the moving shape — which is why the header calls this
		// conservative and says what it is not.
		const core::AABB swept = start.Union(core::AABB{start.Minimum + motion, start.Maximum + motion});
		const ShapeInstance volume{core::CFrame{swept.Centre()}, swept.Size() * 0.5f, scene::ShapeKind::Box};
		return OverlapExact(store, volume, swept, mask, found);
	}
}
