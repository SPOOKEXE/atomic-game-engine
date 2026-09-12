#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace engine::scene {
	core::Name AutoMeshLodArtifactName(const core::Name &base, uint8_t level, float ratio) {
		if (!base.IsValid() || level == 0 || !(ratio > 0.0f) || ratio > 1.0f) {
			return {};
		}
		return core::Name(
			std::string(base.Text()) + ".auto-lod-" + std::to_string(level) + "-" +
			std::to_string(std::bit_cast<uint32_t>(ratio))
		);
	}

	LevelOfDetail
	ResolveMeshLOD(const core::Name &base, const AutoMeshLOD *automatic, const CustomMeshLOD *custom) {
		LevelOfDetail resolved;
		const uint8_t automaticLevels =
			automatic == nullptr ? 1
								 : static_cast<uint8_t>(std::clamp<size_t>(automatic->Levels, 1, LOD_LEVELS));
		const uint8_t customLevels =
			custom == nullptr ? 1 : static_cast<uint8_t>(std::clamp<size_t>(custom->Levels, 1, LOD_LEVELS));
		bool customSelected = false;

		for (size_t slot = 0; slot < LOD_LEVELS - 1; ++slot) {
			const uint8_t level = static_cast<uint8_t>(slot + 1);
			const bool customAvailable =
				custom != nullptr && level < customLevels && custom->Meshes[slot].IsValid();
			const core::Name automaticMesh =
				automatic == nullptr || level >= automaticLevels
					? core::Name{}
					: (automatic->Meshes[slot].IsValid()
						   ? automatic->Meshes[slot]
						   : AutoMeshLodArtifactName(base, level, automatic->Ratios[slot]));
			const bool automaticAvailable = automaticMesh.IsValid();
			if (!customAvailable && !automaticAvailable) {
				break;
			}

			resolved.Meshes[slot] = customAvailable ? custom->Meshes[slot] : automaticMesh;
			const float customRatio = customAvailable ? custom->Ratios[slot] : 0.0f;
			const float automaticRatio = automaticAvailable ? automatic->Ratios[slot] : 0.0f;
			if (customRatio > 0.0f) {
				resolved.Ratios[slot] = std::clamp(customRatio, 0.0f, 1.0f);
			} else if (automaticRatio > 0.0f) {
				resolved.Ratios[slot] = std::clamp(automaticRatio, 0.0f, 1.0f);
			}
			resolved.Levels = static_cast<uint8_t>(level + 1);
			customSelected |= customAvailable;
		}

		if (resolved.Levels <= 1) {
			return resolved;
		}
		resolved.TargetQuadArea = custom != nullptr && custom->TargetQuadArea > 0.0f
									  ? custom->TargetQuadArea
									  : (automatic == nullptr ? 0.0f : automatic->TargetQuadArea);
		resolved.Strategy =
			customSelected || automatic == nullptr
				? LodStrategy::Authored
				: (automatic->Strategy == LodStrategy::None || automatic->Strategy == LodStrategy::Authored
					   ? LodStrategy::Decimated
					   : automatic->Strategy);
		return resolved;
	}

	LevelOfDetail ResolveMeshLOD(const AutoMeshLOD *automatic, const CustomMeshLOD *custom) {
		return ResolveMeshLOD({}, automatic, custom);
	}

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
