#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>

#include <algorithm>
#include <cmath>

namespace engine::scene {

	namespace {
		// How many levels a ladder really offers.
		//
		// `Levels` is authored, so it is clamped rather than trusted: a value of
		// zero would make every loop below run backwards off the end, and a value
		// past `LOD_LEVELS` would index `Meshes` out of range. Both arrive from a
		// file somebody else wrote.
		uint8_t LadderDepth(const LevelOfDetail &lod) {
			if (lod.Strategy == LodStrategy::None) {
				return 1;
			}
			return static_cast<uint8_t>(std::clamp<size_t>(lod.Levels, 1, LOD_LEVELS));
		}
	}

	core::Name LevelMesh(const LevelOfDetail &lod, const core::Name &base, uint8_t level) {
		if (level == 0 || level >= LadderDepth(lod)) {
			return base;
		}

		const core::Name &named = lod.Meshes[level - 1];
		return named.IsValid() ? named : base;
	}

	uint32_t LevelTriangles(
		const LevelOfDetail &lod, const MeshCatalogue &catalogue, const core::Name &base, uint8_t level
	) {
		const uint32_t triangles = catalogue.Find(base);
		if (triangles == 0) {
			// The world has not been told what the base mesh is. Not "empty":
			// `assets::Mesh::Read` refuses a mesh with no triangles, so the two
			// cannot be confused, and `SelectLevel` turns this into "stay at level
			// zero" rather than into a guess.
			return 0;
		}
		if (level == 0 || level >= LadderDepth(lod)) {
			return triangles;
		}

		// An authored level is a mesh in its own right, so the catalogue is the
		// answer whenever it has one. It may not: a client selecting before its
		// content pump has reached the coarse meshes has the base and nothing
		// else, and falling through to the ratio is what keeps that frame
		// selecting instead of stalling at level zero.
		const core::Name named = LevelMesh(lod, base, level);
		if (named != base) {
			if (const uint32_t authored = catalogue.Find(named); authored != 0) {
				return authored;
			}
		}

		const float ratio = std::clamp(lod.Ratios[level - 1], 0.0f, 1.0f);
		const float scaled = static_cast<float>(triangles) * ratio;

		// At least one triangle, because a level of zero triangles would divide
		// the area by nothing and select itself for ever.
		return static_cast<uint32_t>(std::max(1.0f, std::floor(scaled)));
	}

	uint8_t SelectLevel(
		const LevelOfDetail &lod, const MeshCatalogue &catalogue, const core::Name &base, float projectedArea
	) {
		const uint8_t depth = LadderDepth(lod);
		if (depth <= 1 || !(projectedArea > 0.0f)) {
			// Written so a NaN area selects level zero. The obvious way round
			// makes a NaN select the coarsest level, which is a part that quietly
			// turns into a blob on whichever machine produced the NaN.
			return 0;
		}
		if (catalogue.Find(base) == 0) {
			// The world has not been told what the base mesh is, so every level's
			// count is a guess from nothing. Level zero, which is what a part
			// draws while its content is still arriving.
			return 0;
		}

		const float target = lod.TargetQuadArea > 0.0f ? lod.TargetQuadArea : DEFAULT_TARGET_QUAD_AREA;

		// **The finest level whose triangles still cover a quad each**, walking
		// from the top down. That direction is the decision: a coarser level
		// always has *more* pixels per triangle, so asking for the coarsest level
		// that clears the target would answer "the coarsest" for everything and
		// the target would do nothing. What decision 19 wants is the most detail
		// that is still worth shading, which is the first level from the top that
		// clears it.
		for (uint8_t level = 0; level < depth; level++) {
			const uint32_t triangles = LevelTriangles(lod, catalogue, base, level);
			if (triangles == 0) {
				continue;
			}
			if (projectedArea / static_cast<float>(triangles) >= target) {
				return level;
			}
		}

		// Nothing in the ladder is coarse enough, which is a part far enough away
		// that every level is under-utilised. The cheapest one is the honest
		// answer rather than the finest.
		return static_cast<uint8_t>(depth - 1);
	}
}
