#pragma once

#include "SourceShape3D.hpp"

namespace engine::imagegraph::detail {
	// Caller admits storage and supplies materials before filling the source triangle buffers.
	void FillSourceShape3DAxial(const SourceShape3DRecipe &recipe, MeshData3D &mesh);
}
