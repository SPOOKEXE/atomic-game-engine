#include "GridInternals.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/spatial/ChunkMap.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::spatial {

	namespace {
		// The centre of a box, on one axis, without building a `Vector3`.
		//
		// **Halved before adding rather than after**, so a box whose two bounds
		// are both near the float maximum does not sum to an infinity and then
		// halve it back to one. A scene does not usually hold such a box; a
		// scene that has just been handed a garbage transform does, and the
		// coordinate clamp below is what catches it either way.
		float MidpointOf(float minimum, float maximum) {
			return minimum * 0.5f + maximum * 0.5f;
		}
	}

	ChunkMap::ChunkMap(float chunkSize) {
		// `HashGrid`'s constructor argument: a spacing at or below zero makes the
		// reciprocal an infinity, and every chunk coordinate after it is a wrong
		// answer rather than a failure.
		Spacing = chunkSize > 0.0f ? chunkSize : DEFAULT_CHUNK_SIZE;
		InverseSpacing = 1.0f / Spacing;

		if (!(chunkSize > 0.0f)) {
			ENGINE_WARN("chunk size {} is not positive; using the default {}", chunkSize, DEFAULT_CHUNK_SIZE);
		}
	}

	void ChunkMap::Clear() {
		// Cleared, not freed, exactly as `HashGrid::Clear` is and for the same
		// reason: a map rebuilt every tick over a steady scene allocates on the
		// first tick and never again.
		Placements.clear();
		Coordinates.clear();
		Starts.clear();
		Members.clear();
		Owners.clear();
	}

	void ChunkMap::Rebuild(std::span<const Proxy> proxies) {
		const core::ScopedObservation timed("spatial.chunks.rebuild");

		Clear();

		if (proxies.empty()) {
			// One entry, so `MembersOf` on a map with no chunks is still an
			// empty span rather than a subscript past the end. Every other path
			// below leaves the same invariant.
			Starts.push_back(0);
			return;
		}

		// Pass one: where each proxy goes.
		//
		// `CellCoordinateOf` rather than a cast, and it is the same function the
		// grid bins with - see `GridInternals`, which explains why a truncating
		// cast doubles the width of the chunk at the origin. It also clamps an
		// infinite or NaN centre onto the coordinate limit, which is what keeps
		// the sort below comparing defined values.
		Placements.resize(proxies.size());
		for (size_t index = 0; index < proxies.size(); index++) {
			const core::AABB &bounds = proxies[index].Bounds;
			Placements[index] = Placement{
				ChunkCoordinate{
					CellCoordinateOf(MidpointOf(bounds.Minimum.X, bounds.Maximum.X), InverseSpacing),
					CellCoordinateOf(MidpointOf(bounds.Minimum.Y, bounds.Maximum.Y), InverseSpacing),
					CellCoordinateOf(MidpointOf(bounds.Minimum.Z, bounds.Maximum.Z), InverseSpacing),
				},
				static_cast<uint32_t>(index),
			};
		}

		// Pass two: order them.
		//
		// **The proxy index is part of the key**, so this is a total order and
		// `std::sort` not being stable cannot matter. A comparison that only read
		// the coordinate would leave the order inside a chunk to the sort's
		// internals, and `AGENTS.md` refuses a build whose iteration order is a
		// function of anything but the contents.
		//
		// A sort rather than a counting pass because the coordinates are sparse
		// and signed: counting needs a dense index, which is the dense lattice
		// this structure exists to avoid allocating.
		std::sort(Placements.begin(), Placements.end());

		// Pass three: run-length the sorted placements into chunks.
		//
		// One pass, no lookup, and the members come out ascending inside each
		// chunk for free - because the sort ordered equal coordinates by proxy
		// index, which is the property `MembersOf` promises.
		Members.resize(Placements.size());
		Owners.resize(Placements.size());
		Starts.push_back(0);

		for (size_t at = 0; at < Placements.size(); at++) {
			const Placement &placement = Placements[at];
			if (Coordinates.empty() || !(placement.Chunk == Coordinates.back())) {
				Coordinates.push_back(placement.Chunk);
				Starts.push_back(static_cast<uint32_t>(at));
			}

			Members[at] = placement.Proxy;
			Owners[placement.Proxy] = static_cast<uint32_t>(Coordinates.size() - 1);
			Starts.back() = static_cast<uint32_t>(at + 1);
		}

		// **The chunk count is the width of the parallelism a consumer can get**,
		// so one chunk holding a whole scene is a solver running on one thread
		// and nothing else saying so.
		core::Metrics::Count("spatial.chunks", static_cast<double>(Coordinates.size()));
		ENGINE_TRACE(
			"rebuilt: {} proxies into {} chunk(s) at spacing {}", proxies.size(), Coordinates.size(), Spacing
		);
		if (Coordinates.size() == 1 && proxies.size() > 1) {
			ENGINE_DEBUG_EVERY(
				5.0,
				"all {} proxies landed in one chunk at spacing {}; nothing can be split",
				proxies.size(),
				Spacing
			);
		}
	}

	void ChunkMap::SetChunkSize(float chunkSize) {
		const float resolved = chunkSize > 0.0f ? chunkSize : DEFAULT_CHUNK_SIZE;
		if (resolved == Spacing) {
			// Nothing dropped when nothing changed - `HashGrid::SetCellSize`'s
			// argument. The caller asks every time the set changes and the answer
			// is usually the same one.
			return;
		}

		ENGINE_DEBUG("chunk size {} -> {}; the partition is dropped", Spacing, resolved);

		Spacing = resolved;
		InverseSpacing = 1.0f / resolved;

		// Every membership is a function of the spacing, so keeping them would
		// answer against chunks that no longer exist.
		Clear();
	}

	float SuggestChunkSize(std::span<const Proxy> proxies, size_t groups) {
		if (proxies.empty()) {
			return ChunkMap::DEFAULT_CHUNK_SIZE;
		}

		// A caller wanting no subdivision gets one chunk, which is the honest
		// answer to "put everything in a group" and costs nothing to produce.
		if (groups <= 1) {
			return ChunkMap::MAXIMUM_CHUNK_SIZE;
		}

		// The bounding box of the centres, because the centres are what the
		// partition bins. The bounding box of the *bounds* would be wider by one
		// proxy on each side, which for a scene with a baseplate in it is the
		// baseplate's width rather than the scene's.
		//
		// In `double` for `SuggestCellSize`'s reason: the sum is not one, but a
		// float min and max over a world whose coordinates are large loses the
		// same way.
		double minimum[3] = {0.0, 0.0, 0.0};
		double maximum[3] = {0.0, 0.0, 0.0};
		bool any = false;

		for (const Proxy &proxy : proxies) {
			const double centre[3] = {
				static_cast<double>(proxy.Bounds.Minimum.X) * 0.5 +
					static_cast<double>(proxy.Bounds.Maximum.X) * 0.5,
				static_cast<double>(proxy.Bounds.Minimum.Y) * 0.5 +
					static_cast<double>(proxy.Bounds.Maximum.Y) * 0.5,
				static_cast<double>(proxy.Bounds.Minimum.Z) * 0.5 +
					static_cast<double>(proxy.Bounds.Maximum.Z) * 0.5,
			};

			// Written as a rejection rather than a `min`/`max`, so a NaN centre -
			// which compares false against everything - is skipped instead of
			// poisoning the extent. A world of NaNs then has no extent at all and
			// takes the default below, which is the only answer that is not a
			// guess.
			if (!(centre[0] == centre[0]) || !(centre[1] == centre[1]) || !(centre[2] == centre[2])) {
				continue;
			}

			for (int axis = 0; axis < 3; axis++) {
				if (!any || centre[axis] < minimum[axis]) {
					minimum[axis] = centre[axis];
				}
				if (!any || centre[axis] > maximum[axis]) {
					maximum[axis] = centre[axis];
				}
			}
			any = true;
		}

		if (!any) {
			ENGINE_WARN(
				"every one of {} proxy centres is a NaN; suggesting the default chunk size", proxies.size()
			);
			return ChunkMap::DEFAULT_CHUNK_SIZE;
		}

		// **The scene's own extent cut into `groups` boxes.** A box of volume `V`
		// divides into `groups` cubes of edge `cbrt(V / groups)`, and a scene
		// that is flat on one axis - a tray of blocks, which is the common case -
		// has a volume near zero on that axis and would ask for a chunk near
		// zero. So the edge is taken from the two widest axes rather than from
		// the volume: a flat scene is cut in two dimensions, which is what a flat
		// scene wants.
		double extent[3] = {
			maximum[0] - minimum[0],
			maximum[1] - minimum[1],
			maximum[2] - minimum[2],
		};
		std::sort(std::begin(extent), std::end(extent));

		// `extent[2]` is the widest and `extent[1]` the next. Their product cut
		// into `groups` squares gives the edge; a scene that is a line on one
		// axis falls back to cutting the one axis it has.
		const double area = extent[2] * extent[1];
		const double wanted = area > 0.0 ? std::sqrt(area / static_cast<double>(groups))
										 : extent[2] / static_cast<double>(groups);

		if (!(wanted > 0.0)) {
			// Every centre in one place. There is nothing to cut and no size that
			// would cut it, so the default is the honest answer.
			ENGINE_DEBUG(
				"{} proxy centres share one place; no chunk size cuts them into {} groups",
				proxies.size(),
				groups
			);
			return ChunkMap::DEFAULT_CHUNK_SIZE;
		}

		const auto clamped = static_cast<float>(std::clamp(
			wanted,
			static_cast<double>(ChunkMap::MINIMUM_CHUNK_SIZE),
			static_cast<double>(ChunkMap::MAXIMUM_CHUNK_SIZE)
		));

		// `exp2(round(log2(x)))` rather than a loop, matching `SuggestCellSize`,
		// so a world of buildings costs the same as a world of pebbles.
		const float quantised = std::exp2(std::round(std::log2(clamped)));
		return std::clamp(quantised, ChunkMap::MINIMUM_CHUNK_SIZE, ChunkMap::MAXIMUM_CHUNK_SIZE);
	}
}
