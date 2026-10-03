#include "ImageGraphTransform3DRequest.hpp"

#include "ImageGraphTransform3DFormats.hpp"

#include <cmath>
#include <limits>

namespace engine::render::imagegraph {
	TransformImage3DStatus ValidateTransformImage3D(const TransformImage3DRequest &request) {
		const auto validSurface = [&](const TransformImage3DSurface &surface) {
			const uint32_t bytesPerPixel =
				detail::TransformImage3DBytesPerPixel(surface.Format, request.ColorSpace);
			if (surface.Width == 0 || surface.Height == 0 || bytesPerPixel == 0) return false;
			const uint64_t pixels = uint64_t(surface.Width) * surface.Height;
			return pixels <= std::numeric_limits<uint64_t>::max() / bytesPerPixel &&
				   surface.Pixels.size() == pixels * assets::BytesPerPixel(surface.Format);
		};
		if (!validSurface(request.Front)) return TransformImage3DStatus::InvalidSurface;
		if (!request.Back.Pixels.empty() && !validSurface(request.Back))
			return TransformImage3DStatus::InvalidSurface;
		if (!request.SourcePlane && !request.Back.Pixels.empty() &&
			(request.Back.Width != request.Front.Width || request.Back.Height != request.Front.Height))
			return TransformImage3DStatus::InvalidSurface;
		if (request.Front.Width > 4096 || request.Front.Height > 4096 || request.Back.Width > 4096 ||
			request.Back.Height > 4096)
			return TransformImage3DStatus::OutputLimit;
		if ((request.SourcePlane && request.ColorSpace != TransformImage3DColorSpace::Linear) ||
			(request.ColorSpace != TransformImage3DColorSpace::Linear &&
			 request.ColorSpace != TransformImage3DColorSpace::Display))
			return TransformImage3DStatus::InvalidControl;
		const uint64_t pixels = uint64_t(request.Front.Width) * request.Front.Height;
		const uint32_t frontBytesPerPixel =
			detail::TransformImage3DBytesPerPixel(request.Front.Format, request.ColorSpace);
		const uint32_t backBytesPerPixel = detail::TransformImage3DBytesPerPixel(
			request.Back.Pixels.empty() ? request.Front.Format : request.Back.Format, request.ColorSpace
		);
		const uint64_t outputBytes =
			pixels * (request.SourcePlane ? 4 : assets::BytesPerPixel(request.Front.Format));
		const uint64_t frontTransferBytes = pixels * frontBytesPerPixel;
		const uint64_t backPixels =
			request.Back.Pixels.empty() ? pixels : uint64_t(request.Back.Width) * request.Back.Height;
		const uint64_t backTransferBytes = backPixels * backBytesPerPixel;
		const uint64_t outputTransferBytes = pixels * (request.SourcePlane ? 4 : frontBytesPerPixel);
		const uint64_t depthBytes = pixels * 4;
		const uint64_t scratchBytes =
			2 * (frontTransferBytes + backTransferBytes + outputTransferBytes + 2 * depthBytes);
		if (outputBytes > MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES ||
			scratchBytes > MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES)
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
