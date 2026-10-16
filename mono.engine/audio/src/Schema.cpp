#include "Schema.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>

namespace engine::audio::detail {

	namespace {
		graphcore::NodeSchema MakeSchema(std::string_view id, bool hasOutput) {
			graphcore::NodeSchema schema;
			schema.Id = id;
			// Player accepts incoming routing today, though its mixer voice
			// produces its own samples and ignores that input. Keep it routable.
			schema.Ports.push_back({
				.Id = "input",
				.ValueType = "audio.block",
				.Direction = graphcore::PortDirection::Input,
				.MaximumConnections = 0,
				.Optional = true,
			});
			if (hasOutput) {
				schema.Ports.push_back({
					.Id = "output",
					.ValueType = "audio.block",
					.Direction = graphcore::PortDirection::Output,
				});
			}
			return schema;
		}

		const std::array<graphcore::NodeSchema, 5> &Schemas() {
			static const std::array<graphcore::NodeSchema, 5> schemas = [] {
				std::array<graphcore::NodeSchema, 5> declared{
					MakeSchema("player", true),
					MakeSchema("fader", true),
					MakeSchema("emitter", true),
					MakeSchema("bus", true),
					MakeSchema("output", false),
				};
				const graphcore::Limits limits{
					.MaximumSchemas = declared.size(),
					.MaximumPorts = 9,
					.MaximumIdentityBytes = 256,
				};
				graphcore::Diagnostic diagnostic;
				if (graphcore::ValidateSchemas(declared, limits, diagnostic) != graphcore::CompileStatus::Ok)
					std::terminate();
				return declared;
			}();
			return schemas;
		}

		const graphcore::PortSchema *
		PortFor(const graphcore::NodeSchema *schema, graphcore::PortDirection direction) {
			if (schema == nullptr) return nullptr;
			const auto found = std::find_if(
				schema->Ports.begin(), schema->Ports.end(), [direction](const graphcore::PortSchema &port) {
					return port.Direction == direction;
				}
			);
			return found == schema->Ports.end() ? nullptr : &*found;
		}
	}

	std::span<const graphcore::NodeSchema> NodeSchemas() {
		return Schemas();
	}

	const graphcore::NodeSchema *NodeSchemaFor(NodeKind kind) {
		const auto &schemas = Schemas();
		switch (kind) {
		case NodeKind::Player:
			return &schemas[0];
		case NodeKind::Fader:
			return &schemas[1];
		case NodeKind::Emitter:
			return &schemas[2];
		case NodeKind::Bus:
			return &schemas[3];
		case NodeKind::Output:
			return &schemas[4];
		}
		return nullptr;
	}

	RoutingPorts RouteFor(NodeKind from, NodeKind to) {
		const graphcore::PortSchema *output = PortFor(NodeSchemaFor(from), graphcore::PortDirection::Output);
		const graphcore::PortSchema *input = PortFor(NodeSchemaFor(to), graphcore::PortDirection::Input);
		if (output == nullptr || input == nullptr ||
			graphcore::ComparePortTypes(output->ValueType, input->ValueType) ==
				graphcore::TypeCompatibility::Incompatible)
			return {};
		return {.Output = output, .Input = input};
	}
}
