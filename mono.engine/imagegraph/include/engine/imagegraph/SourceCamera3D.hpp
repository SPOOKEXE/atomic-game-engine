#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	enum class SourceCameraProjection : uint8_t { Perspective, Orthographic, Custom };
	struct SourceCameraPose {
		Vector3 Position{}, Target{}, Up{0, 0, -1};
		SourceCameraProjection Projection = SourceCameraProjection::Orthographic;
		double FieldOfViewDegrees = 60;
		Vector2 ClippingDistance{1, 10}, OrthographicViewSize{2, 2};
		std::array<double, 16> CustomProjection{};
		bool operator==(const SourceCameraPose &) const = default;
	};
	Status ResolveSourceCameraPose(
		std::span<const EvaluationInputValue> values,
		uint32_t width,
		uint32_t height,
		SourceCameraPose &result,
		Diagnostic &diagnostic
	);

	// Source left-handed lookat and zero-to-one depth projection, stored column-major for GPU upload.
	Status ResolveSourceCameraMatrices(
		const SourceCameraPose &pose,
		uint32_t width,
		uint32_t height,
		std::array<double, 16> &view,
		std::array<double, 16> &projection,
		Diagnostic &diagnostic
	);
	Status ResolveSourceCameraDimensions(
		const Document &document,
		const Node &node,
		std::span<const EvaluationInputValue> values,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &diagnostic
	);
	Status ResolveSourceCameraSurfaceFormat(
		const Document &document,
		const Node &node,
		std::span<const EvaluationInputValue> values,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	);
	Status ResolveSourceCameraSurfaceFormat(
		const Document &document,
		const Node &node,
		const EvaluationSnapshot &snapshot,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	);
	Status ResolveSourceCameraShader(
		const Document &document,
		std::span<const EvaluationInputValue> values,
		uint32_t &shader,
		Diagnostic &diagnostic
	);
}
