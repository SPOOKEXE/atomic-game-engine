#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <initializer_list>

namespace engine::imagegraph {
	namespace {
		graphcore::NodeSchema Declare(
			std::string_view kind,
			std::initializer_list<std::string_view> inputs,
			std::initializer_list<std::string_view> optional = {}
		) {
			graphcore::NodeSchema schema;
			schema.Id = kind;
			for (const auto input : inputs)
				schema.Ports.push_back({
					.Id = std::string(input),
					.ValueType = std::string(IMAGE_PORT_TYPE),
					.Direction = graphcore::PortDirection::Input,
				});
			for (const auto input : optional)
				schema.Ports.push_back({
					.Id = std::string(input),
					.ValueType = std::string(IMAGE_PORT_TYPE),
					.Direction = graphcore::PortDirection::Input,
					.Optional = true,
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
			Declare(NODE_KIND_IDS[6], {"Background", "Foreground"}, {"Mask"}),
			Declare(NODE_KIND_IDS[7], {}, {"Vector"}),
			Declare(NODE_KIND_IDS[8], {}, {"Vector"}),
			Declare(NODE_KIND_IDS[9], {}, {"Vector"}),
			Declare(NODE_KIND_IDS[10], {}, {"Vector"}),
			Declare(NODE_KIND_IDS[11], {"Image"}),
			Declare(NODE_KIND_IDS[12], {"Image"}),
			Declare(NODE_KIND_IDS[13], {"A"}, {"B"}),
			Declare(NODE_KIND_IDS[14], {"First"}, {"Second", "Third", "Fourth"}),
			Declare(NODE_KIND_IDS[15], {"Image"}),
			Declare(NODE_KIND_IDS[16], {"Image"}),
			Declare(NODE_KIND_IDS[17], {"Height"}),
			Declare(NODE_KIND_IDS[18], {"Image", "Vector"}),
			Declare(NODE_KIND_IDS[19], {}),
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

	bool InputRange(std::string_view kind, size_t &minimum, size_t &maximum) {
		const auto *schema = FindNodeSchema(kind);
		if (schema == nullptr) return false;
		minimum = maximum = 0;
		for (const auto &port : schema->Ports) {
			if (port.Direction != graphcore::PortDirection::Input) continue;
			++maximum;
			if (!port.Optional) ++minimum;
		}
		return true;
	}
}
