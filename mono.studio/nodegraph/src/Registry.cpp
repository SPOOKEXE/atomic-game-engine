// The node type table, and what an evaluation is handed.

#include <nodegraph/Registry.hpp>
#include <unordered_map>
#include <vector>

namespace nodegraph {

	namespace {
		struct NodeTable {
			std::vector<NodeType> Order;
			std::unordered_map<std::string, size_t> ById;
		};

		NodeTable &Registered() {
			static NodeTable table;
			return table;
		}
	}

	Value Inputs::Widget(const std::string &key) const {
		if (Widgets == nullptr) {
			return Value{};
		}
		const auto found = Widgets->find(key);
		return found == Widgets->end() ? Value{} : found->second;
	}

	double Inputs::Real(const std::string &key) const {
		return Widget(key).Number;
	}

	void NodeTypes::Register(const NodeType &type) {
		NodeTable &table = Registered();
		const auto found = table.ById.find(type.Id);
		if (found != table.ById.end()) {
			// **Replaced in place rather than appended.** A hot-reloaded node
			// type has to keep its position in the Library, and appending would
			// move it under a different heading every reload.
			table.Order[found->second] = type;
			return;
		}
		table.ById.emplace(type.Id, table.Order.size());
		table.Order.push_back(type);
	}

	const NodeType *NodeTypes::Find(const std::string &id) {
		const NodeTable &table = Registered();
		const auto found = table.ById.find(id);
		return found == table.ById.end() ? nullptr : &table.Order[found->second];
	}

	const std::vector<NodeType> &NodeTypes::All() {
		return Registered().Order;
	}

	std::vector<std::string> NodeTypes::Categories() {
		std::vector<std::string> names;
		for (const NodeType &type : Registered().Order) {
			if (std::find(names.begin(), names.end(), type.Category) == names.end()) {
				names.push_back(type.Category);
			}
		}
		return names;
	}

	std::vector<const NodeType *> NodeTypes::AcceptingInput(const std::string &type) {
		std::vector<const NodeType *> found;
		for (const NodeType &candidate : Registered().Order) {
			for (const PortSpec &port : candidate.Inputs) {
				if (DataTypes::CanConnect(type, port.Type)) {
					found.push_back(&candidate);
					break;
				}
			}
		}
		return found;
	}
}
