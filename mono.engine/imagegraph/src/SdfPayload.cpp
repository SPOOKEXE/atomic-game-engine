#include "SdfPayload.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph {
	SdfValue::SdfValue() = default;
	SdfValue::~SdfValue() = default;
	SdfValue::SdfValue(const SdfValue &) = default;
	SdfValue &SdfValue::operator=(const SdfValue &) = default;
	SdfValue::SdfValue(SdfValue &&) noexcept = default;
	SdfValue &SdfValue::operator=(SdfValue &&) noexcept = default;
	bool SdfValue::operator==(const SdfValue &other) const {
		return Data == other.Data;
	}
}
namespace engine::imagegraph::detail {
	uint64_t SdfStorageBytes(const SdfValue &value, bool retained) {
		if (!value.Data) return 0;
		const auto &data = *value.Data;
		uint64_t bytes =
			sizeof(SdfData) +
			(retained ? data.Shapes.capacity() : data.Shapes.size()) * sizeof(SourceSdfShape) +
			(retained ? data.Operations.capacity() : data.Operations.size()) * sizeof(SourceSdfOperation);
		for (const auto &shape : data.Shapes) {
			const uint64_t extra =
				(retained ? shape.Identity.capacity() : shape.Identity.size()) +
				(shape.Texture ? (retained ? shape.Texture->Pixels.capacity() : shape.Texture->Pixels.size())
							   : 0);
			if (extra > UINT64_MAX - bytes) return UINT64_MAX;
			bytes += extra;
		}
		return bytes;
	}
	bool ValidSdfPayload(const SdfValue &value) {
		if (!value.Data) return true;
		const auto &data = *value.Data;
		if (data.Shapes.size() > SOURCE_SDF_MAXIMUM_SHAPES ||
			data.Operations.size() > SOURCE_SDF_MAXIMUM_OPERATIONS ||
			SdfStorageBytes(value, true) > Limits::MaximumEvaluationBytes)
			return false;
		const auto number = [](double value) {
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
		};
		const auto vector = [&](auto value) { return number(value.X) && number(value.Y) && number(value.Z); };
		for (size_t index = 0; index < data.Shapes.size(); ++index) {
			const auto &shape = data.Shapes[index];
			if (shape.Identity.empty() || shape.Identity.size() > Limits::MaximumTextBytes ||
				std::any_of(data.Shapes.begin(), data.Shapes.begin() + index, [&](const auto &other) {
					return other.Identity == shape.Identity;
				}))
				return false;
			if (!(shape.Shape >= 100 && shape.Shape <= 103) && !(shape.Shape >= 200 && shape.Shape <= 205) &&
				!(shape.Shape >= 300 && shape.Shape <= 307) && !(shape.Shape >= 400 && shape.Shape <= 401))
				return false;
			if (!vector(shape.Size) || !vector(shape.Elongate) || !vector(shape.WaveAmplitude) ||
				!vector(shape.WaveIntensity) || !vector(shape.WavePhase) || !vector(shape.Position) ||
				!vector(shape.Rotation) || !vector(shape.TileDistance) || !vector(shape.TileAmount))
				return false;
			for (double scalar :
				 {shape.Radius,		  shape.Thickness,	   shape.Crop,			shape.Angle,
				  shape.Height,		  shape.RadiusRange.X, shape.RadiusRange.Y, shape.UniformSize,
				  shape.Rounded,	  shape.Corner.X,	   shape.Corner.Y,		shape.Corner.Z,
				  shape.Corner.W,	  shape.Size2D.X,	   shape.Size2D.Y,		shape.TwistAmount,
				  shape.Scale,		  shape.Specular,	   shape.Reflective,	shape.Density,
				  shape.TextureScale, shape.Triplanar})
				if (!number(scalar)) return false;
			if (shape.TwistAxis < 0 || shape.TwistAxis > 2 || shape.Sides <= 0 || shape.Scale == 0)
				return false;
			if (shape.Tile &&
				(shape.TileDistance.X == 0 || shape.TileDistance.Y == 0 || shape.TileDistance.Z == 0 ||
				 shape.TileAmount.X < 0 || shape.TileAmount.Y < 0 || shape.TileAmount.Z < 0))
				return false;
			if (shape.Texture &&
				(!ValidSurfaceLayout(*shape.Texture, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
				 !FiniteSurfaceSamples(*shape.Texture)))
				return false;
		}
		size_t stack = 0;
		for (const auto &operation : data.Operations) {
			if (!number(operation.Merge)) return false;
			if (operation.Code >= 0 && operation.Code < int32_t(data.Shapes.size()))
				++stack;
			else if (operation.Code >= 100 && operation.Code <= 103 && stack >= 2)
				--stack;
			else
				return false;
		}
		return data.Shapes.empty() ? data.Operations.empty() : stack == 1;
	}
}
