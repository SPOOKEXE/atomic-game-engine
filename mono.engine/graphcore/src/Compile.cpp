#include "Validation.hpp"

#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <set>
#include <unordered_map>

namespace engine::graphcore {

	namespace {
		void FindCycle(const Topology &topology, std::span<const size_t> incoming, Diagnostic &diagnostic) {
			struct Visit {
				NodeIndex Node;
				size_t Next = 0;
			};
			std::vector<uint8_t> colours(incoming.size());
			std::vector<Visit> pending;
			for (size_t start = 0; start < incoming.size(); ++start) {
				if (incoming[start] == 0 || colours[start] != 0) continue;
				pending.push_back({static_cast<NodeIndex>(start)});
				colours[start] = 1;
				while (!pending.empty()) {
					Visit &visit = pending.back();
					const auto &downstream = topology.Dependents[visit.Node];
					if (visit.Next == downstream.size()) {
						colours[visit.Node] = 2;
						pending.pop_back();
						continue;
					}
					const NodeIndex next = downstream[visit.Next++];
					if (incoming[next] == 0 || colours[next] == 2) continue;
					if (colours[next] == 0) {
						colours[next] = 1;
						pending.push_back({next});
						continue;
					}
					const auto first =
						std::find_if(pending.begin(), pending.end(), [next](const Visit &entry) {
							return entry.Node == next;
						});
					for (auto entry = first; entry != pending.end(); ++entry)
						diagnostic.Cycle.push_back(entry->Node);
					diagnostic.Cycle.push_back(next);
					return;
				}
			}
		}

		struct SchemaPorts {
			std::unordered_map<std::string_view, uint32_t> Inputs;
			std::unordered_map<std::string_view, uint32_t> Outputs;
		};

		CompileStatus CheckGraphBudgets(
			std::span<const Node> nodes,
			std::span<const Connection> connections,
			std::span<const NodeSchema> schemas,
			const Limits &limits,
			Diagnostic &diagnostic
		) {
			if (nodes.size() > limits.MaximumNodes || nodes.size() > NO_NODE ||
				connections.size() > limits.MaximumConnections)
				return detail::Refuse(diagnostic, CompileStatus::LimitExceeded);
			size_t bytes = 0;
			if (detail::CheckSchemaBudgets(schemas, limits, bytes, diagnostic) != CompileStatus::Ok)
				return diagnostic.Status;
			for (const Node &node : nodes) {
				if (detail::CountIdentity(node.Id, bytes, limits, diagnostic, node.Id) != CompileStatus::Ok ||
					detail::CountIdentity(node.Schema, bytes, limits, diagnostic, node.Id) !=
						CompileStatus::Ok)
					return diagnostic.Status;
			}
			for (const Connection &connection : connections) {
				if (detail::CountIdentity(
						connection.FromNode, bytes, limits, diagnostic, connection.FromNode
					) != CompileStatus::Ok ||
					detail::CountIdentity(
						connection.FromPort,
						bytes,
						limits,
						diagnostic,
						connection.FromNode,
						connection.FromPort
					) != CompileStatus::Ok ||
					detail::CountIdentity(connection.ToNode, bytes, limits, diagnostic, connection.ToNode) !=
						CompileStatus::Ok ||
					detail::CountIdentity(
						connection.ToPort, bytes, limits, diagnostic, connection.ToNode, connection.ToPort
					) != CompileStatus::Ok)
					return diagnostic.Status;
			}
			return CompileStatus::Ok;
		}
	}

	CompileStatus CompileDependencies(
		size_t nodeCount,
		std::span<const Dependency> dependencies,
		const Limits &limits,
		Topology &out,
		Diagnostic &diagnostic,
		OrderPolicy policy
	) {
		try {
			if (nodeCount > limits.MaximumNodes || nodeCount > NO_NODE ||
				dependencies.size() > limits.MaximumConnections)
				return detail::Refuse(diagnostic, CompileStatus::LimitExceeded);
			if (policy != OrderPolicy::AuthoredSweep && policy != OrderPolicy::LowestReady)
				return detail::Refuse(diagnostic, CompileStatus::InvalidDependency);
			for (const Dependency &dependency : dependencies) {
				if (dependency.Before >= nodeCount || dependency.After >= nodeCount)
					return detail::Refuse(diagnostic, CompileStatus::InvalidDependency);
			}
			std::vector<Dependency> edges(dependencies.begin(), dependencies.end());
			std::sort(edges.begin(), edges.end(), [](const Dependency &left, const Dependency &right) {
				return std::array{left.Before, left.After} < std::array{right.Before, right.After};
			});
			edges.erase(
				std::unique(
					edges.begin(),
					edges.end(),
					[](const Dependency &left, const Dependency &right) {
						return left.Before == right.Before && left.After == right.After;
					}
				),
				edges.end()
			);
			Topology candidate;
			candidate.Order.reserve(nodeCount);
			candidate.Inputs.resize(nodeCount);
			candidate.Dependents.resize(nodeCount);
			std::vector<size_t> incoming(nodeCount);
			for (const Dependency &edge : edges) {
				candidate.Inputs[edge.After].push_back(edge.Before);
				candidate.Dependents[edge.Before].push_back(edge.After);
				++incoming[edge.After];
			}
			std::set<NodeIndex> ready;
			std::set<NodeIndex> nextSweep;
			for (size_t index = 0; index < nodeCount; ++index) {
				if (incoming[index] == 0) ready.insert(static_cast<NodeIndex>(index));
			}
			std::vector<size_t> depth(nodeCount);
			size_t waveCount = 0;
			while (!ready.empty()) {
				const NodeIndex current = *ready.begin();
				ready.erase(ready.begin());
				candidate.Order.push_back(current);
				waveCount = std::max(waveCount, depth[current] + 1);
				for (const NodeIndex after : candidate.Dependents[current]) {
					depth[after] = std::max(depth[after], depth[current] + 1);
					if (--incoming[after] != 0) continue;
					// Earlier declarations wait for the next sweep. A minimum-ready
					// policy deliberately reconsiders them immediately instead.
					auto &destination =
						policy == OrderPolicy::AuthoredSweep && after < current ? nextSweep : ready;
					destination.insert(after);
				}
				if (ready.empty()) ready.swap(nextSweep);
			}
			if (candidate.Order.size() != nodeCount) {
				detail::Refuse(diagnostic, CompileStatus::Cycle);
				const auto blocked =
					std::find_if(incoming.begin(), incoming.end(), [](size_t count) { return count != 0; });
				diagnostic.Blocked = static_cast<NodeIndex>(blocked - incoming.begin());
				FindCycle(candidate, incoming, diagnostic);
				return diagnostic.Status;
			}
			candidate.Waves.resize(waveCount);
			for (size_t index = 0; index < nodeCount; ++index)
				candidate.Waves[depth[index]].push_back(static_cast<NodeIndex>(index));
			out = std::move(candidate);
			diagnostic = {};
			return CompileStatus::Ok;
		} catch (const std::bad_alloc &) {
			diagnostic = {};
			diagnostic.Status = CompileStatus::AllocationFailed;
			return diagnostic.Status;
		}
	}

	CompileStatus CompileGraph(
		std::span<const Node> nodes,
		std::span<const Connection> connections,
		std::span<const NodeSchema> schemas,
		const Limits &limits,
		CompiledGraph &out,
		Diagnostic &diagnostic,
		OrderPolicy policy
	) {
		try {
			if (CheckGraphBudgets(nodes, connections, schemas, limits, diagnostic) != CompileStatus::Ok ||
				ValidateSchemas(schemas, limits, diagnostic) != CompileStatus::Ok)
				return diagnostic.Status;
			std::unordered_map<std::string_view, size_t> schemaNames;
			std::vector<SchemaPorts> schemaPorts(schemas.size());
			for (size_t index = 0; index < schemas.size(); ++index) {
				schemaNames.emplace(schemas[index].Id, index);
				for (size_t portIndex = 0; portIndex < schemas[index].Ports.size(); ++portIndex) {
					const PortSchema &port = schemas[index].Ports[portIndex];
					auto &ports = port.Direction == PortDirection::Input ? schemaPorts[index].Inputs
																		 : schemaPorts[index].Outputs;
					ports.emplace(port.Id, static_cast<uint32_t>(portIndex));
				}
			}
			std::unordered_map<std::string_view, NodeIndex> nodeNames;
			std::vector<size_t> nodeSchemas;
			nodeSchemas.reserve(nodes.size());
			for (size_t index = 0; index < nodes.size(); ++index) {
				const Node &node = nodes[index];
				if (!nodeNames.emplace(node.Id, static_cast<NodeIndex>(index)).second)
					return detail::Refuse(diagnostic, CompileStatus::DuplicateNode, node.Id);
				const auto schema = schemaNames.find(node.Schema);
				if (schema == schemaNames.end())
					return detail::Refuse(diagnostic, CompileStatus::UnknownSchema, node.Id);
				nodeSchemas.push_back(schema->second);
			}
			CompiledGraph candidate;
			candidate.Connections.reserve(connections.size());
			std::vector<Dependency> dependencies;
			dependencies.reserve(connections.size());
			std::vector<std::unordered_map<uint32_t, size_t>> inputCounts(nodes.size());
			std::set<std::array<uint32_t, 4>> uniqueConnections;
			for (const Connection &connection : connections) {
				const auto from = nodeNames.find(connection.FromNode);
				const auto to = nodeNames.find(connection.ToNode);
				if (from == nodeNames.end())
					return detail::Refuse(
						diagnostic, CompileStatus::UnknownNode, connection.FromNode, connection.FromPort
					);
				if (to == nodeNames.end())
					return detail::Refuse(
						diagnostic, CompileStatus::UnknownNode, connection.ToNode, connection.ToPort
					);
				const size_t fromSchema = nodeSchemas[from->second];
				const size_t toSchema = nodeSchemas[to->second];
				const auto output = schemaPorts[fromSchema].Outputs.find(connection.FromPort);
				const auto input = schemaPorts[toSchema].Inputs.find(connection.ToPort);
				if (output == schemaPorts[fromSchema].Outputs.end())
					return detail::Refuse(
						diagnostic, CompileStatus::UnknownPort, connection.FromNode, connection.FromPort
					);
				if (input == schemaPorts[toSchema].Inputs.end())
					return detail::Refuse(
						diagnostic, CompileStatus::UnknownPort, connection.ToNode, connection.ToPort
					);
				const PortSchema &outputPort = schemas[fromSchema].Ports[output->second];
				const PortSchema &inputPort = schemas[toSchema].Ports[input->second];
				if (ComparePortTypes(outputPort.ValueType, inputPort.ValueType) ==
					TypeCompatibility::Incompatible)
					return detail::Refuse(
						diagnostic, CompileStatus::TypeMismatch, connection.ToNode, connection.ToPort
					);
				if (!uniqueConnections.insert({from->second, output->second, to->second, input->second})
						 .second)
					return detail::Refuse(
						diagnostic, CompileStatus::DuplicateConnection, connection.ToNode, connection.ToPort
					);
				size_t &count = inputCounts[to->second][input->second];
				if (inputPort.MaximumConnections != 0 && count >= inputPort.MaximumConnections)
					return detail::Refuse(
						diagnostic, CompileStatus::TooManyConnections, connection.ToNode, connection.ToPort
					);
				++count;
				candidate.Connections.push_back({from->second, output->second, to->second, input->second});
				dependencies.push_back({from->second, to->second});
			}
			for (size_t index = 0; index < nodes.size(); ++index) {
				const auto &ports = schemas[nodeSchemas[index]].Ports;
				for (size_t port = 0; port < ports.size(); ++port) {
					if (ports[port].Direction == PortDirection::Input && !ports[port].Optional &&
						!inputCounts[index].contains(static_cast<uint32_t>(port)))
						return detail::Refuse(
							diagnostic, CompileStatus::MissingInput, nodes[index].Id, ports[port].Id
						);
				}
			}
			if (CompileDependencies(
					nodes.size(), dependencies, limits, candidate.Dependencies, diagnostic, policy
				) != CompileStatus::Ok) {
				if (diagnostic.Blocked < nodes.size())
					diagnostic.Node.assign(nodes[diagnostic.Blocked].Id.substr(0, 128));
				return diagnostic.Status;
			}
			out = std::move(candidate);
			diagnostic = {};
			return CompileStatus::Ok;
		} catch (const std::bad_alloc &) {
			diagnostic = {};
			diagnostic.Status = CompileStatus::AllocationFailed;
			return diagnostic.Status;
		}
	}
}
