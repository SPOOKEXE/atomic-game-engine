#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct SourceNineSliceRecipe {
		Image Source;
		Vector4 Splice;
		int64_t FillingMode = 0;
		int64_t Interpolation = 1, Oversample = 4;
		bool operator==(const SourceNineSliceRecipe &) const = default;
	};
}
namespace engine::imagegraph::detail {
	struct NodeContext;
	Status RasterizeSourceNineSlice(
		const DynamicSurfaceValue &value,
		Vector2 dimension,
		std::array<double, 4> tint,
		Image &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	);
	// The caller owns and admits the RGBA8 target. Tint is applied inside staging,
	// before the dynamic object's Normal-blended outer draw.
	bool StageSourceNineSlice(
		NodeContext &context,
		const SourceNineSliceRecipe &recipe,
		Vector2 dimension,
		Image &target,
		std::array<double, 4> tint,
		bool render = true
	);
}
