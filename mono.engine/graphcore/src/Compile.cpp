#include "Validation.hpp"

#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
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

	OrderWorkspace::OrderWorkspace(size_t maximumNodes, size_t maximumDependencies) {
		if (maximumNodes > NO_NODE || maximumNodes == std::numeric_limits<size_t>::max())
			throw std::length_error("graph workspace node capacity is not representable");
		Offsets.resize(maximumNodes + 1);
		Incoming.resize(maximumNodes);
		CursorDepth.resize(maximumNodes);
		Targets.resize(maximumDependencies);
		Ready.resize(maximumNodes);
		NextReady.resize(maximumNodes);
		Candidate.resize(maximumNodes);
	}

	OrderResult CompileOrder(
		size_t nodeCount,
		std::span<const Dependency> dependencies,
		const Limits &limits,
		OrderWorkspace &workspace,
		std::span<NodeIndex> out,
		OrderPolicy policy
	) noexcept {
		if (nodeCount > limits.MaximumNodes || nodeCount > NO_NODE || nodeCount >= workspace.Offsets.size() ||
			nodeCount > workspace.Incoming.size() || nodeCount > workspace.CursorDepth.size() ||
			nodeCount > workspace.Ready.size() || nodeCount > workspace.NextReady.size() ||
			nodeCount > workspace.Candidate.size() || nodeCount > out.size() ||
			dependencies.size() > limits.MaximumConnections || dependencies.size() > workspace.Targets.size())
			return {.Status = CompileStatus::LimitExceeded};
		if (policy != OrderPolicy::AuthoredSweep && policy != OrderPolicy::LowestReady &&
			policy != OrderPolicy::FifoReady)
			return {.Status = CompileStatus::InvalidDependency};
		for (const Dependency &edge : dependencies) {
			if (edge.Before >= nodeCount || edge.After >= nodeCount)
				return {.Status = CompileStatus::InvalidDependency};
		}
		std::fill_n(workspace.Offsets.begin(), nodeCount + 1, size_t{0});
		std::fill_n(workspace.Incoming.begin(), nodeCount, size_t{0});
		for (const Dependency &edge : dependencies) {
			++workspace.Offsets[edge.Before + 1];
			++workspace.Incoming[edge.After];
		}
		for (size_t index = 0; index < nodeCount; ++index)
			workspace.Offsets[index + 1] += workspace.Offsets[index];
		std::copy_n(workspace.Offsets.begin(), nodeCount, workspace.CursorDepth.begin());
		for (const Dependency &edge : dependencies)
			workspace.Targets[workspace.CursorDepth[edge.Before]++] = edge.After;
		std::fill_n(workspace.CursorDepth.begin(), nodeCount, size_t{0});

		std::span<NodeIndex> ready = workspace.Ready;
		std::span<NodeIndex> nextReady = workspace.NextReady;
		size_t readyCount = 0;
		size_t nextCount = 0;
		size_t fifoRead = 0;
		size_t fifoWrite = 0;
		for (size_t index = 0; index < nodeCount; ++index) {
			if (workspace.Incoming[index] != 0) continue;
			if (policy == OrderPolicy::FifoReady)
				ready[fifoWrite++] = static_cast<NodeIndex>(index);
			else {
				ready[readyCount++] = static_cast<NodeIndex>(index);
				std::push_heap(ready.begin(), ready.begin() + readyCount, std::greater<NodeIndex>{});
			}
		}
		size_t emitted = 0;
		while (policy == OrderPolicy::FifoReady ? fifoRead < fifoWrite : readyCount != 0) {
			NodeIndex current;
			if (policy == OrderPolicy::FifoReady)
				current = ready[fifoRead++];
			else {
				std::pop_heap(ready.begin(), ready.begin() + readyCount, std::greater<NodeIndex>{});
				current = ready[--readyCount];
			}
			workspace.Candidate[emitted++] = current;
			for (size_t edge = workspace.Offsets[current]; edge < workspace.Offsets[current + 1]; ++edge) {
				const NodeIndex after = workspace.Targets[edge];
				workspace.CursorDepth[after] =
					std::max(workspace.CursorDepth[after], workspace.CursorDepth[current] + 1);
				if (--workspace.Incoming[after] != 0) continue;
				if (policy == OrderPolicy::FifoReady) {
					ready[fifoWrite++] = after;
					continue;
				}
				// An earlier declaration waits for the next sweep; LowestReady
				// deliberately reconsiders it immediately.
				const bool defer = policy == OrderPolicy::AuthoredSweep && after < current;
				auto destination = defer ? nextReady : ready;
				size_t &count = defer ? nextCount : readyCount;
				destination[count++] = after;
				std::push_heap(destination.begin(), destination.begin() + count, std::greater<NodeIndex>{});
			}
			if (readyCount == 0 && policy == OrderPolicy::AuthoredSweep) {
				std::swap(ready, nextReady);
				std::swap(readyCount, nextCount);
			}
		}
		if (emitted != nodeCount) {
			const auto first = workspace.Incoming.begin();
			const auto blocked =
				std::find_if(first, first + nodeCount, [](size_t count) { return count != 0; });
			return {.Status = CompileStatus::Cycle, .Blocked = static_cast<NodeIndex>(blocked - first)};
		}
		std::copy_n(workspace.Candidate.begin(), nodeCount, out.begin());
		return {.Count = nodeCount};
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
				nodeCount == std::numeric_limits<size_t>::max() ||
				dependencies.size() > limits.MaximumConnections)
				return detail::Refuse(diagnostic, CompileStatus::LimitExceeded);
			if (policy != OrderPolicy::AuthoredSweep && policy != OrderPolicy::LowestReady &&
				policy != OrderPolicy::FifoReady)
				return detail::Refuse(diagnostic, CompileStatus::InvalidDependency);
			for (const Dependency &edge : dependencies) {
				if (edge.Before >= nodeCount || edge.After >= nodeCount)
					return detail::Refuse(diagnostic, CompileStatus::InvalidDependency);
			}
			OrderWorkspace workspace(nodeCount, dependencies.size());
			Topology candidate;
			candidate.Order.resize(nodeCount);
			const OrderResult result =
				CompileOrder(nodeCount, dependencies, limits, workspace, candidate.Order, policy);
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
			candidate.Inputs.resize(nodeCount);
			candidate.Dependents.resize(nodeCount);
			for (const Dependency &edge : edges) {
				candidate.Inputs[edge.After].push_back(edge.Before);
				candidate.Dependents[edge.Before].push_back(edge.After);
			}
			if (result.Status != CompileStatus::Ok) {
				detail::Refuse(diagnostic, result.Status);
				diagnostic.Blocked = result.Blocked;
				if (result.Status == CompileStatus::Cycle)
					FindCycle(candidate, std::span(workspace.Incoming).first(nodeCount), diagnostic);
				return diagnostic.Status;
			}
			size_t waveCount = 0;
			for (size_t index = 0; index < nodeCount; ++index)
				waveCount = std::max(waveCount, workspace.CursorDepth[index] + 1);
			candidate.Waves.resize(waveCount);
			for (size_t index = 0; index < nodeCount; ++index)
				candidate.Waves[workspace.CursorDepth[index]].push_back(static_cast<NodeIndex>(index));
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
