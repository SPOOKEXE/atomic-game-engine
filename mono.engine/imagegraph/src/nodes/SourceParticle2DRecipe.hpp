#pragma once
#include "SourceParticle2DState.hpp"
#include <memory>

namespace engine::imagegraph::detail {
	struct SourceParticle2DPrepared {
		AllocationReservation Charge;
		std::vector<Image> Sprites;
		std::vector<std::optional<Vector4>> SpriteAtlasRects;
		std::vector<Colour> Palette;
		std::vector<Vector2> SpawnData, PoissonPoints;
		std::vector<SourceParticle2DRotation> Directions, Rotations;
		std::unique_ptr<PathRuntime> SpawnRuntime, FollowRuntime;
		SourceParticle2DControls Controls;
		SourceParticle2DPrepared();
		~SourceParticle2DPrepared();
	};
	bool PrepareSourceParticle2DControls(NodeContext &, SourceParticle2DPrepared &);
}
