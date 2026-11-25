#include <engine/ang/Registry.hpp>
#include <engine/graphcore/Schema.hpp>

#include <unordered_set>

namespace engine::ang {

	const char *DomainName(Domain domain) {
		switch (domain) {
		case Domain::Tick:
			return "Tick";
		case Domain::Frame:
			return "Frame";
		case Domain::Tool:
			return "Tool";
		}
		return "Tool";
	}

	std::optional<Domain> DomainFromName(std::string_view name) {
		if (name == "Tick") {
			return Domain::Tick;
		}
		if (name == "Frame") {
			return Domain::Frame;
		}
		if (name == "Tool") {
			return Domain::Tool;
		}
		return std::nullopt;
	}

	TypeId NormaliseType(std::string_view text) {
		if (text == "data.ANY") {
			return TypeId(ANY_TYPE);
		}
		return TypeId(text);
	}

	namespace {
		template <typename T> const T *FindNamed(const std::vector<T> &list, std::string_view name) {
			for (const auto &entry : list) {
				if (entry.Name == name) {
					return &entry;
				}
			}
			return nullptr;
		}
	}

	const PortDef *NodeDef::FindInput(std::string_view name) const {
		return FindNamed(Inputs, name);
	}

	const PortDef *NodeDef::FindOutput(std::string_view name) const {
		return FindNamed(Outputs, name);
	}

	const ParamDef *NodeDef::FindParam(std::string_view name) const {
		return FindNamed(Params, name);
	}

	Registry::Registry() {
		DefineType({.Id = TypeId(ANY_TYPE), .Label = "Any"});
	}

	Registry Registry::WithScalars() {
		Registry registry;
		const auto toNumber = [](const Value &value) { return Value(value.AsNumber()); };
		registry.DefineType({.Id = "bool", .Label = "Bool", .Wire = {0.9f, 0.3f, 0.3f}});
		registry.DefineType(
			{.Id = "int",
			 .Label = "Integer",
			 .Wire = {0.3f, 0.9f, 0.6f},
			 .ConvertsTo = {{"number", toNumber}}}
		);
		registry.DefineType({.Id = "number", .Label = "Number", .Wire = {0.6f, 0.9f, 0.3f}});
		registry.DefineType({.Id = "vector2", .Label = "Vector2", .Wire = {0.9f, 0.8f, 0.2f}});
		registry.DefineType({.Id = "vector3", .Label = "Vector3", .Wire = {0.9f, 0.7f, 0.2f}});
		registry.DefineType({.Id = "vector4", .Label = "Vector4", .Wire = {0.9f, 0.6f, 0.2f}});
		registry.DefineType({.Id = "color3", .Label = "Color3", .Wire = {0.9f, 0.4f, 0.9f}});
		registry.DefineType({.Id = "string", .Label = "String", .Wire = {0.9f, 0.5f, 0.7f}});
		registry.DefineType({.Id = "name", .Label = "Name", .Wire = {0.7f, 0.7f, 0.9f}});
		return registry;
	}

	RegistryResult Registry::DefineType(DataType type) {
		type.Id = NormaliseType(type.Id);
		if (type.Id.empty() || type.Id.find('\0') != std::string::npos) {
			return {false, "type id is empty"};
		}
		if (TypeIndex.contains(type.Id)) {
			return {false, "type '" + type.Id + "' is already defined"};
		}
		if (type.Label.empty()) {
			type.Label = type.Id;
		}
		TypeIndex.emplace(type.Id, TypeList.size());
		TypeList.push_back(std::move(type));
		return {};
	}

	namespace {
		RegistryResult
		CheckPorts(const Registry &registry, const std::vector<PortDef> &ports, const char *side) {
			std::unordered_set<std::string_view> seen;
			for (const auto &port : ports) {
				if (port.Name.empty()) {
					return {false, std::string(side) + " port name is empty"};
				}
				if (!seen.insert(port.Name).second) {
					return {false, std::string(side) + " port '" + port.Name + "' is repeated"};
				}
				if (!port.Generic.empty()) {
					for (const auto &constraint : port.Constraints) {
						if (registry.FindType(constraint) == nullptr) {
							return {
								false,
								"port '" + port.Name + "' constrains to unknown type '" + constraint + "'"
							};
						}
					}
					continue;
				}
				if (registry.FindType(port.Type) == nullptr) {
					return {false, "port '" + port.Name + "' names unknown type '" + port.Type + "'"};
				}
			}
			return {};
		}

		void NormalisePorts(std::vector<PortDef> &ports, Direction direction) {
			for (auto &port : ports) {
				port.Type = NormaliseType(port.Type);
				port.Dir = direction;
			}
		}
	}

	RegistryResult Registry::DefineNode(NodeDef node) {
		if (node.Id.empty()) {
			return {false, "node id is empty"};
		}
		if (NodeIndex.contains(node.Id) || SubgraphIndex.contains(node.Id)) {
			return {false, "node '" + node.Id + "' is already defined"};
		}
		if (node.Version == 0) {
			return {false, "node '" + node.Id + "' version must start at 1"};
		}
		NormalisePorts(node.Inputs, Direction::Input);
		NormalisePorts(node.Outputs, Direction::Output);
		if (auto result = CheckPorts(*this, node.Inputs, "input"); !result.Ok) {
			return result;
		}
		if (auto result = CheckPorts(*this, node.Outputs, "output"); !result.Ok) {
			return result;
		}
		for (const auto &route : node.Bypass) {
			const PortDef *input = node.FindInput(route.Input);
			const PortDef *output = node.FindOutput(route.Output);
			if (input == nullptr || output == nullptr) {
				return {false, "bypass route names a missing port"};
			}
		}
		if (node.Label.empty()) {
			node.Label = node.Id;
		}
		NodeIndex.emplace(node.Id, NodeList.size());
		NodeList.push_back(std::make_shared<const NodeDef>(std::move(node)));
		return {};
	}

	RegistryResult Registry::DefineSubgraph(SubgraphDef subgraph) {
		if (subgraph.Id.empty()) {
			return {false, "subgraph id is empty"};
		}
		if (NodeIndex.contains(subgraph.Id) || SubgraphIndex.contains(subgraph.Id)) {
			return {false, "subgraph '" + subgraph.Id + "' is already defined"};
		}
		if (subgraph.Body == nullptr) {
			return {false, "subgraph '" + subgraph.Id + "' has no body"};
		}
		for (auto &port : subgraph.Inputs) {
			port.Port.Type = NormaliseType(port.Port.Type);
			port.Port.Dir = Direction::Input;
		}
		for (auto &port : subgraph.Outputs) {
			port.Port.Type = NormaliseType(port.Port.Type);
			port.Port.Dir = Direction::Output;
		}
		SubgraphIndex.emplace(subgraph.Id, SubgraphList.size());
		SubgraphList.push_back(std::move(subgraph));
		return {};
	}

	const DataType *Registry::FindType(std::string_view id) const {
		const auto found = TypeIndex.find(std::string(id));
		return found == TypeIndex.end() ? nullptr : &TypeList[found->second];
	}

	std::shared_ptr<const NodeDef> Registry::FindNode(std::string_view id) const {
		const auto found = NodeIndex.find(std::string(id));
		return found == NodeIndex.end() ? nullptr : NodeList[found->second];
	}

	const SubgraphDef *Registry::FindSubgraph(std::string_view id) const {
		const auto found = SubgraphIndex.find(std::string(id));
		return found == SubgraphIndex.end() ? nullptr : &SubgraphList[found->second];
	}

	LinkCheck Registry::CheckLink(std::string_view from, std::string_view to) const {
		const DataType *source = FindType(from);
		if (source == nullptr || FindType(to) == nullptr) {
			return {};
		}
		if (graphcore::ComparePortTypes(from, to) != graphcore::TypeCompatibility::Incompatible) {
			return {.Ok = true};
		}
		for (const auto &conversion : source->ConvertsTo) {
			if (conversion.To == to) {
				return {.Ok = true, .Via = &conversion};
			}
		}
		return {};
	}

	size_t Registry::BytesOf(const Value &value) const {
		const DataType *type = FindType(value.TypeName());
		if (type != nullptr && type->Bytes) {
			return type->Bytes(value);
		}
		return value.Bytes();
	}
}
