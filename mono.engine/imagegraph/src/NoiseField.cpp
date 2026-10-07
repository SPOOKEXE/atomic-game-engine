#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/NoiseField.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace engine::imagegraph {
	namespace {
		constexpr std::array<std::array<ValueType, 3>, 3> FIELD_TYPES{
			{{ValueType::Noise1D, ValueType::Noise2D, ValueType::Noise3D},
			 {ValueType::Noise1DVector2, ValueType::Noise2DVector2, ValueType::Noise3DVector2},
			 {ValueType::Noise1DVector3, ValueType::Noise2DVector3, ValueType::Noise3DVector3}}
		};
		constexpr std::array VALUE_TYPES{ValueType::Scalar, ValueType::Vector2, ValueType::Vector3};
		bool ValidRecipe(const NoiseFieldData &data) {
			return data.Dimensions >= 1 && data.Dimensions <= 3 && data.Components >= 1 &&
				   data.Components <= 3 && data.Octaves >= 1 && data.Octaves <= 16 &&
				   std::isfinite(data.Frequency) && data.Frequency > 0 && data.Frequency <= 8192 &&
				   std::isfinite(data.Gain) && data.Gain >= 0 && data.Gain <= 1;
		}
	}
	bool ValidNoiseRecipe(const NoiseFieldData &recipe) {
		return ValidRecipe(recipe) && !recipe.Raster;
	}
	std::optional<ValueType> NoiseFieldType(uint8_t dimensions, uint8_t components) {
		if (dimensions < 1 || dimensions > 3 || components < 1 || components > 3) return std::nullopt;
		return FIELD_TYPES[components - 1][dimensions - 1];
	}
	bool IsNoiseFieldType(ValueType type) {
		for (const auto &row : FIELD_TYPES)
			for (auto member : row)
				if (type == member) return true;
		return false;
	}
	ValueType NoiseFieldType(const NoiseFieldValue &field) {
		return field.Data ? NoiseFieldType(field.Data->Dimensions, field.Data->Components)
								.value_or(ValueType::Noise1D)
						  : ValueType::Noise1D;
	}
	bool ResolveNoiseNodeChoices(const Node &node, NoiseNodeChoices &choices, std::string_view &failedPort) {
		choices = {};
		failedPort = {};
		for (const auto &property : node.Values) {
			uint8_t *target = property.Port == "output_type" ? &choices.Components
							  : node.Type == "value.noise_field" && property.Port == "mode" ? &choices.Mode
							  : node.Type == "value.noise_field" && property.Port == "dimension"
								  ? &choices.Dimensions
								  : nullptr;
			if (!target) continue;
			const auto *value = std::get_if<EnumValue>(&property.Data);
			const int64_t minimum = target == &choices.Mode ? 0 : 1,
						  maximum = target == &choices.Mode ? 1 : 3;
			if (!value || value->Value < minimum || value->Value > maximum) {
				failedPort = property.Port;
				return false;
			}
			*target = uint8_t(value->Value);
		}
		return node.Type == "value.noise_field" || node.Type == "value.sample_noise";
	}
	std::optional<PortSchema> NoiseNodePort(const Node &node, std::string_view id, PortDirection side) {
		NoiseNodeChoices choices;
		std::string_view failed;
		if (!ResolveNoiseNodeChoices(node, choices, failed)) return std::nullopt;
		if (node.Type == "value.noise_field") {
			if (side == PortDirection::Output) {
				if (choices.Mode == 0 && id == "field")
					return PortSchema{id, *NoiseFieldType(choices.Dimensions, choices.Components), side};
				if (choices.Mode == 1 && id == "value")
					return PortSchema{id, VALUE_TYPES[choices.Components - 1], side};
			} else {
				if (id == "position" && choices.Mode == 1)
					return PortSchema{id, VALUE_TYPES[choices.Dimensions - 1], side};
				if (id == "seed" || id == "octaves") return PortSchema{id, ValueType::Integer, side};
				if (id == "frequency" || id == "gain") return PortSchema{id, ValueType::Scalar, side};
			}
		} else {
			if (side == PortDirection::Output && id == "value")
				return PortSchema{id, VALUE_TYPES[choices.Components - 1], side};
			if (side == PortDirection::Input && id == "field")
				return PortSchema{
					id, FIELD_TYPES[choices.Components - 1][1], side, FIELD_TYPES[choices.Components - 1]
				};
			if (side == PortDirection::Input && id == "position")
				return PortSchema{id, ValueType::Scalar, side, VALUE_TYPES};
		}
		return std::nullopt;
	}

	namespace {
		bool ValidNoiseLayout(const NoiseFieldValue &field) {
			if (!field.Data) return false;
			const auto &data = *field.Data;
			if (!ValidRecipe(data)) return false;
			return !data.Raster ||
				   (data.Dimensions == 2 && data.Components == 1 &&
					ValidSurfaceLayout(*data.Raster, Limits::MaximumDimension, Limits::MaximumArrayBytes));
		}
	}
	bool ValidNoiseField(const NoiseFieldValue &field) {
		return ValidNoiseLayout(field) && (!field.Data->Raster || FiniteSurfaceSamples(*field.Data->Raster));
	}
	std::optional<ValueType> NoiseGeneratorOutputType(const Node &node) {
		const auto port = NoiseNodePort(node, "field", PortDirection::Output);
		return port ? std::optional(port->Type) : std::nullopt;
	}

	bool IsNoiseImageGenerator(std::string_view type) {
		if (type == "image.noise_simplex") return true;
		const auto *entry = FindCatalogueEntry(type);
		return entry && std::any_of(entry->Outputs.begin(), entry->Outputs.end(), [](const auto &output) {
				   return output.Id == "field" && output.Type == ValueType::Noise2D;
			   });
	}
	namespace {
		uint64_t Mix(uint64_t value) {
			value ^= value >> 30;
			value *= 0xbf58476d1ce4e5b9ull;
			value ^= value >> 27;
			value *= 0x94d049bb133111ebull;
			return value ^ (value >> 31);
		}
		bool SampleRecipe(
			const NoiseFieldData &field, std::span<const double> coordinates, double &value, uint64_t seed
		) {
			std::array<double, 3> point{};
			for (size_t axis = 0; axis < coordinates.size(); ++axis)
				point[axis] = coordinates[axis] * field.Frequency;
			double amplitude = 1, total = 0, weight = 0;
			for (int64_t octave = 0; octave < field.Octaves; ++octave) {
				std::array<int64_t, 3> cell{};
				std::array<double, 3> blend{};
				for (size_t axis = 0; axis < coordinates.size(); ++axis) {
					if (!std::isfinite(point[axis]) || std::abs(point[axis]) > 0x1p30) return false;
					cell[axis] = static_cast<int64_t>(std::floor(point[axis]));
					const double t = point[axis] - double(cell[axis]);
					blend[axis] = t * t * t * (t * (t * 6 - 15) + 10);
				}
				double sample = 0;
				for (unsigned corner = 0; corner < (1u << field.Dimensions); ++corner) {
					uint64_t hash = Mix(seed + static_cast<uint64_t>(octave));
					double influence = 1;
					for (size_t axis = 0; axis < coordinates.size(); ++axis) {
						const bool upper = (corner & (1u << axis)) != 0;
						hash = Mix(hash ^ Mix(static_cast<uint64_t>(cell[axis] + upper) + axis));
						influence *= upper ? blend[axis] : 1 - blend[axis];
					}
					sample += double(hash >> 11) * 0x1p-53 * influence;
				}
				total += sample * amplitude;
				weight += amplitude;
				amplitude *= field.Gain;
				if (amplitude == 0) break;
				for (auto &axis : point)
					axis *= 2;
			}
			value = std::clamp(total / weight, 0.0, 1.0);
			return std::isfinite(value);
		}
	}
	bool SampleNoiseRecipe(
		const NoiseFieldData &recipe, std::span<const double> coordinates, std::span<double> values
	) {
		if (!ValidRecipe(recipe) || recipe.Raster || coordinates.size() != recipe.Dimensions ||
			values.size() != recipe.Components)
			return false;
		for (double coordinate : coordinates)
			if (!std::isfinite(coordinate)) return false;
		for (size_t channel = 0; channel < values.size(); ++channel) {
			const uint64_t seed = channel == 0
									  ? uint64_t(recipe.Seed)
									  : Mix(uint64_t(recipe.Seed) ^ (0x9e3779b97f4a7c15ull * channel));
			if (!SampleRecipe(recipe, coordinates, values[channel], seed)) return false;
		}
		return true;
	}
	bool SampleNoiseField(
		const NoiseFieldValue &field, std::span<const double> coordinates, std::span<double> values
	) {
		if (!ValidNoiseLayout(field) || coordinates.size() != field.Data->Dimensions ||
			values.size() != field.Data->Components)
			return false;
		for (double coordinate : coordinates)
			if (!std::isfinite(coordinate)) return false;
		if (!field.Data->Raster) return SampleNoiseRecipe(*field.Data, coordinates, values);
		const auto &image = *field.Data->Raster;
		const auto axis = [](double coordinate, uint32_t count) {
			return static_cast<uint32_t>(
				std::min(std::floor(std::clamp(coordinate, 0.0, 1.0) * count), double(count - 1))
			);
		};
		SurfacePixel pixel;
		if (!LoadSurfacePixel(
				image, axis(coordinates[0], image.Width), axis(coordinates[1], image.Height), pixel
			))
			return false;
		values[0] = pixel[0];
		return std::isfinite(values[0]);
	}
	bool SampleNoiseField(const NoiseFieldValue &field, std::span<const double> coordinates, double &value) {
		return field.Data && field.Data->Components == 1 &&
			   SampleNoiseField(field, coordinates, std::span(&value, 1));
	}
}
