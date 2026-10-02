#include "AtlasPayload.hpp"

#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	uint64_t AtlasStorageBytes(const AtlasValue &value, bool retained) {
		if (!value.Data) return 0;
		uint64_t bytes = sizeof(AtlasData);
		const auto imageBytes = [&](const Image &image) {
			const uint64_t extra = retained ? image.Pixels.capacity() : image.Pixels.size();
			if (extra > UINT64_MAX - bytes)
				bytes = UINT64_MAX;
			else
				bytes += extra;
		};
		imageBytes(value.Data->Surface.Data);
		if (value.Data->OriginalSurface) imageBytes(value.Data->OriginalSurface->Data);
		return bytes;
	}
	bool ValidAtlasPayload(const AtlasValue &value) {
		if (!value.Data) return true;
		const auto &data = *value.Data;
		const auto number = [](double value) {
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
		};
		const auto vector = [&](Vector2 value) { return number(value.X) && number(value.Y); };
		const auto surface = [](const Image &image) {
			if (!image.Width && !image.Height && image.Pixels.empty())
				return DescribeSurfaceFormat(image.Format).has_value();
			return ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) &&
				   FiniteSurfaceSamples(image);
		};
		return (data.Kind == AtlasKind::Atlas || data.Kind == AtlasKind::SurfaceAtlas) &&
			   vector(data.Position) && vector(data.Scale) && vector(data.Dimension) &&
			   vector(data.OriginalDimension) && number(data.RotationDegrees) && number(data.Alpha) &&
			   data.Dimension.X >= 0 && data.Dimension.Y >= 0 && data.OriginalDimension.X >= 0 &&
			   data.OriginalDimension.Y >= 0 && surface(data.Surface.Data) &&
			   (!data.OriginalSurface || surface(data.OriginalSurface->Data)) &&
			   AtlasStorageBytes(value, true) <= Limits::MaximumEvaluationBytes;
	}
}
