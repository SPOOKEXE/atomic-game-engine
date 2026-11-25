#include <engine/graph/PipelineGraph.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace engine::graph {

	namespace pipeline_graph_detail {
		constexpr std::string_view DEFAULT_NAME = "Default PBR";

		// A catalogue port when the document gave one, else the resource's name.
		std::string
		PortName(const RenderGraph &graph, ResourceId id, std::span<const core::Name> ports, size_t index) {
			if (index < ports.size() && ports[index].IsValid()) {
				return std::string(ports[index].Text());
			}
			return std::string(graph.FindResourceDesc(id)->Name.Text());
		}

		// Adds a port, or widens one an earlier pass of the kind used with
		// another resource kind.
		void
		AddPort(std::vector<ang::PortDef> &ports, const std::string &name, const char *type, bool input) {
			const auto found = std::find_if(ports.begin(), ports.end(), [&](const ang::PortDef &port) {
				return port.Name == name;
			});
			if (found != ports.end()) {
				if (found->Type != type) {
					found->Type = ang::ANY_TYPE;
				}
				return;
			}
			ports.push_back(
				{.Name = name,
				 .Type = type,
				 .Dir = input ? ang::Direction::Input : ang::Direction::Output,
				 .Optional = true,
				 .MaximumLinks = static_cast<uint16_t>(input ? 0 : 1)}
			);
		}
	}

	bool
	ToAngGraph(const PipelineDocument &document, ang::Registry &types, ang::Graph &out, std::string &error) {
		using namespace pipeline_graph_detail;
		RenderGraph graph;
		core::Name offender;
		if (const PipelineDocumentStatus status = Build(document, graph, offender);
			status != PipelineDocumentStatus::Ok) {
			error = std::string(Describe(status)) + ": " + std::string(offender.Text());
			return false;
		}
		struct Writer {
			std::string Node;
			std::string Port;
			size_t Depth = 0;
		};
		std::map<std::string, ang::NodeDef> defs;
		std::map<uint32_t, Writer> writers;
		std::vector<ang::NodeInstance> nodes;
		std::vector<ang::Link> links;
		std::set<std::string> linked;
		std::map<size_t, size_t> rows;
		ang::Registry registry = ang::Registry::WithScalars();
		for (uint32_t index = 1; graph.Find(NodeId{index}) != nullptr; ++index) {
			const Node *node = graph.Find(NodeId{index});
			const std::string id(node->Name.Text());
			const std::string kind(node->Kind.IsValid() ? node->Kind.Text() : node->Name.Text());
			ang::NodeDef &def = defs[kind];
			def.Id = kind;
			def.Category = "Render Pipeline";
			size_t depth = 0;
			const auto typeOf = [&](ResourceId resource) {
				const char *type = Describe(graph.FindResourceDesc(resource)->Kind);
				if (registry.FindType(type) == nullptr) {
					registry.DefineType({.Id = type});
				}
				return type;
			};
			for (size_t read = 0; read < node->Reads.size(); ++read) {
				const ResourceId resource = node->Reads[read];
				const std::string port = PortName(graph, resource, node->ReadPorts, read);
				AddPort(def.Inputs, port, typeOf(resource), true);
				const auto writer = writers.find(resource.Value);
				if (writer == writers.end()) {
					continue;
				}
				depth = std::max(depth, writer->second.Depth + 1);
				if (linked.insert(writer->second.Node + '\0' + writer->second.Port + '\0' + id + '\0' + port)
						.second) {
					links.push_back({writer->second.Node, writer->second.Port, id, port});
				}
			}
			for (size_t write = 0; write < node->Writes.size(); ++write) {
				const ResourceId resource = node->Writes[write];
				const std::string port = PortName(graph, resource, node->WritePorts, write);
				AddPort(def.Outputs, port, typeOf(resource), false);
				if (node->Enabled) {
					writers[resource.Value] = {id, port, depth};
				}
			}
			ang::NodeInstance placed{
				.Id = id,
				.Def = kind,
				.Position = {static_cast<float>(depth) * 280.0f, static_cast<float>(rows[depth]++) * 180.0f},
			};
			for (const NodeParameter &parameter : node->Parameters) {
				placed.Params[std::string(parameter.Key.Text())] = ang::Value(parameter.Value);
			}
			if (!node->Enabled) {
				placed.Params["enabled"] = ang::Value(false);
			}
			nodes.push_back(std::move(placed));
		}
		for (auto &[kind, def] : defs) {
			if (const ang::RegistryResult result = registry.DefineNode(std::move(def)); !result.Ok) {
				error = result.Error;
				return false;
			}
		}
		ang::Graph candidate;
		if (const ang::EditResult result = candidate.Load(std::move(nodes), std::move(links)); !result.Ok) {
			error = result.Error;
			return false;
		}
		types = std::move(registry);
		out = std::move(candidate);
		return true;
	}

	std::vector<std::string> PipelineGraphNames(const PipelineSet &set) {
		using namespace pipeline_graph_detail;
		std::vector<std::string> names;
		for (const core::Name name : set.Names()) {
			names.emplace_back(name.Text());
		}
		if (set.Find(core::Name(DEFAULT_NAME)) == nullptr) {
			names.emplace_back(DEFAULT_NAME);
		}
		return names;
	}

	bool OpenPipelineGraph(
		const PipelineSet &set,
		std::string_view name,
		ang::Registry &types,
		ang::Graph &out,
		std::string &error
	) {
		using namespace pipeline_graph_detail;
		// Compared as text so a script cannot intern names by guessing them.
		for (const core::Name held : set.Names()) {
			if (held.Text() == name) {
				return ToAngGraph(*set.Find(held), types, out, error);
			}
		}
		if (name == DEFAULT_NAME) {
			return ToAngGraph(DefaultPbrDocument(), types, out, error);
		}
		error = "no pipeline has that name";
		return false;
	}
}
