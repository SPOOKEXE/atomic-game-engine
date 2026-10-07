#pragma once

#include "SourceShape3D.hpp"

namespace engine::imagegraph::detail {
	// Counts source VB vertices in material order before the caller admits storage.
	bool SourceShape3DRoundCounts(const SourceShape3DRecipe &recipe, std::array<uint64_t, 2> &counts);
	void FillSourceShape3DRound(const SourceShape3DRecipe &recipe, MeshData3D &mesh);
}
