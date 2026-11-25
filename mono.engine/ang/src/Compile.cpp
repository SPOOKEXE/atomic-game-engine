#include <engine/ang/Compile.hpp>
#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <unordered_set>

namespace engine::ang {

	const char *Describe(DiagnosticCode code) {
		switch (code) {
		case DiagnosticCode::Graph:
			return "graph refused";
		case DiagnosticCode::UnknownType:
			return "unknown type";
		case DiagnosticCode::UnknownPort:
			return "unknown port";
		case DiagnosticCode::TypeMismatch:
			return "link types are incompatible";
		case DiagnosticCode::GenericConflict:
			return "generic port cannot resolve to this type";
		case DiagnosticCode::AsyncInTick:
			return "async node in a Tick graph";
		case DiagnosticCode::InvalidBypass:
			return "bypassed node has no bypass route";
		case DiagnosticCode::SubgraphRecursion:
			return "subgraph includes itself";
		case DiagnosticCode::SubgraphPort:
			return "subgraph port is not exposed";
		}
		return "graph refused";
	}

	int64_t Plan::Find(std::string_view id) const {
		const auto found = Index.find(std::string(id));
		return found == Index.end() ? -1 : static_cast<int64_t>(found->second);
	}

	void Plan::Reindex() {
		Index.clear();
		for (uint32_t index = 0; index < Nodes.size(); ++index) {
			Index.emplace(Nodes[index].Id, index);
		}
	}

	std::vector<bool> Plan::DirtySince(const Plan &previous) const {
		std::vector<bool> dirty(Nodes.size(), true);
		for (size_t index = 0; index < Nodes.size(); ++index) {
			const int64_t old = previous.Find(Nodes[index].Id);
			dirty[index] = old < 0 || previous.Nodes[static_cast<size_t>(old)].Hash != Nodes[index].Hash;
		}
		return dirty;
	}

	namespace {
		// A port declared by a definition, a subgraph, or invented for an unknown node.
		const PortDef *
		DeclaredPort(const Registry &registry, std::string_view def, std::string_view port, Direction dir) {
			if (const auto node = registry.FindNode(def)) {
				return dir == Direction::Input ? node->FindInput(port) : node->FindOutput(port);
			}
			if (const SubgraphDef *subgraph = registry.FindSubgraph(def)) {
				const auto &ports = dir == Direction::Input ? subgraph->Inputs : subgraph->Outputs;
				for (const auto &exposed : ports) {
					if (exposed.Port.Name == port) {
						return &exposed.Port;
					}
				}
			}
			return nullptr;
		}

		const PortDef &WildcardPort(Direction dir) {
			static const PortDef input{
				.Name = "*", .Type = TypeId(ANY_TYPE), .Optional = true, .MaximumLinks = 0
			};
			static const PortDef output{.Name = "*", .Type = TypeId(ANY_TYPE), .Dir = Direction::Output};
			return dir == Direction::Input ? input : output;
		}

		struct WorkNode {
			std::string Id{};
			const NodeInstance *Source = nullptr;
			std::shared_ptr<const NodeDef> Def{};
			// Generic name -> resolved type.
			std::map<std::string, TypeId> Generics{};
			// Ports invented for an unknown definition, from the links that touch it.
			std::vector<std::string> UnknownInputs{};
			std::vector<std::string> UnknownOutputs{};
		};

		struct Builder {
			const Registry &Types;
			const CompileOptions &Options;
			std::vector<Diagnostic> Diagnostics{};
			bool Refused = false;
			std::vector<WorkNode> Nodes{};
			std::vector<Link> Links{};
			std::unordered_map<std::string, size_t> NodeIndex{};

			void Report(Diagnostic diagnostic) {
				Refused = true;
				if (Diagnostics.size() < Options.MaximumDiagnostics) {
					if (diagnostic.Message.empty()) {
						diagnostic.Message = Describe(diagnostic.Code);
					}
					Diagnostics.push_back(std::move(diagnostic));
				}
			}

			// Copies `graph` in with ids prefixed, replacing each subgraph node with its body.
			// `exposed` maps "outer node \n port \n dir" to the inner endpoint.
			void Flatten(const Graph &graph, const std::string &prefix, std::vector<std::string> &stack) {
				std::unordered_map<std::string, const SubgraphDef *> subgraphs;
				for (const NodeInstance &node : graph.Nodes()) {
					const SubgraphDef *subgraph = Types.FindSubgraph(node.Def);
					if (subgraph == nullptr) {
						WorkNode work{
							.Id = prefix + node.Id, .Source = &node, .Def = Types.FindNode(node.Def)
						};
						NodeIndex.emplace(work.Id, Nodes.size());
						Nodes.push_back(std::move(work));
						continue;
					}
					const bool recursive = std::find(stack.begin(), stack.end(), subgraph->Id) != stack.end();
					if (recursive || stack.size() >= Options.MaximumSubgraphDepth) {
						Report({.Code = DiagnosticCode::SubgraphRecursion, .Node = prefix + node.Id});
						continue;
					}
					subgraphs.emplace(node.Id, subgraph);
					stack.push_back(subgraph->Id);
					Flatten(*subgraph->Body, prefix + node.Id + "/", stack);
					stack.pop_back();
				}
				for (const Link &wire : graph.Links()) {
					Link flat{prefix + wire.FromNode, wire.FromPort, prefix + wire.ToNode, wire.ToPort};
					if (auto found = subgraphs.find(wire.FromNode); found != subgraphs.end()) {
						if (!MapExposed(
								found->second->Outputs, prefix + wire.FromNode, flat.FromNode, flat.FromPort
							)) {
							continue;
						}
					}
					if (auto found = subgraphs.find(wire.ToNode); found != subgraphs.end()) {
						if (!MapExposed(
								found->second->Inputs, prefix + wire.ToNode, flat.ToNode, flat.ToPort
							)) {
							continue;
						}
					}
					Links.push_back(std::move(flat));
				}
			}

			bool MapExposed(
				const std::vector<SubgraphPort> &ports,
				const std::string &outer,
				std::string &node,
				std::string &port
			) {
				for (const auto &exposed : ports) {
					if (exposed.Port.Name == port) {
						node = outer + "/" + exposed.Node;
						port = exposed.NodePort;
						return true;
					}
				}
				Report({.Code = DiagnosticCode::SubgraphPort, .Node = outer, .Port = port});
				return false;
			}

			WorkNode *Find(const std::string &id) {
				const auto found = NodeIndex.find(id);
				return found == NodeIndex.end() ? nullptr : &Nodes[found->second];
			}

			const PortDef *Port(const WorkNode &node, std::string_view port, Direction dir) const {
				if (node.Def == nullptr) {
					return &WildcardPort(dir);
				}
				return dir == Direction::Input ? node.Def->FindInput(port) : node.Def->FindOutput(port);
			}

			// Concrete type of a port, or empty while its generic is unresolved.
			TypeId TypeOf(const WorkNode &node, const PortDef &port) const {
				if (port.Generic.empty()) {
					return port.Type;
				}
				const auto found = node.Generics.find(port.Generic);
				return found == node.Generics.end() ? TypeId{} : found->second;
			}

			bool Allowed(const PortDef &port, const TypeId &type) const {
				if (Types.FindType(type) == nullptr) {
					return false;
				}
				return port.Constraints.empty() ||
					   std::find(port.Constraints.begin(), port.Constraints.end(), type) !=
						   port.Constraints.end();
			}

			void CheckNodes() {
				for (WorkNode &node : Nodes) {
					if (node.Def == nullptr) {
						continue;
					}
					if (node.Def->Mode == ExecMode::Async && Options.Runs == Domain::Tick) {
						Report(
							{.Code = DiagnosticCode::AsyncInTick,
							 .Node = node.Id,
							 .Message =
								 "node '" + node.Id + "' is Async and this graph runs in the Tick domain"}
						);
					}
					if (node.Source->Bypassed && node.Def->Bypass.empty()) {
						Report({.Code = DiagnosticCode::InvalidBypass, .Node = node.Id});
					}
				}
			}

			// Checks link ends, then resolves generics from the first link that pins
			// them, repeating until nothing changes so generic chains settle.
			void ResolveTypes() {
				for (const Link &wire : Links) {
					WorkNode *from = Find(wire.FromNode);
					WorkNode *to = Find(wire.ToNode);
					if (from == nullptr || to == nullptr) {
						continue;
					}
					if (Port(*from, wire.FromPort, Direction::Output) == nullptr) {
						Report(
							{.Code = DiagnosticCode::UnknownPort, .Node = from->Id, .Port = wire.FromPort}
						);
					}
					if (Port(*to, wire.ToPort, Direction::Input) == nullptr) {
						Report({.Code = DiagnosticCode::UnknownPort, .Node = to->Id, .Port = wire.ToPort});
					}
					if (from->Def == nullptr) {
						from->UnknownOutputs.push_back(wire.FromPort);
					}
					if (to->Def == nullptr) {
						to->UnknownInputs.push_back(wire.ToPort);
					}
				}
				if (Refused) {
					return;
				}
				for (size_t pass = 0; pass <= Links.size(); ++pass) {
					bool changed = false;
					for (const Link &wire : Links) {
						WorkNode *from = Find(wire.FromNode);
						WorkNode *to = Find(wire.ToNode);
						if (from == nullptr || to == nullptr) {
							continue;
						}
						const PortDef &output = *Port(*from, wire.FromPort, Direction::Output);
						const PortDef &input = *Port(*to, wire.ToPort, Direction::Input);
						const TypeId fromType = TypeOf(*from, output);
						const TypeId toType = TypeOf(*to, input);
						if (fromType.empty() && !toType.empty() && toType != ANY_TYPE) {
							changed |= Pin(*from, output, toType);
						} else if (toType.empty() && !fromType.empty() && fromType != ANY_TYPE) {
							changed |= Pin(*to, input, fromType);
						}
					}
					if (!changed || Refused) {
						break;
					}
				}
			}

			bool Pin(WorkNode &node, const PortDef &port, const TypeId &type) {
				if (!Allowed(port, type)) {
					Report(
						{.Code = DiagnosticCode::GenericConflict,
						 .Node = node.Id,
						 .Port = port.Name,
						 .Message = "generic '" + port.Generic + "' cannot be '" + type + "'"}
					);
					return false;
				}
				node.Generics.emplace(port.Generic, type);
				return true;
			}

			// Resolved type for a port; an unresolved generic behaves as `any`.
			TypeId Resolved(const WorkNode &node, const PortDef &port) const {
				TypeId type = TypeOf(node, port);
				return type.empty() ? TypeId(ANY_TYPE) : type;
			}

			void CheckLinkTypes(std::vector<const Conversion *> &conversions) {
				conversions.assign(Links.size(), nullptr);
				for (size_t index = 0; index < Links.size(); ++index) {
					const Link &wire = Links[index];
					WorkNode *from = Find(wire.FromNode);
					WorkNode *to = Find(wire.ToNode);
					if (from == nullptr || to == nullptr) {
						continue;
					}
					const TypeId fromType = Resolved(*from, *Port(*from, wire.FromPort, Direction::Output));
					const TypeId toType = Resolved(*to, *Port(*to, wire.ToPort, Direction::Input));
					const LinkCheck check = Types.CheckLink(fromType, toType);
					if (!check.Ok) {
						Report(
							{.Code = DiagnosticCode::TypeMismatch,
							 .Node = to->Id,
							 .Port = wire.ToPort,
							 .Message = "'" + fromType + "' does not connect to '" + toType + "'"}
						);
						continue;
					}
					conversions[index] = check.Via;
				}
			}
		};

		void Unique(std::vector<std::string> &names) {
			std::vector<std::string> kept;
			for (auto &name : names) {
				if (std::find(kept.begin(), kept.end(), name) == kept.end()) {
					kept.push_back(std::move(name));
				}
			}
			names = std::move(kept);
		}

	}

	CompileResult
	Compile(const Graph &graph, const Registry &registry, const CompileOptions &options, Plan &out) {
		Builder builder{.Types = registry, .Options = options};
		std::vector<std::string> stack;
		builder.Flatten(graph, "", stack);
		builder.CheckNodes();
		builder.ResolveTypes();
		std::vector<const Conversion *> conversions;
		if (!builder.Refused) {
			builder.CheckLinkTypes(conversions);
		}
		if (builder.Refused) {
			return {.Ok = false, .Diagnostics = std::move(builder.Diagnostics)};
		}

		// Lower to graphcore. Types are already checked, so every port reads `any`
		// and every input is optional: a run may supply unlinked inputs, so a
		// missing one is a runtime Skipped, not a compile refusal.
		std::vector<graphcore::Node> nodes;
		std::vector<graphcore::NodeSchema> schemas;
		for (WorkNode &work : builder.Nodes) {
			Unique(work.UnknownInputs);
			Unique(work.UnknownOutputs);
			graphcore::NodeSchema schema{.Id = work.Id};
			const auto addPort = [&](const std::string &name, graphcore::PortDirection dir, size_t cap) {
				schema.Ports.push_back(
					{.Id = name,
					 .ValueType = std::string(ANY_TYPE),
					 .Direction = dir,
					 .MaximumConnections = cap,
					 .Optional = true}
				);
			};
			if (work.Def != nullptr) {
				for (const auto &port : work.Def->Inputs) {
					addPort(port.Name, graphcore::PortDirection::Input, port.MaximumLinks);
				}
				for (const auto &port : work.Def->Outputs) {
					addPort(port.Name, graphcore::PortDirection::Output, 0);
				}
			} else {
				for (const auto &name : work.UnknownInputs) {
					addPort(name, graphcore::PortDirection::Input, 0);
				}
				for (const auto &name : work.UnknownOutputs) {
					addPort(name, graphcore::PortDirection::Output, 0);
				}
			}
			nodes.push_back({.Id = work.Id, .Schema = work.Id});
			schemas.push_back(std::move(schema));
		}
		std::vector<graphcore::Connection> connections;
		connections.reserve(builder.Links.size());
		for (const Link &wire : builder.Links) {
			connections.push_back({wire.FromNode, wire.FromPort, wire.ToNode, wire.ToPort});
		}
		const graphcore::Limits limits{
			.MaximumNodes = options.MaximumNodes,
			.MaximumConnections = options.MaximumLinks,
			.MaximumSchemas = options.MaximumNodes,
			.MaximumPorts = options.MaximumPorts,
			.MaximumIdentityBytes = options.MaximumIdentityBytes,
		};
		graphcore::CompiledGraph compiled;
		graphcore::Diagnostic graphDiagnostic;
		const graphcore::CompileStatus status = graphcore::CompileGraph(
			nodes, connections, schemas, limits, compiled, graphDiagnostic, options.Order
		);
		if (status != graphcore::CompileStatus::Ok) {
			Diagnostic diagnostic{
				.Code = DiagnosticCode::Graph,
				.GraphStatus = status,
				.Node = graphDiagnostic.Node,
				.Port = graphDiagnostic.Port,
				.Message = graphcore::Describe(status),
			};
			for (const graphcore::NodeIndex index : graphDiagnostic.Cycle) {
				if (index < builder.Nodes.size()) {
					diagnostic.Cycle.push_back(builder.Nodes[index].Id);
				}
			}
			return {.Ok = false, .Diagnostics = {std::move(diagnostic)}};
		}

		Plan plan;
		plan.Runs = options.Runs;
		plan.Nodes.resize(builder.Nodes.size());
		for (size_t index = 0; index < builder.Nodes.size(); ++index) {
			const WorkNode &work = builder.Nodes[index];
			PlannedNode &planned = plan.Nodes[index];
			planned.Id = work.Id;
			planned.DefId = work.Source->Def;
			planned.Def = work.Def;
			planned.Bypassed = work.Source->Bypassed;
			auto params = std::make_shared<std::map<std::string, Value>>();
			if (work.Def != nullptr) {
				planned.Mode = work.Def->Mode;
				for (const auto &param : work.Def->Params) {
					(*params)[param.Name] = param.Default;
				}
				for (const auto &port : work.Def->Inputs) {
					planned.Inputs.push_back(
						{.Port = port.Name,
						 .Type = builder.Resolved(work, port),
						 .Optional = port.Optional,
						 .Default = port.Default}
					);
				}
				for (const auto &port : work.Def->Outputs) {
					planned.OutputNames.push_back(port.Name);
					planned.OutputTypes.push_back(builder.Resolved(work, port));
					const DataType *type = registry.FindType(planned.OutputTypes.back());
					planned.OutputBytes.push_back(type != nullptr ? type->Bytes : nullptr);
				}
			} else {
				for (const auto &name : work.UnknownInputs) {
					planned.Inputs.push_back({.Port = name, .Type = TypeId(ANY_TYPE), .Optional = true});
				}
				for (const auto &name : work.UnknownOutputs) {
					planned.OutputNames.push_back(name);
					planned.OutputTypes.push_back(TypeId(ANY_TYPE));
				}
			}
			for (const auto &[name, value] : work.Source->Params) {
				(*params)[name] = value;
			}
			planned.Params = std::move(params);
		}
		// graphcore keeps authored connection order; its port index counts inputs first.
		for (size_t index = 0; index < compiled.Connections.size(); ++index) {
			const auto &resolved = compiled.Connections[index];
			PlannedNode &to = plan.Nodes[resolved.ToNode];
			const size_t fromInputs = plan.Nodes[resolved.FromNode].Inputs.size();
			PlannedSource source{
				.Node = resolved.FromNode, .Output = static_cast<uint32_t>(resolved.FromPort - fromInputs)
			};
			if (conversions[index] != nullptr) {
				source.Convert = conversions[index]->Convert;
			}
			to.Inputs[resolved.ToPort].Sources.push_back(std::move(source));
		}
		for (uint32_t wave = 0; wave < compiled.Dependencies.Waves.size(); ++wave) {
			std::vector<uint32_t> members;
			for (const graphcore::NodeIndex index : compiled.Dependencies.Waves[wave]) {
				plan.Nodes[index].Wave = wave;
				members.push_back(index);
			}
			plan.Waves.push_back(std::move(members));
		}
		plan.Order.assign(compiled.Dependencies.Order.begin(), compiled.Dependencies.Order.end());
		uint64_t planHash = HASH_SEED;
		for (const graphcore::NodeIndex index : compiled.Dependencies.Order) {
			PlannedNode &planned = plan.Nodes[index];
			uint64_t hash = HashText(HASH_SEED, planned.DefId);
			hash = HashMix(hash, planned.Def != nullptr ? planned.Def->Version : 0);
			hash = HashMix(hash, planned.Bypassed ? 1 : 0);
			for (const auto &[name, value] : *planned.Params) {
				hash = HashMix(HashText(hash, name), value.Hash());
			}
			for (const PlannedInput &input : planned.Inputs) {
				hash = HashMix(HashText(hash, input.Port), input.Default.Hash());
				for (const PlannedSource &source : input.Sources) {
					hash = HashMix(HashMix(hash, plan.Nodes[source.Node].Hash), source.Output);
					hash = HashMix(hash, source.Convert ? 1 : 0);
				}
			}
			planned.Hash = hash;
			planHash = HashMix(HashText(planHash, planned.Id), hash);
		}
		plan.Hash = planHash;
		plan.Reindex();
		out = std::move(plan);
		return {.Ok = true};
	}

	TypeId PortType(
		const Graph &graph,
		const Registry &registry,
		std::string_view node,
		std::string_view port,
		Direction dir
	) {
		const NodeInstance *instance = graph.FindNode(node);
		if (instance == nullptr) {
			return {};
		}
		const PortDef *declared = DeclaredPort(registry, instance->Def, port, dir);
		if (declared == nullptr) {
			const bool known = registry.FindNode(instance->Def) != nullptr ||
							   registry.FindSubgraph(instance->Def) != nullptr;
			return known ? TypeId{} : TypeId(ANY_TYPE);
		}
		if (declared->Generic.empty()) {
			return declared->Type;
		}
		// Resolve from any link on a port of this node sharing the generic name.
		for (const Link &wire : graph.Links()) {
			const bool outgoing = wire.FromNode == node;
			const bool incoming = wire.ToNode == node;
			if (!outgoing && !incoming) {
				continue;
			}
			const PortDef *mine = DeclaredPort(
				registry,
				instance->Def,
				outgoing ? wire.FromPort : wire.ToPort,
				outgoing ? Direction::Output : Direction::Input
			);
			if (mine == nullptr || mine->Generic != declared->Generic) {
				continue;
			}
			const NodeInstance *other = graph.FindNode(outgoing ? wire.ToNode : wire.FromNode);
			if (other == nullptr) {
				continue;
			}
			const PortDef *theirs = DeclaredPort(
				registry,
				other->Def,
				outgoing ? wire.ToPort : wire.FromPort,
				outgoing ? Direction::Input : Direction::Output
			);
			if (theirs != nullptr && theirs->Generic.empty() && theirs->Type != ANY_TYPE) {
				return theirs->Type;
			}
		}
		return TypeId(ANY_TYPE);
	}

	LinkVerdict CheckNewLink(const Graph &graph, const Registry &registry, const Link &candidate) {
		const NodeInstance *from = graph.FindNode(candidate.FromNode);
		const NodeInstance *to = graph.FindNode(candidate.ToNode);
		if (from == nullptr || to == nullptr) {
			return {.Reason = "node does not exist"};
		}
		if (candidate.FromNode == candidate.ToNode) {
			return {.Reason = "a node cannot link to itself"};
		}
		const bool fromKnown =
			registry.FindNode(from->Def) != nullptr || registry.FindSubgraph(from->Def) != nullptr;
		const bool toKnown =
			registry.FindNode(to->Def) != nullptr || registry.FindSubgraph(to->Def) != nullptr;
		const PortDef *output = DeclaredPort(registry, from->Def, candidate.FromPort, Direction::Output);
		const PortDef *input = DeclaredPort(registry, to->Def, candidate.ToPort, Direction::Input);
		if ((fromKnown && output == nullptr) || (toKnown && input == nullptr)) {
			return {.Reason = "port does not exist"};
		}
		for (const Link &wire : graph.Links()) {
			if (wire == candidate) {
				return {.Reason = "link already exists"};
			}
		}
		if (input != nullptr && input->MaximumLinks != 0) {
			size_t used = 0;
			for (const Link &wire : graph.Links()) {
				used += wire.ToNode == candidate.ToNode && wire.ToPort == candidate.ToPort ? 1 : 0;
			}
			if (used >= input->MaximumLinks) {
				return {.Reason = "input '" + candidate.ToPort + "' is full"};
			}
		}
		TypeId fromType =
			PortType(graph, registry, candidate.FromNode, candidate.FromPort, Direction::Output);
		TypeId toType = PortType(graph, registry, candidate.ToNode, candidate.ToPort, Direction::Input);
		// An unresolved generic accepts what its constraints allow.
		const auto generic = [&](const PortDef *port, const TypeId &resolved, const TypeId &other) {
			if (port == nullptr || port->Generic.empty() || resolved != ANY_TYPE || other == ANY_TYPE) {
				return resolved;
			}
			const bool allowed = port->Constraints.empty() ||
								 std::find(port->Constraints.begin(), port->Constraints.end(), other) !=
									 port->Constraints.end();
			return allowed ? other : TypeId("unresolved generic");
		};
		const TypeId pinnedFrom = generic(output, fromType, toType);
		const TypeId pinnedTo = generic(input, toType, fromType);
		const LinkCheck check = registry.CheckLink(pinnedFrom, pinnedTo);
		if (!check.Ok) {
			return {.Reason = "'" + pinnedFrom + "' does not connect to '" + pinnedTo + "'"};
		}
		if (WouldCycle(graph, candidate)) {
			return {.Reason = "link would form a cycle"};
		}
		return {.Ok = true, .Converts = check.Via != nullptr};
	}

	bool WouldCycle(const Graph &graph, const Link &candidate) {
		if (candidate.FromNode == candidate.ToNode) {
			return true;
		}
		// A cycle forms if the target already reaches the source.
		std::vector<std::string_view> frontier{candidate.ToNode};
		std::unordered_set<std::string_view> seen{candidate.ToNode};
		while (!frontier.empty()) {
			const std::string_view current = frontier.back();
			frontier.pop_back();
			for (const Link &wire : graph.Links()) {
				if (wire.FromNode != current) {
					continue;
				}
				if (wire.ToNode == candidate.FromNode) {
					return true;
				}
				if (seen.insert(wire.ToNode).second) {
					frontier.push_back(wire.ToNode);
				}
			}
		}
		return false;
	}
}
