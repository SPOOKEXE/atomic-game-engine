#include "ImageGraphTransform3DRequest.hpp"

#include <cmath>

namespace engine::render::imagegraph {
	TransformImage3DStatus ValidateTransformImage3D(const TransformImage3DRequest &request) {
		const auto validSurface = [](const TransformImage3DSurface &surface) {
			return surface.Width != 0 && surface.Height != 0 &&
				   surface.Rgba8.size() == uint64_t(surface.Width) * surface.Height * 4;
		};
		if (!validSurface(request.Front)) return TransformImage3DStatus::InvalidSurface;
		if (!request.Back.Rgba8.empty() && !validSurface(request.Back))
			return TransformImage3DStatus::InvalidSurface;
		if (!request.Back.Rgba8.empty() &&
			(request.Back.Width != request.Front.Width || request.Back.Height != request.Front.Height))
			return TransformImage3DStatus::InvalidSurface;
		const uint64_t outputBytes = uint64_t(request.Front.Width) * request.Front.Height * 4;
		constexpr uint64_t RESOURCE_OUTPUT_MULTIPLIER = 10;
		if (request.Front.Width > 4096 || request.Front.Height > 4096 ||
			outputBytes > MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES ||
			outputBytes > MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES / RESOURCE_OUTPUT_MULTIPLIER)
			return TransformImage3DStatus::OutputLimit;
		const auto finite = [](float value) { return std::isfinite(value); };
		for (float value : request.Position)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.Anchor)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.Scale)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.Rotation)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.TextureTiling)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.ViewRange)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		for (float value : request.DepthRange)
			if (!finite(value)) return TransformImage3DStatus::InvalidControl;
		const double rotationLength = std::sqrt(
			double(request.Rotation[0]) * request.Rotation[0] +
			double(request.Rotation[1]) * request.Rotation[1] +
			double(request.Rotation[2]) * request.Rotation[2] +
			double(request.Rotation[3]) * request.Rotation[3]
		);
		if (!std::isfinite(rotationLength) || rotationLength <= 0 || request.ViewRange[0] <= 0 ||
			request.ViewRange[1] <= request.ViewRange[0] || request.DepthRange[0] == request.DepthRange[1] ||
			(request.Projection == TransformImage3DProjection::Perspective &&
			 (!finite(request.FieldOfViewDegrees) || request.FieldOfViewDegrees <= 0 ||
			  request.FieldOfViewDegrees >= 180)))
			return TransformImage3DStatus::InvalidControl;
		return TransformImage3DStatus::Ok;
	}
}
