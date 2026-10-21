#pragma once

#include "FrameObservation.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
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
		// The coordinate chart of object.Frame, never object.SourceWorld provenance.
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
		if (mapping) {
			if (mapping->SourceWorld != object.World || mapping->EyeWorld != image.WorldName)
				return {
					ProjectionStatus::MissingWorldMapping,
					{},
					"explicit mapping does not name object and eye charts"
				};
			worldMapping = mapping->SourceToEye;
		} else if (object.World != image.WorldName)
			return {ProjectionStatus::MissingWorldMapping, {}, "object and eye use different charts"};
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

	struct PortalAperture {
		engine::core::Vector3 Centre;
		engine::core::Vector3 Normal;
		engine::core::Vector3 First;
		engine::core::Vector3 Second;
	};

	struct ObjectProjectionRoute {
		ObjectWorldMapping Mapping;
		std::optional<PortalAperture> Aperture;
	};

	enum class ObjectPixelStatus { Tested, Outside, Hidden, Unsupported, Invalid };

	struct ObjectPixelStatistics {
		ObjectPixelStatus Status = ObjectPixelStatus::Invalid;
		PixelStatistics Pixels;
		std::string Detail;
	};

	inline bool FiniteMatrix(const glm::mat4 &matrix) {
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (!std::isfinite(matrix[column][row])) return false;
		return true;
	}

	inline glm::vec3 ImageVector(const engine::core::Vector3 &vector) {
		return {vector.X, vector.Y, vector.Z};
	}

	inline bool InsideAperture(glm::vec3 point, const PortalAperture &aperture) {
		const auto first = ImageVector(aperture.First), second = ImageVector(aperture.Second);
		const auto relative = point - ImageVector(aperture.Centre);
		const float firstLength = glm::dot(first, first), secondLength = glm::dot(second, second);
		return firstLength > 0 && secondLength > 0 && std::abs(glm::dot(relative, first)) <= firstLength &&
			   std::abs(glm::dot(relative, second)) <= secondLength;
	}

	// The same sample predicate as seam-mask.glsl, evaluated in the copied row's chart.
	inline bool KeepObjectSurface(glm::vec3 point, const FrameObjectObservation &object) {
		const auto normal = ImageVector(object.SeamNormal);
		if (glm::dot(normal, normal) == 0) return true;
		const float distance = glm::dot(point, normal) - object.SeamOffset;
		if (object.SeamMask == 0) return distance >= 0;
		const bool inside = InsideAperture(
			point, {object.SeamCentre, object.SeamNormal, object.SeamFirst, object.SeamSecond}
		);
		return object.SeamMask == 1 ? distance >= 0 || !inside : distance >= 0 && inside;
	}

	// Count positive appearance only on actual supported box entry surfaces. A colour
	// sample still cannot prove absence of occlusion by another object of that colour.
	template <typename Predicate>
	ObjectPixelStatistics CountObjectPixels(
		const engine::render::SceneCaptureResult &image,
		const FrameObjectObservation &object,
		Predicate matches,
		const std::optional<ObjectProjectionRoute> &route = std::nullopt,
		std::span<uint8_t> unionMask = {}
	) {
		if (!CanonicalImage(image)) return {ObjectPixelStatus::Invalid, {}, "incomplete canonical image"};
		if (!object.Mesh.empty() && object.Mesh != "engine.Cube")
			return {ObjectPixelStatus::Unsupported, {}, "only actual cube mesh surfaces are supported"};
		if (!route && object.HiddenForEye)
			return {ObjectPixelStatus::Hidden, {}, "row excluded from the direct eye"};
		if (object.SeamMask > 2) return {ObjectPixelStatus::Unsupported, {}, "unknown seam sample mode"};
		if (!unionMask.empty() && unionMask.size() != size_t(image.Width) * image.Height)
			return {ObjectPixelStatus::Invalid, {}, "union mask size does not match image"};
		const glm::mat4 cameraMatrix = image.CameraFrame.ToMatrix();
		glm::mat4 mappingMatrix = route ? route->Mapping.SourceToEye : glm::mat4(1.0f);
		if (!FiniteMatrix(cameraMatrix) || std::abs(glm::determinant(cameraMatrix)) < 1e-12f)
			return {ObjectPixelStatus::Invalid, {}, "nonfinite or singular camera transform"};
		// Inverting a native scaled seam can round its affine bottom-right away
		// from one. Canonicalize only the small float error, not a projective route.
		constexpr float AFFINE_ROUNDING = 1e-6f;
		if (!FiniteMatrix(mappingMatrix) || std::abs(mappingMatrix[0][3]) > AFFINE_ROUNDING ||
			std::abs(mappingMatrix[1][3]) > AFFINE_ROUNDING ||
			std::abs(mappingMatrix[2][3]) > AFFINE_ROUNDING ||
			std::abs(mappingMatrix[3][3] - 1) > AFFINE_ROUNDING)
			return {ObjectPixelStatus::Invalid, {}, "object route must be a finite affine mapping"};
		mappingMatrix[0][3] = mappingMatrix[1][3] = mappingMatrix[2][3] = 0;
		mappingMatrix[3][3] = 1;
		auto mapping = route ? std::optional<ObjectWorldMapping>(route->Mapping) : std::nullopt;
		if (mapping) mapping->SourceToEye = mappingMatrix;
		const auto projected = ProjectObjectBounds(image, object, mapping);
		if (projected.Status == ProjectionStatus::Outside)
			return {ObjectPixelStatus::Outside, {}, projected.Detail};
		if (projected.Status == ProjectionStatus::NearClipped ||
			projected.Status == ProjectionStatus::MissingWorldMapping)
			return {ObjectPixelStatus::Unsupported, {}, projected.Detail};
		if (projected.Status != ProjectionStatus::Candidate)
			return {ObjectPixelStatus::Invalid, {}, projected.Detail};
		const glm::mat4 rowFromLocal = object.Frame.ToMatrix();
		const glm::mat4 eyeFromLocal = mappingMatrix * rowFromLocal;
		if (!FiniteMatrix(eyeFromLocal) || !FiniteMatrix(image.Projection) ||
			!FiniteMatrix(image.CameraFrame.ToMatrix()) ||
			std::abs(glm::determinant(eyeFromLocal)) < 1e-12f ||
			std::abs(glm::determinant(image.Projection)) < 1e-12f)
			return {ObjectPixelStatus::Invalid, {}, "nonfinite or singular object/camera projection"};
		if (!std::isfinite(object.SeamOffset))
			return {ObjectPixelStatus::Invalid, {}, "nonfinite seam offset"};
		for (const auto &vector : {object.SeamNormal, object.SeamCentre, object.SeamFirst, object.SeamSecond})
			if (!std::isfinite(vector.X) || !std::isfinite(vector.Y) || !std::isfinite(vector.Z))
				return {ObjectPixelStatus::Invalid, {}, "nonfinite seam geometry"};
		if (route && route->Aperture) {
			const auto &aperture = *route->Aperture;
			for (const auto &vector : {aperture.Centre, aperture.Normal, aperture.First, aperture.Second})
				if (!std::isfinite(vector.X) || !std::isfinite(vector.Y) || !std::isfinite(vector.Z))
					return {ObjectPixelStatus::Invalid, {}, "nonfinite portal aperture"};
			if (aperture.Normal.Magnitude() <= 0 || aperture.First.Magnitude() <= 0 ||
				aperture.Second.Magnitude() <= 0)
				return {ObjectPixelStatus::Invalid, {}, "degenerate portal aperture"};
		}
		const glm::mat4 eyeFromClip = cameraMatrix * glm::inverse(image.Projection);
		const glm::mat4 localFromEye = glm::inverse(eyeFromLocal);
		if (!FiniteMatrix(eyeFromClip) || !FiniteMatrix(localFromEye))
			return {ObjectPixelStatus::Invalid, {}, "nonfinite inverse projection or object transform"};
		// Failed calls must not publish partial positive pixels into a variant union.
		std::vector<uint8_t> stagedMask(unionMask.size(), 0);
		const auto extent = ImageVector(object.HalfExtent);
		ObjectPixelStatistics result{ObjectPixelStatus::Tested, {}, "actual box surface samples"};
		result.Pixels.Available = true;
		result.Pixels.Region = projected.Region;
		for (uint32_t y = projected.Region.Top; y < projected.Region.Bottom; ++y) {
			for (uint32_t x = projected.Region.Left; x < projected.Region.Right; ++x) {
				const float screenX = (float(x) + .5f) / image.Width * 2 - 1;
				const float screenY = 1 - (float(y) + .5f) / image.Height * 2;
				glm::vec4 nearPoint = eyeFromClip * glm::vec4(screenX, screenY, 0, 1);
				glm::vec4 farPoint = eyeFromClip * glm::vec4(screenX, screenY, 1, 1);
				if (nearPoint.w == 0 || farPoint.w == 0)
					return {ObjectPixelStatus::Unsupported, {}, "infinite unprojection endpoint"};
				nearPoint /= nearPoint.w;
				farPoint /= farPoint.w;
				for (int axis = 0; axis < 4; ++axis)
					if (!std::isfinite(nearPoint[axis]) || !std::isfinite(farPoint[axis]))
						return {ObjectPixelStatus::Invalid, {}, "nonfinite unprojection endpoint"};
				const glm::vec3 origin = glm::vec3(localFromEye * nearPoint);
				const glm::vec3 direction = glm::vec3(localFromEye * (farPoint - nearPoint));
				float enter = 0, leave = 1;
				bool intersects = true;
				for (int axis = 0; axis < 3; ++axis) {
					if (std::abs(direction[axis]) < 1e-12f) {
						if (origin[axis] < -extent[axis] || origin[axis] > extent[axis]) intersects = false;
						continue;
					}
					const float first = (-extent[axis] - origin[axis]) / direction[axis];
					const float second = (extent[axis] - origin[axis]) / direction[axis];
					enter = std::max(enter, std::min(first, second));
					leave = std::min(leave, std::max(first, second));
				}
				if (!intersects || enter > leave || enter <= 0 || enter > 1) continue;
				const auto localHit = origin + enter * direction;
				const auto rowHit = glm::vec3(rowFromLocal * glm::vec4(localHit, 1));
				if (!KeepObjectSurface(rowHit, object)) continue;
				if (route && route->Aperture) {
					const auto &aperture = *route->Aperture;
					const auto eyeDirection = glm::vec3(farPoint - nearPoint);
					const auto normal = ImageVector(aperture.Normal);
					const float divisor = glm::dot(eyeDirection, normal);
					if (std::abs(divisor) < 1e-12f) continue;
					const float parameter =
						glm::dot(ImageVector(aperture.Centre) - glm::vec3(nearPoint), normal) / divisor;
					if (parameter < 0 || parameter > enter ||
						!InsideAperture(glm::vec3(nearPoint) + parameter * eyeDirection, aperture))
						continue;
				}
				const size_t pixel = size_t(y) * image.Width + x;
				const size_t offset = pixel * 4;
				const auto red = image.Pixels[offset], green = image.Pixels[offset + 1],
						   blue = image.Pixels[offset + 2];
				result.Pixels.Samples++;
				result.Pixels.NonBlack += red != 0 || green != 0 || blue != 0;
				const bool matching = matches(red, green, blue);
				result.Pixels.Matching += matching;
				if (matching && !stagedMask.empty()) stagedMask[pixel] = 1;
			}
		}
		for (size_t pixel = 0; pixel < unionMask.size(); ++pixel)
			unionMask[pixel] |= stagedMask[pixel];
		return result;
	}
}
