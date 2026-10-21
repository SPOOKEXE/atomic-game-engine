#pragma once

#include "FrameObservation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace client::tests {
	struct ImageRegion {
		uint32_t Left = 0;
		uint32_t Top = 0;
		uint32_t Right = 0;
		uint32_t Bottom = 0;
		bool operator==(const ImageRegion &) const = default;
	};

	struct PixelStatistics {
		bool Available = false;
		std::string Failure;
		ImageRegion Region;
		size_t Samples = 0;
		size_t NonBlack = 0;
		size_t Matching = 0;
	};

	inline bool CanonicalImage(const engine::render::SceneCaptureResult &image) {
		using engine::render::ResourceImageFormat;
		if (image.Status != engine::render::SceneCaptureStatus::Ready || !image.Width || !image.Height)
			return false;
		if (image.Format != ResourceImageFormat::RGBA8_UNorm &&
			image.Format != ResourceImageFormat::RGBA8_SRGB)
			return false;
		const size_t width = image.Width;
		if (width > std::numeric_limits<size_t>::max() / 4 || image.RowStride != width * 4) return false;
		if (image.Height > std::numeric_limits<size_t>::max() / image.RowStride) return false;
		return image.Pixels.size() == image.RowStride * image.Height;
	}

	// Callers supply their colour oracle. The region is clamped before reading owned bytes.
	template <typename Predicate>
	PixelStatistics
	CountPixels(const engine::render::SceneCaptureResult &image, ImageRegion region, Predicate matches) {
		PixelStatistics result;
		if (!CanonicalImage(image)) {
			result.Failure = "capture is not a complete canonical RGBA image";
			return result;
		}
		region.Left = std::min(region.Left, image.Width);
		region.Right = std::clamp(region.Right, region.Left, image.Width);
		region.Top = std::min(region.Top, image.Height);
		region.Bottom = std::clamp(region.Bottom, region.Top, image.Height);
		result.Available = true;
		result.Region = region;
		for (uint32_t y = region.Top; y < region.Bottom; ++y) {
			for (uint32_t x = region.Left; x < region.Right; ++x) {
				const size_t offset = y * image.RowStride + size_t(x) * 4;
				const auto red = image.Pixels[offset], green = image.Pixels[offset + 1],
						   blue = image.Pixels[offset + 2];
				result.Samples++;
				result.NonBlack += red != 0 || green != 0 || blue != 0;
				result.Matching += matches(red, green, blue);
			}
		}
		return result;
	}

	enum class ProjectionStatus { Candidate, Outside, NearClipped, MissingWorldMapping, Invalid };

	struct ProjectedBounds {
		ProjectionStatus Status = ProjectionStatus::Invalid;
		ImageRegion Region;
		std::string Detail;
	};

	struct ObjectWorldMapping {
		std::string SourceWorld;
		std::string EyeWorld;
		glm::mat4 SourceToEye{1.0f};
	};

	// Bounds identify a candidate region only. Occlusion, mesh coverage and visibility
	// still require an image oracle. Near-clipped boxes need a clipping-aware check.
	inline ProjectedBounds ProjectObjectBounds(
		const engine::render::SceneCaptureResult &image,
		const FrameObjectObservation &object,
		const std::optional<ObjectWorldMapping> &mapping = std::nullopt
	) {
		if (!CanonicalImage(image)) return {ProjectionStatus::Invalid, {}, "unavailable canonical image"};
		glm::mat4 worldMapping{1.0f};
		if (object.World != image.WorldName) {
			if (!mapping || mapping->SourceWorld != object.World || mapping->EyeWorld != image.WorldName)
				return {ProjectionStatus::MissingWorldMapping, {}, "object and eye use different charts"};
			worldMapping = mapping->SourceToEye;
		}
		const auto extent = object.HalfExtent;
		if (!std::isfinite(extent.X) || !std::isfinite(extent.Y) || !std::isfinite(extent.Z) ||
			extent.X <= 0 || extent.Y <= 0 || extent.Z <= 0)
			return {ProjectionStatus::Invalid, {}, "object bounds are not positive finite extents"};
		const glm::mat4 clipFromObject = image.Projection * image.CameraFrame.Inverse().ToMatrix() *
										 worldMapping * object.Frame.ToMatrix();
		float left = std::numeric_limits<float>::infinity(), top = left;
		float right = -left, bottom = -left;
		bool beyondFar = true;
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec4 point = clipFromObject * glm::vec4(
														 (corner & 1) ? extent.X : -extent.X,
														 (corner & 2) ? extent.Y : -extent.Y,
														 (corner & 4) ? extent.Z : -extent.Z,
														 1.0f
													 );
			if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
				!std::isfinite(point.w))
				return {ProjectionStatus::Invalid, {}, "nonfinite projection"};
			if (point.w <= 0 || point.z < 0)
				return {ProjectionStatus::NearClipped, {}, "bounds meet or pass behind the near plane"};
			beyondFar &= point.z > point.w;
			const float x = point.x / point.w, y = point.y / point.w;
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
		if (beyondFar || right <= -1 || left >= 1 || bottom <= -1 || top >= 1)
			return {ProjectionStatus::Outside, {}, "bounds miss the captured frustum"};
		const auto horizontal = [&](float x) {
			return (std::clamp(x, -1.0f, 1.0f) + 1) * 0.5f * image.Width;
		};
		const auto vertical = [&](float y) { return (1 - std::clamp(y, -1.0f, 1.0f)) * 0.5f * image.Height; };
		ImageRegion region{
			static_cast<uint32_t>(std::floor(horizontal(left))),
			static_cast<uint32_t>(std::floor(vertical(bottom))),
			static_cast<uint32_t>(std::ceil(horizontal(right))),
			static_cast<uint32_t>(std::ceil(vertical(top)))
		};
		return {
			ProjectionStatus::Candidate, region, "projected owned bounds; visibility requires pixel evidence"
		};
	}
}
