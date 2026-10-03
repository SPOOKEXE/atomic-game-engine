#pragma once

#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraph {
	std::optional<uint64_t> SourceCameraSceneRetainedBytes(const SceneValue3D &);
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

	Status ResolveSourceCameraPose(
		std::span<const AuthoredValue>, uint32_t width, uint32_t height, SourceCameraPose &, Diagnostic &
	);
	Status ResolveSourceCameraDimensions(
		const Node &,
		const SourceCameraEvaluationPolicy &,
		std::span<const AuthoredValue>,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &
	);
	Status ResolveSourceCameraSurfaceFormat(
		const Node &,
		const SourceCameraEvaluationPolicy &,
		std::span<const AuthoredValue>,
		std::optional<SurfaceFormat> inherited,
		SurfaceFormat &,
		Diagnostic &
	);
	Status ResolveSourceCameraShader(
		const SourceCameraEvaluationPolicy &, std::span<const AuthoredValue>, uint32_t &shader, Diagnostic &
	);

	Status ResolveSourceCameraDimensions(
		const Node &,
		const SourceCameraEvaluationPolicy &,
		std::span<const EvaluationInputValue>,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &
	);
	Status ResolveSourceCameraSurfaceFormat(
		const Node &,
		const SourceCameraEvaluationPolicy &,
		std::span<const EvaluationInputValue>,
		std::optional<SurfaceFormat> inherited,
		SurfaceFormat &,
		Diagnostic &
	);
	Status ResolveSourceCameraShader(
		const SourceCameraEvaluationPolicy &,
		std::span<const EvaluationInputValue>,
		uint32_t &shader,
		Diagnostic &
	);
}
