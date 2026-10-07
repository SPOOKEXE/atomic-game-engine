#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// grug coordinate dimensions and result components are separate recipe choices.
	ValueType NoiseFieldType(const NoiseFieldValue &field);
	std::optional<ValueType> NoiseFieldType(uint8_t dimensions, uint8_t components);
	bool IsNoiseFieldType(ValueType type);
	struct NoiseNodeChoices {
		uint8_t Mode = 0, Dimensions = 2, Components = 1;
	};
	bool ResolveNoiseNodeChoices(const Node &node, NoiseNodeChoices &choices, std::string_view &failedPort);
	std::optional<PortSchema> NoiseNodePort(
		const Node &node, std::string_view id, PortDirection side, const Document *document = nullptr
	);
	uint8_t RasterNoiseComponents(const Node &node, const Document *document = nullptr);
	bool ValidNoiseRecipe(const NoiseFieldData &recipe);
	bool SampleNoiseRecipe(
		const NoiseFieldData &recipe, std::span<const double> coordinates, std::span<double> values
	);
	bool SampleNoiseField(
		const NoiseFieldValue &field, std::span<const double> coordinates, std::span<double> values
	);
	bool ValidNoiseField(const NoiseFieldValue &field);
	// grug finite bounded coordinates sample owned pixels or a native smooth value-noise recipe.
	bool SampleNoiseField(const NoiseFieldValue &field, std::span<const double> coordinates, double &value);
	// grug raster fields keep two coordinates and select red, RG, or RGB results.
	std::optional<ValueType> NoiseGeneratorOutputType(const Node &node);
	bool IsNoiseImageGenerator(std::string_view type);
}
