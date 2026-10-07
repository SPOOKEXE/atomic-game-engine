#pragma once

#include "../EvaluationBudget.hpp"
#include "SourceShape3D.hpp"

#include <span>

namespace engine::imagegraph::detail {
	// Device-independent orthographic cut point. Screen is in pixels, Y downward.
	// ClipDepth is the shader's gl_Position.z; TestDepth is normalized hardware depth.
	// ViewNormal must already be normalized per vertex, as the source vertex shader does.
	struct SourceShape3DRasterVertex {
		Vector2 Screen{}, UV{};
		double ClipDepth = 0, TestDepth = 0;
		Vector3 ViewNormal{};
		Colour Tint{255, 255, 255, 255};
	};
	struct SourceShape3DRasterTriangle {
		std::array<SourceShape3DRasterVertex, 3> Vertices;
		size_t Submesh = 0;
	};
	struct SourceShape3DRasterResult {
		// Charge precedes storage so storage dies before its reservation.
		AllocationReservation Charge;
		std::array<Image, 3> Images;
	};
	// Raster base only: caller verifies source matrices, viewport orientation and blend setup.
	// Counterclockwise screen triangles are culled; depth uses less-or-equal and writes even at alpha zero.
	// Orthographic varyings are affine and vertex-normalized normals are not renormalized per fragment.
	// Outputs are raw shader attachments, with optional straight-alpha over background on attachment 0.
	// No context outputs are published, and failure leaves result intact.
	bool RasterSourceShape3D(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		std::span<const SourceShape3DRasterTriangle> triangles,
		std::span<const Image *const> textures,
		const Image *background,
		SurfaceFormat format,
		SourceShape3DRasterResult &result
	);
}
