#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <initializer_list>

namespace engine::imagegraph {
	namespace {
		graphcore::NodeSchema Declare(std::string_view kind, std::initializer_list<std::string_view> inputs) {
			graphcore::NodeSchema schema;
			schema.Id = kind;
			for (const auto input : inputs)
				schema.Ports.push_back({
					.Id = std::string(input),
					.ValueType = std::string(IMAGE_PORT_TYPE),
					.Direction = graphcore::PortDirection::Input,
				});
			schema.Ports.push_back(
				{.Id = "Image",
				 .ValueType = std::string(IMAGE_PORT_TYPE),
				 .Direction = graphcore::PortDirection::Output}
			);
			return schema;
		}
	}

	std::span<const graphcore::NodeSchema> NodeSchemas() {
		static_assert(std::variant_size_v<Operation> == NODE_KIND_IDS.size());
		static const std::array schemas{
			Declare(NODE_KIND_IDS[0], {}),
			Declare(NODE_KIND_IDS[1], {}),
			Declare(NODE_KIND_IDS[2], {"Image"}),
			Declare(NODE_KIND_IDS[3], {"Image"}),
			Declare(NODE_KIND_IDS[4], {"Image"}),
			Declare(NODE_KIND_IDS[5], {"Image"}),
			Declare(NODE_KIND_IDS[6], {"Background", "Foreground"}),
		};
		return schemas;
	}

	const graphcore::NodeSchema *FindNodeSchema(std::string_view kind) {
		const auto schemas = NodeSchemas();
		const auto found = std::find_if(schemas.begin(), schemas.end(), [&](const auto &schema) {
			return schema.Id == kind;
		});
		return found == schemas.end() ? nullptr : &*found;
	}
}
