#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	inline constexpr size_t SOURCE_SDF_MAXIMUM_SHAPES = 16;
	inline constexpr size_t SOURCE_SDF_MAXIMUM_OPERATIONS = 32;

	// Source RM_Object uniforms, captured by value before a renderer or sampler consumes them.
	struct SourceSdfShape {
		std::string Identity;
		int32_t Shape = 101;
		Vector3 Size{1, 1, 1};
		double Radius = .7, Thickness = .2, Crop = 0, Angle = 0, Height = .5;
		Vector2 RadiusRange{.7, .1};
		double UniformSize = 1;
		Vector3 Elongate{};
		double Rounded = 0;
		Vector4 Corner{.25, .25, .25, .25};
		Vector2 Size2D{.5, .5};
		int32_t Sides = 3;
		Vector3 WaveAmplitude{4, 4, 4}, WaveIntensity{}, WavePhase{};
		int32_t TwistAxis = 0;
		double TwistAmount = 0;
		Vector3 Position{}, Rotation{};
		double Scale = 1;
		bool Tile = false;
		Vector3 TileDistance{1, 1, 1}, TileAmount{1, 1, 1};
		// Source apply() does not submit its authored random tile shifts to the shader.
		Colour Diffuse{255, 255, 255, 255};
		double Specular = 0, Reflective = 0;
		bool Volumetric = false;
		double Density = .3;
		std::optional<Image> Texture;
		bool TextureInterpolation = false;
		double TextureScale = 1, Triplanar = 1;
		bool operator==(const SourceSdfShape &) const = default;
	};
	struct SourceSdfOperation {
		// Shape indices below 100; source operation codes 100 through 103.
		int32_t Code = 0;
		double Merge = 0;
		bool operator==(const SourceSdfOperation &) const = default;
	};
	struct SdfData {
		std::vector<SourceSdfShape> Shapes;
		std::vector<SourceSdfOperation> Operations;
		bool operator==(const SdfData &) const = default;
	};
	struct SourceSdfMarchResult {
		double Depth = 0;
		uint32_t Steps = 0;
		bool Hit = false;
		Vector3 Normal{};
	};

	// Builds a primitive or composition from already captured controls without re-evaluating producers.
	Status BuildSourceSdfObject(
		const Node &node,
		const EvaluationSnapshot &snapshot,
		SdfValue &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

	// Checks the owned resource structure without sampling potentially singular primitive coordinates.
	bool ValidateSourceSdfValue(const SdfValue &sdf);

	// Samples the pinned source shader's distance kernel, including its deformation order.
	Status SampleSourceSdf(const SdfValue &sdf, Vector3 point, double &distance, Diagnostic &diagnostic);
	// Source sphere tracing: 512 steps, epsilon 1e-5, and the authored near/far interval.
	Status MarchSourceSdf(
		const SdfValue &sdf,
		Vector3 camera,
		Vector3 direction,
		Vector2 viewRange,
		SourceSdfMarchResult &result,
		Diagnostic &diagnostic
	);
}
