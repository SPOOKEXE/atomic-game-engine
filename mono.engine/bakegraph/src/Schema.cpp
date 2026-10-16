#include <engine/bakegraph/Schema.hpp>

#include <array>
#include <string>
#include <string_view>

namespace engine::bake {

	namespace {
		graphcore::NodeSchema
		MakeSchema(std::string_view id, std::string_view inputType, std::string_view outputType) {
			graphcore::NodeSchema schema;
			schema.Id = id;
			if (!inputType.empty()) {
				schema.Ports.push_back({
					.Id = "input",
					.ValueType = std::string(inputType),
					.Direction = graphcore::PortDirection::Input,
					.MaximumConnections = 1,
					// Graph::Evaluate reports a missing input with its established error.
					.Optional = true,
				});
			}
			if (!outputType.empty()) {
				schema.Ports.push_back({
					.Id = "output",
					.ValueType = std::string(outputType),
					.Direction = graphcore::PortDirection::Output,
					.MaximumConnections = 0,
					.Optional = false,
				});
			}
			return schema;
		}

		const std::array<graphcore::NodeSchema, 14> &Schemas() {
			static const std::array<graphcore::NodeSchema, 14> schemas{
				MakeSchema("source", "", "bake.bytes"),
				MakeSchema("builtin", "", "bake.mesh"),
				MakeSchema("import", "bake.bytes", "any"),
				MakeSchema("fit", "bake.mesh", "bake.mesh"),
				MakeSchema("scale", "bake.mesh", "bake.mesh"),
				MakeSchema("smooth", "bake.mesh", "bake.mesh"),
				MakeSchema("resize", "bake.texture", "bake.texture"),
				MakeSchema("opaque", "bake.texture", "bake.texture"),
				MakeSchema("mipmap", "bake.texture", "bake.texture"),
				MakeSchema("retime", "bake.texture", "bake.texture"),
				MakeSchema("write", "any", "any"),
				MakeSchema("rasterize", "bake.bytes", "bake.texture"),
				MakeSchema("decimate", "bake.mesh", "bake.mesh"),
				MakeSchema("flipbook", "bake.texture", "bake.texture"),
			};
			return schemas;
		}
	}

	std::span<const graphcore::NodeSchema> NodeSchemas() {
		return Schemas();
	}

	const graphcore::NodeSchema *NodeSchemaFor(NodeKind kind) {
		const auto &schemas = Schemas();
		switch (kind) {
		case NodeKind::Source:
			return &schemas[0];
		case NodeKind::Builtin:
			return &schemas[1];
		case NodeKind::Import:
			return &schemas[2];
		case NodeKind::Fit:
			return &schemas[3];
		case NodeKind::Scale:
			return &schemas[4];
		case NodeKind::Smooth:
			return &schemas[5];
		case NodeKind::Resize:
			return &schemas[6];
		case NodeKind::Opaque:
			return &schemas[7];
		case NodeKind::Mipmap:
			return &schemas[8];
		case NodeKind::Retime:
			return &schemas[9];
		case NodeKind::Write:
			return &schemas[10];
		case NodeKind::Rasterize:
			return &schemas[11];
		case NodeKind::Decimate:
			return &schemas[12];
		case NodeKind::Flipbook:
			return &schemas[13];
		}
		return nullptr;
	}
}
