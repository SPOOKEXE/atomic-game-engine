#pragma once
#include "SourceParticle2DState.hpp"

namespace engine::imagegraph::detail {
	// Native pixel-center/top-left coverage; source GPU coverage requires separate observations.
	bool DrawSourceParticle2D(NodeContext &, const SourceParticle2DControls &, SourceParticle2DState &);
}
