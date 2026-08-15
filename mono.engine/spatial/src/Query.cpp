#include "GridInternals.hpp"
#include "RayBox.hpp"

#include <engine/spatial/Query.hpp>

#include <cstdint>

namespace engine::spatial {

	namespace {
		// The box that encloses a ray segment, which is the volume the grid
		// walk is given.
		//
		// The walk visits cells in a fixed order rather than along the line, so
		// a raycast asks the same question every other query asks - "what is in
		// this box" - and then tests the candidates exactly. A digital
		// differential walk along the ray would visit fewer cells, and would
		// also visit them in an order that is not the ascending one the
		// de-duplication rule is built on. See `AGENTS.md`: it is an
		// optimisation with a rule change attached, not a free improvement.
		core::AABB SegmentBounds(const core::Ray &ray, float maxDistance) {
			const core::Vector3 end = ray.PointAt(maxDistance);
			return core::AABB{ray.Origin, ray.Origin}.Union(core::AABB{end, end});
		}

		// Whether a ray can find anything at all before a grid is consulted.
		bool CanTravel(const core::Ray &ray, float maxDistance) {
			// `!(x > 0)` rather than `x <= 0`, so a NaN distance is refused
			// rather than being carried into the slab test where it compares
			// false against everything.
			return !(ray.Direction == core::Vector3::Zero) && maxDistance > 0.0f;
		}

		// Appends one id, or reports that there was nowhere to put it.
		//
		// @return False when the span is full, which stops the walk - there is
		//         nothing further to learn once the answer cannot be recorded.
		bool Append(std::span<uint64_t> found, uint64_t id, QueryResult &result) {
			if (result.Written == found.size()) {
				result.Overflowed = true;
				return false;
			}
			found[result.Written++] = id;
			return true;
		}

		// Inserts a hit into a span held in increasing distance, dropping the
		// furthest when there is no room.
		//
		// Linear in the span's length, which is the right shape here: the spans
		// a caller passes are small, and the alternative - collect everything
		// then sort - needs storage the caller did not give us.
		void InsertNearest(std::span<core::RayHit> hits, const core::RayHit &hit, QueryResult &result) {
			if (result.Written == hits.size()) {
				if (hits.empty() || !(hit.Distance < hits.back().Distance)) {
					result.Overflowed = true;
					return;
				}
				// Room is made by discarding the furthest, so a full span keeps
				// the nearest rather than the first found.
				result.Overflowed = true;
				result.Written--;
			}

			size_t slot = result.Written;
			while (slot > 0 && hits[slot - 1].Distance > hit.Distance) {
				hits[slot] = hits[slot - 1];
				slot--;
			}
			hits[slot] = hit;
			result.Written++;
		}
	}

	std::optional<core::RayHit>
	Raycast(const HashGrid &grid, const core::Ray &ray, float maxDistance, LayerMask mask) {
		if (!CanTravel(ray, maxDistance)) {
			return std::nullopt;
		}

		const RayReciprocal reciprocal{ray.Direction};
		std::optional<core::RayHit> nearest;

		GridInternals::ForEachCandidate(grid, SegmentBounds(ray, maxDistance), mask, [&](const Proxy &proxy) {
			const BoxHit box = IntersectRayBox(ray, reciprocal, proxy.Bounds, maxDistance);
			if (!box.Touched) {
				return true;
			}
			if (nearest && nearest->Distance <= box.Distance) {
				return true;
			}
			nearest = core::RayHit{proxy.Id, box.Distance, ray.PointAt(box.Distance), box.Normal};
			return true;
		});

		return nearest;
	}

	QueryResult RaycastAll(
		const HashGrid &grid,
		const core::Ray &ray,
		float maxDistance,
		LayerMask mask,
		std::span<core::RayHit> hits
	) {
		QueryResult result;
		if (!CanTravel(ray, maxDistance)) {
			return result;
		}

		const RayReciprocal reciprocal{ray.Direction};

		// No early exit even once the span is full: a later candidate may be
		// nearer than one already written, and dropping it would make the
		// answer depend on which cell happened to be walked first.
		GridInternals::ForEachCandidate(grid, SegmentBounds(ray, maxDistance), mask, [&](const Proxy &proxy) {
			const BoxHit box = IntersectRayBox(ray, reciprocal, proxy.Bounds, maxDistance);
			if (box.Touched) {
				InsertNearest(
					hits, core::RayHit{proxy.Id, box.Distance, ray.PointAt(box.Distance), box.Normal}, result
				);
			}
			return true;
		});

		return result;
	}

	QueryResult
	OverlapBox(const HashGrid &grid, const core::AABB &box, LayerMask mask, std::span<uint64_t> found) {
		QueryResult result;
		GridInternals::ForEachCandidate(grid, box, mask, [&](const Proxy &proxy) {
			return Append(found, proxy.Id, result);
		});
		return result;
	}

	QueryResult OverlapSphere(
		const HashGrid &grid,
		const core::Vector3 &centre,
		float radius,
		LayerMask mask,
		std::span<uint64_t> found
	) {
		QueryResult result;

		// `!(radius >= 0)` also refuses a NaN, which would otherwise build a
		// volume that matches nothing while the distance test matched
		// everything.
		if (!(radius >= 0.0f)) {
			return result;
		}

		const core::AABB volume = core::AABB::FromCentre(centre, core::Vector3{radius, radius, radius});
		const float radiusSquared = radius * radius;

		GridInternals::ForEachCandidate(grid, volume, mask, [&](const Proxy &proxy) {
			// Squared throughout, so the test costs no square root. The nearest
			// point of the box, not its centre - a large box beside a small
			// sphere is in contact long before their centres are close.
			const core::Vector3 offset = proxy.Bounds.ClosestPoint(centre) - centre;
			if (offset.MagnitudeSquared() > radiusSquared) {
				return true;
			}
			return Append(found, proxy.Id, result);
		});

		return result;
	}

	QueryResult ShapeCast(
		const HashGrid &grid,
		const core::AABB &box,
		const core::Vector3 &motion,
		LayerMask mask,
		std::span<uint64_t> found
	) {
		if (motion == core::Vector3::Zero) {
			return OverlapBox(grid, box, mask, found);
		}

		QueryResult result;

		const core::Vector3 halfExtent = box.Size() * 0.5f;
		const float distance = motion.Magnitude();
		const core::Ray ray{box.Centre(), motion / distance};
		const RayReciprocal reciprocal{ray.Direction};

		// The cells to look in are everything the box passes through, which is
		// its start and its end unioned. The exact test then rejects the
		// corners the union covers and the sweep does not.
		const core::AABB swept = box.Union(core::AABB{box.Minimum + motion, box.Maximum + motion});

		GridInternals::ForEachCandidate(grid, swept, mask, [&](const Proxy &proxy) {
			// A moving box against a still box is a moving *point* against the
			// still box grown by the moving one's half-extent - so the swept
			// test is the slab test already written, with no second algorithm
			// to keep correct.
			const core::AABB expanded =
				core::AABB::FromCentre(proxy.Bounds.Centre(), proxy.Bounds.Size() * 0.5f + halfExtent);
			if (!IntersectRayBox(ray, reciprocal, expanded, distance).Touched) {
				return true;
			}
			return Append(found, proxy.Id, result);
		});

		return result;
	}
}
