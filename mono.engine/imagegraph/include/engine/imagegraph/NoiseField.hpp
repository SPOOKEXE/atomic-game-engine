#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// grug dimension is coordinate count; each field returns one scalar.
	ValueType NoiseFieldType(const NoiseFieldValue &field);
	bool ValidNoiseField(const NoiseFieldValue &field);
	// grug finite bounded coordinates sample owned pixels or a native smooth value-noise recipe.
	bool SampleNoiseField(const NoiseFieldValue &field, std::span<const double> coordinates, double &value);
	// grug static dimension choice controls the generator's stable field socket.
	std::optional<ValueType> NoiseGeneratorOutputType(const Node &node);
	bool IsNoiseImageGenerator(std::string_view type);
}
