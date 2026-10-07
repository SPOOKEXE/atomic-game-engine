#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/NoiseField.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace engine::imagegraph {
	ValueType NoiseFieldType(const NoiseFieldValue &field) {
		const auto dimension = field.Data ? field.Data->Dimensions : 0;
		return dimension == 3 ? ValueType::Noise3D : dimension == 2 ? ValueType::Noise2D : ValueType::Noise1D;
	}
	namespace {
		bool ValidNoiseLayout(const NoiseFieldValue &field) {
			if (!field.Data) return false;
			const auto &data = *field.Data;
			if (data.Dimensions < 1 || data.Dimensions > 3 || data.Octaves < 1 || data.Octaves > 16 ||
				!std::isfinite(data.Frequency) || data.Frequency <= 0 || data.Frequency > 8192 ||
				!std::isfinite(data.Gain) || data.Gain < 0 || data.Gain > 1)
				return false;
			return !data.Raster ||
				   (data.Dimensions == 2 &&
					ValidSurfaceLayout(*data.Raster, Limits::MaximumDimension, Limits::MaximumArrayBytes));
		}
	}
	bool ValidNoiseField(const NoiseFieldValue &field) {
		return ValidNoiseLayout(field) && (!field.Data->Raster || FiniteSurfaceSamples(*field.Data->Raster));
	}
	std::optional<ValueType> NoiseGeneratorOutputType(const Node &node) {
		if (node.Type != "value.noise_field") return std::nullopt;
		int64_t dimension = 2;
		for (const auto &property : node.Values)
			if (property.Port == "dimension") {
				const auto *choice = std::get_if<EnumValue>(&property.Data);
				if (!choice) return std::nullopt;
				dimension = choice->Value;
			}
		if (dimension < 1 || dimension > 3) return std::nullopt;
		return dimension == 1 ? ValueType::Noise1D : dimension == 2 ? ValueType::Noise2D : ValueType::Noise3D;
	}
	bool IsNoiseImageGenerator(std::string_view type) {
		if (type == "image.noise_simplex") return true;
		const auto *entry = FindCatalogueEntry(type);
		return entry && entry->Family == "generate" && type.find("noise") != std::string_view::npos;
	}
	namespace {
		uint64_t Mix(uint64_t value) {
			value ^= value >> 30;
			value *= 0xbf58476d1ce4e5b9ull;
			value ^= value >> 27;
			value *= 0x94d049bb133111ebull;
			return value ^ (value >> 31);
		}
		bool SampleRecipe(const NoiseFieldData &field, std::span<const double> coordinates, double &value) {
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
					uint64_t hash = Mix(static_cast<uint64_t>(field.Seed) + static_cast<uint64_t>(octave));
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
	bool SampleNoiseField(const NoiseFieldValue &field, std::span<const double> coordinates, double &value) {
		if (!ValidNoiseLayout(field) || coordinates.size() != field.Data->Dimensions) return false;
		for (double coordinate : coordinates)
			if (!std::isfinite(coordinate)) return false;
		if (!field.Data->Raster) return SampleRecipe(*field.Data, coordinates, value);
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
		value = pixel[0];
		return std::isfinite(value);
	}
}
