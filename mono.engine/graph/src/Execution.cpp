#include <engine/graph/Execution.hpp>

#include <algorithm>
#include <deque>
#include <sstream>

namespace engine::graph {

	namespace {
		const NodePortSchema *
		FindPort(const NodeSchema &schema, std::string_view id, NodePortDirection direction) {
			const auto found =
				std::find_if(schema.Ports.begin(), schema.Ports.end(), [&](const NodePortSchema &port) {
					return port.Id == id && port.Direction == direction;
				});
			return found == schema.Ports.end() ? nullptr : &*found;
		}

		bool Compatible(std::string_view from, std::string_view to) {
			return ArePortTypesCompatible(from, to);
		}

		bool ValueMatches(const NodeValue &value, std::string_view type) {
			return type == VALUE_TYPE_ANY ? !ValueTypeOf(value).empty() : ValueTypeOf(value) == type;
		}

		std::string Mismatch(std::string_view port, std::string_view expected, const NodeValue &actual) {
			std::ostringstream message;
			message << "port '" << port << "' expects " << expected << " but received "
					<< ValueTypeOf(actual);
			return message.str();
		}
	}

	const char *Describe(NodeExecutionState state) {
		switch (state) {
		case NodeExecutionState::Idle:
			return "idle";
		case NodeExecutionState::Queued:
			return "queued";
		case NodeExecutionState::Running:
			return "running";
		case NodeExecutionState::Complete:
			return "complete";
		case NodeExecutionState::Error:
			return "error";
		}
		return "unknown node execution state";
	}

	const char *Describe(NodeExecutionStatus status) {
		switch (status) {
		case NodeExecutionStatus::Ok:
			return "ok";
		case NodeExecutionStatus::InvalidNode:
			return "invalid node";
		case NodeExecutionStatus::DuplicateNode:
			return "duplicate node";
		case NodeExecutionStatus::UnknownSchema:
			return "unknown node schema";
		case NodeExecutionStatus::InvalidConnection:
			return "invalid node connection";
		case NodeExecutionStatus::TypeMismatch:
			return "node connection type mismatch";
		case NodeExecutionStatus::Cycle:
			return "node graph has a cycle without a delay or feedback boundary";
		case NodeExecutionStatus::Pending:
			return "node execution pending";
		case NodeExecutionStatus::StaleCompletion:
			return "node execution completion is stale";
		case NodeExecutionStatus::EvaluationFailed:
			return "node evaluation failed";
		}
		return "unknown node execution status";
	}

	NodeExecutionStatus NodeRuntime::AddNode(NodeExecutionNode node) {
		if (!node.Id.IsValid()) {
			return NodeExecutionStatus::InvalidNode;
		}
		if (NodeIndexes.contains(node.Id)) {
			return NodeExecutionStatus::DuplicateNode;
		}
		if (Schemas.Find(node.Schema.Text()) == nullptr) {
			return NodeExecutionStatus::UnknownSchema;
		}
		NodeIndexes.emplace(node.Id, Nodes.size());
		Nodes.push_back(std::move(node));
		Records.emplace_back();
		return NodeExecutionStatus::Ok;
	}

	NodeExecutionStatus NodeRuntime::ValidateConnection(const NodeExecutionConnection &connection) const {
		const NodeExecutionNode *fromNode = FindNode(connection.FromNode);
		const NodeExecutionNode *toNode = FindNode(connection.ToNode);
		if (fromNode == nullptr || toNode == nullptr || connection.FromPort.empty() ||
			connection.ToPort.empty()) {
			return NodeExecutionStatus::InvalidConnection;
		}
		const NodeSchema *fromSchema = Schemas.Find(fromNode->Schema.Text());
		const NodeSchema *toSchema = Schemas.Find(toNode->Schema.Text());
		if (fromSchema == nullptr || toSchema == nullptr) {
			return NodeExecutionStatus::UnknownSchema;
		}
		const NodePortSchema *fromPort =
			FindPort(*fromSchema, connection.FromPort, NodePortDirection::Output);
		const NodePortSchema *toPort = FindPort(*toSchema, connection.ToPort, NodePortDirection::Input);
		if (fromPort == nullptr || toPort == nullptr) {
			return NodeExecutionStatus::InvalidConnection;
		}
		return Compatible(fromPort->ValueType, toPort->ValueType) ? NodeExecutionStatus::Ok
																  : NodeExecutionStatus::TypeMismatch;
	}

	NodeExecutionStatus NodeRuntime::Connect(NodeExecutionConnection connection) {
		const NodeExecutionStatus status = ValidateConnection(connection);
		if (status != NodeExecutionStatus::Ok) {
			return status;
		}
		const NodeExecutionNode *toNode = FindNode(connection.ToNode);
		const NodeSchema *toSchema = Schemas.Find(toNode->Schema.Text());
		const NodePortSchema *toPort = FindPort(*toSchema, connection.ToPort, NodePortDirection::Input);
		const size_t existing =
			std::count_if(Wires.begin(), Wires.end(), [&](const NodeExecutionConnection &wire) {
				return wire.ToNode == connection.ToNode && wire.ToPort == connection.ToPort;
			});
		if (toPort->MaxConnections != 0 && existing >= toPort->MaxConnections) {
			return NodeExecutionStatus::InvalidConnection;
		}
		Wires.push_back(std::move(connection));
		return MarkDirty(Wires.back().ToNode);
	}

	NodeExecutionStatus NodeRuntime::SetInput(core::Name node, std::string_view port, NodeValue input) {
		const NodeExecutionNode *instance = FindNode(node);
		if (instance == nullptr) {
			return NodeExecutionStatus::InvalidNode;
		}
		const NodeSchema *schema = Schemas.Find(instance->Schema.Text());
		const NodePortSchema *inputPort =
			schema == nullptr ? nullptr : FindPort(*schema, port, NodePortDirection::Input);
		if (inputPort == nullptr) {
			return NodeExecutionStatus::InvalidConnection;
		}
		if (!ValueMatches(input, inputPort->ValueType)) {
			return NodeExecutionStatus::TypeMismatch;
		}
		Records[NodeIndexes.at(node)].Inputs[std::string(port)] = std::move(input);
		return MarkDirty(node);
	}

	NodeExecutionStatus NodeRuntime::SetEnabled(core::Name node, bool enabled) {
		const auto found = NodeIndexes.find(node);
		if (found == NodeIndexes.end()) {
			return NodeExecutionStatus::InvalidNode;
		}
		Nodes[found->second].Enabled = enabled;
		return MarkDirty(node);
	}

	NodeExecutionStatus NodeRuntime::SetBypassMode(core::Name node, NodeBypassMode bypassMode) {
		const auto found = NodeIndexes.find(node);
		if (found == NodeIndexes.end()) {
			return NodeExecutionStatus::InvalidNode;
		}
		Nodes[found->second].BypassMode = bypassMode;
		return MarkDirty(node);
	}

	NodeExecutionStatus NodeRuntime::MarkDirty(core::Name node) {
		const auto start = NodeIndexes.find(node);
		if (start == NodeIndexes.end()) {
			return NodeExecutionStatus::InvalidNode;
		}
		std::deque<core::Name> pending = {node};
		std::unordered_map<core::Name, bool> visited;
		while (!pending.empty()) {
			const core::Name current = pending.front();
			pending.pop_front();
			if (!visited.emplace(current, true).second) {
				continue;
			}
			NodeExecutionRecord &record = Records[NodeIndexes.at(current)];
			record.Dirty = true;
			record.Revision++;
			if (record.State != NodeExecutionState::Running) {
				record.State = NodeExecutionState::Queued;
				record.Error.clear();
			}
			for (const NodeExecutionConnection &wire : Wires) {
				if (wire.FromNode == current) {
					pending.push_back(wire.ToNode);
				}
			}
		}
		return NodeExecutionStatus::Ok;
	}

	NodeExecutionStatus NodeRuntime::BuildOrder(std::vector<size_t> &order) const {
		order.clear();
		std::vector<size_t> incoming(Nodes.size());
		std::vector<std::vector<size_t>> outgoing(Nodes.size());
		for (const NodeExecutionConnection &wire : Wires) {
			const size_t from = NodeIndexes.at(wire.FromNode);
			const size_t to = NodeIndexes.at(wire.ToNode);
			if (Nodes[from].BreaksCycle) {
				continue;
			}
			outgoing[from].push_back(to);
			incoming[to]++;
		}
		std::deque<size_t> ready;
		for (size_t index = 0; index < incoming.size(); index++) {
			if (incoming[index] == 0) {
				ready.push_back(index);
			}
		}
		while (!ready.empty()) {
			const size_t current = ready.front();
			ready.pop_front();
			order.push_back(current);
			for (const size_t dependent : outgoing[current]) {
				if (--incoming[dependent] == 0) {
					ready.push_back(dependent);
				}
			}
		}
		return order.size() == Nodes.size() ? NodeExecutionStatus::Ok : NodeExecutionStatus::Cycle;
	}

	NodeExecutionStatus
	NodeRuntime::BuildInputs(const NodeExecutionNode &node, NodeValues &inputs, std::string &error) const {
		const size_t index = NodeIndexes.at(node.Id);
		const NodeSchema *schema = Schemas.Find(node.Schema.Text());
		if (schema == nullptr) {
			return NodeExecutionStatus::UnknownSchema;
		}
		inputs = Records[index].Inputs;
		for (const NodePortSchema &port : schema->Ports) {
			if (port.Direction == NodePortDirection::Input && !inputs.contains(port.Id) &&
				!std::holds_alternative<std::monostate>(port.DefaultValue)) {
				inputs.emplace(port.Id, port.DefaultValue);
			}
		}
		for (const NodeExecutionConnection &wire : Wires) {
			if (wire.ToNode != node.Id) {
				continue;
			}
			const size_t fromIndex = NodeIndexes.at(wire.FromNode);
			const NodeExecutionRecord &fromRecord = Records[fromIndex];
			if (!Nodes[fromIndex].BreaksCycle && (fromRecord.State == NodeExecutionState::Running ||
												  fromRecord.State == NodeExecutionState::Queued)) {
				return NodeExecutionStatus::Pending;
			}
			if (fromRecord.State == NodeExecutionState::Error) {
				error = "upstream node '" + std::string(Nodes[fromIndex].Id.Text()) +
						"' failed: " + fromRecord.Error;
				return NodeExecutionStatus::EvaluationFailed;
			}
			const auto output = fromRecord.Outputs.find(wire.FromPort);
			if (output == fromRecord.Outputs.end()) {
				error = "upstream node '" + std::string(Nodes[fromIndex].Id.Text()) + "' produced no '" +
						wire.FromPort + "' output";
				return NodeExecutionStatus::EvaluationFailed;
			}
			const NodePortSchema *inputPort = FindPort(*schema, wire.ToPort, NodePortDirection::Input);
			if (inputPort == nullptr || !ValueMatches(output->second, inputPort->ValueType)) {
				error = inputPort == nullptr ? "linked input port no longer exists"
											 : Mismatch(wire.ToPort, inputPort->ValueType, output->second);
				return NodeExecutionStatus::TypeMismatch;
			}
			inputs[wire.ToPort] = output->second;
		}
		return NodeExecutionStatus::Ok;
	}

	NodeExecutionStatus NodeRuntime::Finish(core::Name node, NodeValues outputs, std::string error) {
		const size_t index = NodeIndexes.at(node);
		NodeExecutionRecord &record = Records[index];
		if (!error.empty()) {
			record.State = NodeExecutionState::Error;
			record.Error = std::move(error);
			record.Dirty = false;
			return NodeExecutionStatus::EvaluationFailed;
		}
		const NodeSchema *schema = Schemas.Find(Nodes[index].Schema.Text());
		if (schema == nullptr) {
			return Finish(node, {}, "node schema no longer exists");
		}
		for (const auto &[id, value] : outputs) {
			const NodePortSchema *outputPort = FindPort(*schema, id, NodePortDirection::Output);
			if (outputPort == nullptr) {
				return Finish(node, {}, "node produced undeclared output '" + id + "'");
			}
			if (!ValueMatches(value, outputPort->ValueType)) {
				return Finish(node, {}, Mismatch(id, outputPort->ValueType, value));
			}
		}
		for (const NodePortSchema &port : schema->Ports) {
			if (port.Direction == NodePortDirection::Output && !port.Optional && !outputs.contains(port.Id)) {
				return Finish(node, {}, "node produced no '" + port.Id + "' output");
			}
		}
		for (const NodeExecutionConnection &wire : Wires) {
			if (wire.FromNode != node) {
				continue;
			}
			const auto output = outputs.find(wire.FromPort);
			if (output == outputs.end()) {
				return Finish(node, {}, "node produced no '" + wire.FromPort + "' output");
			}
			const NodePortSchema *outputPort = FindPort(*schema, wire.FromPort, NodePortDirection::Output);
			const NodeExecutionNode *destination = FindNode(wire.ToNode);
			const NodeSchema *destinationSchema = Schemas.Find(destination->Schema.Text());
			const NodePortSchema *inputPort =
				FindPort(*destinationSchema, wire.ToPort, NodePortDirection::Input);
			if (!ValueMatches(output->second, outputPort->ValueType)) {
				return Finish(node, {}, Mismatch(wire.FromPort, outputPort->ValueType, output->second));
			}
			if (!ValueMatches(output->second, inputPort->ValueType)) {
				return Finish(node, {}, Mismatch(wire.ToPort, inputPort->ValueType, output->second));
			}
		}
		record.Outputs = std::move(outputs);
		record.Error.clear();
		record.State = NodeExecutionState::Complete;
		record.Dirty = false;
		return NodeExecutionStatus::Ok;
	}

	NodeExecutionStatus NodeRuntime::Evaluate(NodeExecutionDispatcher *dispatcher) {
		std::vector<size_t> order;
		if (const NodeExecutionStatus status = BuildOrder(order); status != NodeExecutionStatus::Ok) {
			return status;
		}
		bool pending = false;
		bool failed = false;
		for (const size_t index : order) {
			NodeExecutionNode &node = Nodes[index];
			NodeExecutionRecord &record = Records[index];
			if (!record.Dirty) {
				continue;
			}
			if (record.State == NodeExecutionState::Running) {
				pending = true;
				continue;
			}
			NodeValues inputs;
			std::string error;
			const NodeExecutionStatus inputStatus = BuildInputs(node, inputs, error);
			if (inputStatus == NodeExecutionStatus::Pending) {
				pending = true;
				continue;
			}
			if (inputStatus != NodeExecutionStatus::Ok) {
				Finish(node.Id, {}, std::move(error));
				failed = true;
				continue;
			}
			const NodeSchema *schema = Schemas.Find(node.Schema.Text());
			if (schema == nullptr) {
				Finish(node.Id, {}, "node schema no longer exists");
				failed = true;
				continue;
			}
			if (!node.Enabled || node.BypassMode == NodeBypassMode::Bypass) {
				NodeValues outputs;
				for (const NodeBypassMapping &mapping : schema->BypassMappings) {
					const auto input = inputs.find(mapping.Input);
					if (input == inputs.end()) {
						error = "bypass input '" + mapping.Input + "' is not available";
						break;
					}
					outputs[mapping.Output] = input->second;
				}
				if (Finish(node.Id, std::move(outputs), std::move(error)) != NodeExecutionStatus::Ok) {
					failed = true;
				}
				continue;
			}
			record.State = NodeExecutionState::Running;
			NodeValues outputs;
			if (dispatcher == nullptr) {
				if (EvaluateNode(*schema, inputs, outputs, &error) != NodeEvaluationStatus::Ok) {
					Finish(node.Id, {}, std::move(error));
					failed = true;
					continue;
				}
				if (Finish(node.Id, std::move(outputs), {}) != NodeExecutionStatus::Ok) {
					failed = true;
				}
				continue;
			}
			const NodeDispatchResult dispatch = dispatcher->Dispatch(
				{node.Id, node.Schema, record.Revision, std::move(inputs)}, outputs, error
			);
			if (dispatch == NodeDispatchResult::Pending) {
				pending = true;
				continue;
			}
			if (Finish(
					node.Id,
					std::move(outputs),
					dispatch == NodeDispatchResult::Error ? std::move(error) : std::string{}
				) != NodeExecutionStatus::Ok) {
				failed = true;
			}
		}
		return failed	 ? NodeExecutionStatus::EvaluationFailed
			   : pending ? NodeExecutionStatus::Pending
						 : NodeExecutionStatus::Ok;
	}

	NodeExecutionStatus
	NodeRuntime::CompleteAsync(core::Name node, uint64_t revision, NodeValues outputs, std::string error) {
		const auto found = NodeIndexes.find(node);
		if (found == NodeIndexes.end()) {
			return NodeExecutionStatus::InvalidNode;
		}
		NodeExecutionRecord &record = Records[found->second];
		if (record.State != NodeExecutionState::Running) {
			return NodeExecutionStatus::InvalidNode;
		}
		if (revision != record.Revision) {
			record.State = NodeExecutionState::Queued;
			return NodeExecutionStatus::StaleCompletion;
		}
		return Finish(node, std::move(outputs), std::move(error));
	}

	const NodeExecutionRecord *NodeRuntime::Find(core::Name node) const {
		const auto found = NodeIndexes.find(node);
		return found == NodeIndexes.end() ? nullptr : &Records[found->second];
	}

	const NodeExecutionNode *NodeRuntime::FindNode(core::Name node) const {
		const auto found = NodeIndexes.find(node);
		return found == NodeIndexes.end() ? nullptr : &Nodes[found->second];
	}
}
