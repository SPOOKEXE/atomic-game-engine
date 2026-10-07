#pragma once

// Source local-space triangles for Draw Shape 3D. The caller keeps the allocation charge through raster.

#include "../EvaluationBudget.hpp"
#include "SourceShape3D.hpp"

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	class NodeContext;
	// No graph output is published. Both result and charge remain unchanged on refusal.
	bool BuildSourceShape3DGeometry(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		MeshValue3D &result,
		AllocationReservation &charge
	);
}
