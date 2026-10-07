#pragma once
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/NoiseField.hpp>

#include <algorithm>
#include <vector>

namespace studio::detail {
	// Outputs are an instance interface. Changing it never mutates the shared type registry.
	inline std::vector<engine::imagegraph::PortSchema> ImageGraphOutputPorts(
		const engine::imagegraph::Node &node, const engine::imagegraph::Document *document = nullptr
	) {
		using namespace engine::imagegraph;
		std::vector<PortSchema> ports;
		const auto *schema = FindSchema(node.Type);
		if (!schema) return ports;
		if (node.Type == "pc.array_split") {
			const auto count = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &value) {
				return value.Port == "attribute_output_amount";
			});
			if (count != node.Values.end() && std::get_if<double>(&count->Data) &&
				std::get<double>(count->Data) == 0)
				return ports;
		}
		bool channelArray = false;
		if (node.Type == "pc.color_to_rgb" || node.Type == "pc.color_to_hsv") {
			const auto choice = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &value) {
				return value.Port == "output_array";
			});
			channelArray = choice != node.Values.end() && std::get_if<bool>(&choice->Data) &&
						   std::get<bool>(choice->Data);
		}
		for (const auto &port : schema->Ports) {
			if (port.Direction != PortDirection::Output) continue;
			if (channelArray && !ports.empty()) continue;
			if (node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
				(IsNoiseImageGenerator(node.Type) && port.Id == "field")) {
				const auto instance = NoiseNodePort(node, port.Id, PortDirection::Output, document);
				if (instance) ports.push_back(*instance);
			} else {
				ports.push_back(
					{port.Id, channelArray ? ValueType::Array : port.Type, PortDirection::Output}
				);
			}
		}
		for (const auto &port : node.DynamicOutputs)
			ports.push_back({port.Id, port.Type, PortDirection::Output});
		return ports;
	}
}
