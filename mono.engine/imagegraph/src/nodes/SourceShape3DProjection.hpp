#pragma once

#include "SourceShape3DRaster.hpp"

#include <vector>

namespace engine::imagegraph::detail {
	// NativeTopOrigin uses the engine image row convention and [0,1] depth.
	// HTML5FramebufferRows follows the audited GameMaker HTML5 surface framebuffer rows and GL depth.
	// Neither profile asserts equivalence to the pinned Windows renderer.
	enum class SourceShape3DFramebufferProfile { NativeTopOrigin, HTML5FramebufferRows };
	struct SourceShape3DProjectionResult {
		AllocationReservation Charge;
		std::vector<SourceShape3DRasterTriangle> Triangles;
	};
	// Applies the pinned world stack and fixed source camera in double arithmetic.
	// Zero source normals stay zero explicitly; shader normalize(0) has no portable GPU result.
	bool ProjectSourceShape3DVertex(
		const SourceShape3DRecipe &recipe,
		const MeshVertex3D &vertex,
		SourceShape3DFramebufferProfile profile,
		SourceShape3DRasterVertex &result
	);
	// Accepts local triangles produced by BuildSourceShape3DGeometry, not arbitrary transformed meshes.
	// Admission precedes allocation. Refusal leaves the result and its charge unchanged.
	bool BuildSourceShape3DProjection(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		const MeshValue3D &geometry,
		SourceShape3DFramebufferProfile profile,
		SourceShape3DProjectionResult &result
	);
}
