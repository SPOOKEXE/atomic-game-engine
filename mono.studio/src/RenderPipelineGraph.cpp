#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Types.hpp>
#include <studio/RenderPipelineGraph.hpp>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace studio {

	namespace {
		using engine::core::Name;
		using namespace engine::graph;

		constexpr std::string_view TYPE_PREFIX = "render.pass.";
		constexpr std::string_view INTERNAL_PREFIX = "__render.";

		std::string TypeId(Name kind) {
			return std::string(TYPE_PREFIX) + std::string(kind.Text());
		}

		Name KindOf(const nodegraph::Node &node) {
			if (!node.Type.starts_with(TYPE_PREFIX)) {
				return {};
			}
			return Name(std::string_view(node.Type).substr(TYPE_PREFIX.size()));
		}

		bool IsImage(ResourceKind kind) {
			return kind == ResourceKind::Colour || kind == ResourceKind::Depth ||
				   kind == ResourceKind::Texture || kind == ResourceKind::Storage;
		}

		std::string ResourceType(ResourceKind kind) {
			switch (kind) {
			case ResourceKind::Colour:
			case ResourceKind::Depth:
			case ResourceKind::Texture:
			case ResourceKind::Storage:
				return "render.image";
			case ResourceKind::Buffer:
				return "render.buffer";
			case ResourceKind::Camera:
				return "render.camera";
			case ResourceKind::Entities:
				return "render.entities";
			}
			return nodegraph::ANY_TYPE;
		}

		nodegraph::Colour Accent(NodeCategory category) {
			switch (category) {
			case NodeCategory::Draw:
				return nodegraph::Colour::Hex(0x4776A8);
			case NodeCategory::Composite:
				return nodegraph::Colour::Hex(0x8059A6);
			case NodeCategory::Interface:
				return nodegraph::Colour::Hex(0xA86F47);
			case NodeCategory::Output:
				return nodegraph::Colour::Hex(0x4A9164);
			}
			return nodegraph::Colour::Hex(0x666666);
		}

		std::string Key(std::string_view group, std::string_view name) {
			return std::string(INTERNAL_PREFIX) + std::string(group) + "." + std::string(name);
		}

		void PutText(nodegraph::Node &node, std::string key, std::string value) {
			nodegraph::Value held;
			held.Kind = nodegraph::WidgetKind::Text;
			held.Text = std::move(value);
			node.Widgets[std::move(key)] = std::move(held);
		}

		std::string TextOf(const nodegraph::Node &node, const std::string &key) {
			const auto found = node.Widgets.find(key);
			return found == node.Widgets.end() ? std::string() : found->second.Text;
		}

		void PutToggle(nodegraph::Node &node, const char *key, bool value) {
			nodegraph::Value held;
			held.Kind = nodegraph::WidgetKind::Toggle;
			held.Flag = value;
			node.Widgets[key] = held;
		}

		bool ToggleOf(const nodegraph::Node &node, const char *key, bool fallback) {
			const auto found = node.Widgets.find(key);
			return found == node.Widgets.end() ? fallback : found->second.Flag;
		}

		void PutSelect(nodegraph::Node &node, const char *key, std::string value) {
			nodegraph::Value held;
			held.Kind = nodegraph::WidgetKind::Select;
			held.Text = std::move(value);
			node.Widgets[key] = std::move(held);
		}

		std::string SelectOf(const nodegraph::Node &node, const char *key, std::string fallback) {
			const auto found = node.Widgets.find(key);
			return found == node.Widgets.end() || found->second.Text.empty() ? fallback : found->second.Text;
		}

		void PutNumber(nodegraph::Node &node, const char *key, uint32_t value) {
			nodegraph::Value held;
			held.Kind = nodegraph::WidgetKind::Number;
			held.Number = static_cast<double>(value);
			node.Widgets[key] = held;
		}

		std::string NumberText(const nodegraph::Node &node, const char *key, uint32_t minimum = 1) {
			const auto found = node.Widgets.find(key);
			const uint32_t value =
				found == node.Widgets.end()
					? minimum
					: static_cast<uint32_t>(std::max(found->second.Number, static_cast<double>(minimum)));
			return std::to_string(value);
		}

		uint32_t NumberOf(const nodegraph::Node &node, const std::string &key, uint32_t fallback) {
			const auto found = node.Widgets.find(key);
			return found == node.Widgets.end() ? fallback
											   : static_cast<uint32_t>(std::max(found->second.Number, 1.0));
		}

		std::string ResourceKey(std::string_view setting, std::string_view port) {
			return "resource." + std::string(port) + "." + std::string(setting);
		}

		struct Binding {
			std::string Port;
			Name Resource;
		};

		struct AuthoredNode {
			Name Name_;
			Name Kind;
			NodeScope Scope = NodeScope::View;
			bool Optional = false;
			bool Enabled = true;
			float X = 0.0f;
			float Y = 0.0f;
			bool Moved = false;
			std::vector<Binding> Reads;
			std::vector<Binding> Writes;
			std::vector<NodeParameter> Parameters;
		};

		std::vector<AuthoredNode> AuthoredNodes(const PipelineDocument &document) {
			std::vector<AuthoredNode> nodes;
			AuthoredNode *current = nullptr;
			for (const Edit &edit : document.Edits()) {
				switch (edit.Kind) {
				case EditKind::AddNode:
					nodes.push_back(AuthoredNode{});
					nodes.back().Name_ = edit.Name;
					nodes.back().Kind = edit.NodeKind;
					nodes.back().Scope = edit.Scope;
					nodes.back().Optional = edit.Optional;
					current = &nodes.back();
					break;
				case EditKind::Reads:
				case EditKind::Writes:
					if (current != nullptr) {
						auto &bindings = edit.Kind == EditKind::Reads ? current->Reads : current->Writes;
						bindings.push_back({std::string(edit.Key.Text()), edit.Target});
					}
					break;
				case EditKind::Set:
					if (current != nullptr) {
						current->Parameters.push_back({edit.Key, edit.Value});
					}
					break;
				case EditKind::Enable:
					for (AuthoredNode &node : nodes) {
						if (node.Name_ == edit.Name) {
							node.Enabled = edit.Enabled;
						}
					}
					current = nullptr;
					break;
				case EditKind::Move:
					for (AuthoredNode &node : nodes) {
						if (node.Name_ == edit.Name) {
							node.X = edit.X;
							node.Y = edit.Y;
							node.Moved = true;
						}
					}
					break;
				case EditKind::AddResource:
					current = nullptr;
					break;
				}
			}
			return nodes;
		}

		std::string BindingPort(const Binding &binding, const std::vector<PortSpec> &ports, size_t index) {
			if (!binding.Port.empty()) {
				return binding.Port;
			}
			return index < ports.size() ? std::string(ports[index].Name.Text()) : std::string();
		}

		const PortSpec *PortNamed(const std::vector<PortSpec> &ports, std::string_view name) {
			for (const PortSpec &port : ports) {
				if (port.Name.Text() == name) {
					return &port;
				}
			}
			return nullptr;
		}

		Name UniqueNodeName(const nodegraph::Node &node, Name kind, std::unordered_set<uint32_t> &used) {
			const std::string base = node.Label.empty() ? std::string(kind.Text()) : node.Label;
			Name candidate(base);
			uint32_t suffix = 2;
			while (!used.insert(candidate.Id()).second) {
				candidate = Name(base + "." + std::to_string(suffix++));
			}
			return candidate;
		}
	}

	void RegisterRenderPipelineNodeTypes() {
		using namespace engine::graph;
		RegisterRenderNodeKinds();

		for (const auto &[id, label, tint, description] : std::array{
				 std::tuple{
					 "render.image",
					 "IMAGE",
					 0xD07852u,
					 "A sampled, colour, depth, or compute image."
				 },
				 std::tuple{"render.buffer", "Buffer", 0xB39A58u, "A structured GPU buffer."},
				 std::tuple{"render.camera", "Camera", 0x53A7A0u, "A viewpoint and projection."},
				 std::tuple{"render.entities", "Entities", 0x73A856u, "A filtered ordered draw list."},
			 }) {
			nodegraph::DataType type;
			type.Id = id;
			type.Label = label;
			type.Tint = nodegraph::Colour::Hex(tint);
			type.Description = description;
			nodegraph::DataTypes::Register(type);
		}

		for (const NodeKindSpec &spec : NodeCatalogue::All()) {
			nodegraph::NodeType type;
			type.Id = TypeId(spec.Kind);
			type.Title = spec.Label.empty() ? std::string(spec.Kind.Text()) : spec.Label;
			type.Category = Describe(spec.Category);
			type.Accent = Accent(spec.Category);
			type.Subtitle = spec.Summary;
			for (const PortSpec &port : spec.Inputs) {
				type.Inputs.push_back(
					nodegraph::Port(std::string(port.Name.Text()), ResourceType(port.Kind))
				);
			}
			for (const PortSpec &port : spec.Outputs) {
				type.Outputs.push_back(
					nodegraph::Port(std::string(port.Name.Text()), ResourceType(port.Kind))
				);
			}
			type.Evaluate = [outputs = type.Outputs](const nodegraph::Inputs &) {
				nodegraph::Outputs made;
				for (const nodegraph::PortSpec &output : outputs) {
					made.emplace(output.Name, uint8_t{0});
				}
				return made;
			};
			for (const PortSpec &port : spec.Outputs) {
				if (IsImage(port.Kind)) {
					type.PreviewPort = std::string(port.Name.Text());
					type.Preview = [](const std::any &, nodegraph::PreviewImage &) { return false; };
					break;
				}
			}
			type.Widgets = {
				nodegraph::Toggle("enabled", "Enabled", true),
				nodegraph::Toggle("optional", "Optional", false),
				nodegraph::Select(
					"scope",
					"Scope",
					{"world", "view", "frame"},
					spec.Scope == NodeScope::World	? 0
					: spec.Scope == NodeScope::View ? 1
													: 2
				),
				nodegraph::Select("queue", "Queue", {"auto", "cpu", "graphics", "compute", "transfer"}, 0),
				nodegraph::Select("async", "Async", {"auto", "allow", "serial"}, 0),
			};
			if (spec.Kind == Name("cull-frustum")) {
				type.Widgets.push_back(
					nodegraph::Select("culling", "Culling", {"inherit", "none", "frustum"}, 0)
				);
			}
			if (spec.Kind == Name("cull-distance")) {
				type.Widgets.push_back(nodegraph::Text("radius", "Radius", "0"));
			}
			if (spec.Kind == Name("filter-tag")) {
				type.Widgets.push_back(nodegraph::Text("mask", "Tag mask", "0"));
			}
			if (spec.Kind == Name("raster") || spec.Kind == Name("dispatch")) {
				type.Widgets.push_back(nodegraph::Text("shader", "Shader", ""));
				type.Widgets.push_back(nodegraph::Text("source", "GLSL source", ""));
			}
			if (spec.Kind == Name("raster")) {
				type.Widgets.push_back(nodegraph::Select("load", "Load", {"clear", "load"}, 0));
			}
			if (spec.Kind == Name("dispatch")) {
				type.Widgets.push_back(
					nodegraph::Select("dispatch.mode", "Dispatch", {"target", "groups"}, 0)
				);
				type.Widgets.push_back(nodegraph::Number("local.x", "Threads X", 8));
				type.Widgets.push_back(nodegraph::Number("local.y", "Threads Y", 8));
				type.Widgets.push_back(nodegraph::Number("local.z", "Threads Z", 1));
				type.Widgets.push_back(nodegraph::Number("dispatch.x", "Groups X", 1));
				type.Widgets.push_back(nodegraph::Number("dispatch.y", "Groups Y", 1));
				type.Widgets.push_back(nodegraph::Number("dispatch.z", "Groups Z", 1));
			}
			if (spec.Kind == Name("viewer") || spec.Kind == Name("capture")) {
				type.Widgets.push_back(nodegraph::Number("view", "Viewport slot", 0));
			}
			if (spec.Kind == Name("capture")) {
				type.Widgets.push_back(nodegraph::Text("path", "BMP path", ""));
				type.Widgets.push_back(
					nodegraph::Select("capture.mode", "Capture", {"once", "every-frame"}, 0)
				);
			}
			for (const PortSpec &port : spec.Outputs) {
				const std::string output(port.Name.Text());
				type.Widgets.push_back(
					nodegraph::Select(
						ResourceKey("lifetime", output), output + " lifetime", {"transient", "external"}, 0
					)
				);
				type.Widgets.push_back(
					nodegraph::Select(
						ResourceKey("resolution", output),
						output + " resolution",
						{"full", "half", "quarter", "eighth", "fixed"},
						0
					)
				);
				type.Widgets.push_back(
					nodegraph::Number(ResourceKey("width", output), output + " width", 1920)
				);
				type.Widgets.push_back(
					nodegraph::Number(ResourceKey("height", output), output + " height", 1080)
				);
			}
			nodegraph::NodeTypes::Register(type);
		}
	}

	bool
	LoadRenderPipelineGraph(const PipelineDocument &document, nodegraph::Graph &graph, std::string &error) {
		using namespace engine::graph;
		RegisterRenderPipelineNodeTypes();
		graph.Clear();
		error.clear();

		const std::vector<AuthoredNode> authored = AuthoredNodes(document);
		std::unordered_map<uint32_t, Edit> resourceSettings;
		for (const Edit &edit : document.Edits()) {
			if (edit.Kind == EditKind::AddResource) {
				resourceSettings[edit.Name.Id()] = edit;
			}
		}
		std::vector<nodegraph::NodeId> ids;
		ids.reserve(authored.size());
		for (size_t index = 0; index < authored.size(); index++) {
			const AuthoredNode &source = authored[index];
			const float x = source.Moved ? source.X : static_cast<float>(index) * 230.0f;
			const float y = source.Moved					   ? source.Y
							: source.Scope == NodeScope::World ? 0.0f
							: source.Scope == NodeScope::View  ? 180.0f
															   : 360.0f;
			const nodegraph::NodeId id = graph.Add(TypeId(source.Kind), x, y);
			if (id == nodegraph::NO_NODE) {
				error = "no render node type is registered for " + std::string(source.Kind.Text());
				graph.Clear();
				return false;
			}
			nodegraph::Node &node = *graph.Find(id);
			node.Label = std::string(source.Name_.Text());
			PutToggle(node, "enabled", source.Enabled);
			PutToggle(node, "optional", source.Optional);
			PutSelect(node, "scope", Describe(source.Scope));
			for (const NodeParameter &parameter : source.Parameters) {
				const std::string key(parameter.Key.Text());
				if (key == "queue" || key == "async" || key == "load" || key == "dispatch.mode" ||
					key == "capture.mode") {
					PutSelect(node, key.c_str(), parameter.Value);
				} else if (key == "culling" && node.Widgets.contains("culling")) {
					PutSelect(node, key.c_str(), parameter.Value);
				} else if (key == "dispatch.x" || key == "dispatch.y" || key == "dispatch.z" ||
						   key == "local.x" || key == "local.y" || key == "local.z" || key == "view") {
					uint32_t number = 1;
					std::from_chars(
						parameter.Value.data(), parameter.Value.data() + parameter.Value.size(), number
					);
					PutNumber(node, key.c_str(), number);
				} else if (key == "shader" || key == "source" || key == "path" ||
						   (key == "radius" && source.Kind == Name("cull-distance")) ||
						   (key == "mask" && source.Kind == Name("filter-tag"))) {
					PutText(node, key, parameter.Value);
				} else {
					PutText(node, Key("parameter", key), parameter.Value);
				}
			}

			const NodeKindSpec *spec = NodeCatalogue::Find(source.Kind);
			for (size_t index = 0; spec != nullptr && index < source.Reads.size(); index++) {
				const std::string port = BindingPort(source.Reads[index], spec->Inputs, index);
				PutText(node, Key("read", port), std::string(source.Reads[index].Resource.Text()));
			}
			for (size_t index = 0; spec != nullptr && index < source.Writes.size(); index++) {
				const std::string port = BindingPort(source.Writes[index], spec->Outputs, index);
				PutText(node, Key("write", port), std::string(source.Writes[index].Resource.Text()));
				const auto found = resourceSettings.find(source.Writes[index].Resource.Id());
				if (found != resourceSettings.end()) {
					const Edit &resource = found->second;
					PutSelect(
						node,
						ResourceKey("lifetime", port).c_str(),
						resource.External ? "external" : "transient"
					);
					const std::string resolution = resource.Width > 0 && resource.Height > 0 ? "fixed"
												   : resource.Divisor >= 8					 ? "eighth"
												   : resource.Divisor >= 4					 ? "quarter"
												   : resource.Divisor >= 2					 ? "half"
																							 : "full";
					PutSelect(node, ResourceKey("resolution", port).c_str(), resolution);
					PutNumber(
						node, ResourceKey("width", port).c_str(), resource.Width > 0 ? resource.Width : 1920
					);
					PutNumber(
						node,
						ResourceKey("height", port).c_str(),
						resource.Height > 0 ? resource.Height : 1080
					);
				}
			}
			ids.push_back(id);
		}

		struct Writer {
			nodegraph::NodeId Node = nodegraph::NO_NODE;
			std::string Port;
			size_t Index = 0;
		};
		std::unordered_map<uint32_t, std::vector<Writer>> writers;
		for (size_t index = 0; index < authored.size(); index++) {
			const NodeKindSpec *spec = NodeCatalogue::Find(authored[index].Kind);
			for (size_t slot = 0; spec != nullptr && slot < authored[index].Writes.size(); slot++) {
				writers[authored[index].Writes[slot].Resource.Id()].push_back(
					{ids[index], BindingPort(authored[index].Writes[slot], spec->Outputs, slot), index}
				);
			}
		}

		for (size_t index = 0; index < authored.size(); index++) {
			const NodeKindSpec *spec = NodeCatalogue::Find(authored[index].Kind);
			for (size_t slot = 0; spec != nullptr && slot < authored[index].Reads.size(); slot++) {
				const Binding &read = authored[index].Reads[slot];
				const std::string input = BindingPort(read, spec->Inputs, slot);
				const auto found = writers.find(read.Resource.Id());
				if (found == writers.end()) {
					PutText(*graph.Find(ids[index]), Key("external", input), "yes");
					continue;
				}

				const Writer *producer = nullptr;
				for (const Writer &candidate : found->second) {
					if (candidate.Index < index) {
						producer = &candidate;
					}
				}
				if (producer == nullptr && found->second.size() == 1) {
					producer = &found->second.front();
				}
				if (producer != nullptr && !producer->Port.empty() && !input.empty()) {
					const nodegraph::LinkResult linked =
						graph.Connect(producer->Node, producer->Port, ids[index], input);
					if (linked != nodegraph::LinkResult::Made) {
						error = "could not restore " + std::string(read.Resource.Text()) + ": " +
								nodegraph::Describe(linked);
						graph.Clear();
						return false;
					}
				}
			}
		}
		return true;
	}

	bool SaveRenderPipelineGraph(
		const nodegraph::Graph &graph,
		const PipelineDocument &basis,
		PipelineDocument &document,
		std::string &error
	) {
		using namespace engine::graph;
		document.Clear();
		error.clear();
		RegisterRenderPipelineNodeTypes();

		std::unordered_map<uint32_t, Edit> resources;
		std::vector<Name> resourceOrder;
		for (const Edit &edit : basis.Edits()) {
			if (edit.Kind == EditKind::AddResource && !resources.contains(edit.Name.Id())) {
				resources.emplace(edit.Name.Id(), edit);
				resourceOrder.push_back(edit.Name);
			}
		}

		std::unordered_map<nodegraph::NodeId, Name> names;
		std::unordered_set<uint32_t> usedNames;
		for (const nodegraph::Node &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			if (!kind.IsValid()) {
				error = "the canvas contains a non-render node";
				return false;
			}
			names[node.Id] = UniqueNodeName(node, kind, usedNames);
		}

		for (const nodegraph::Link &link : graph.Links()) {
			const nodegraph::Node *from = graph.Find(link.From);
			const nodegraph::Node *to = graph.Find(link.To);
			if (from == nullptr || to == nullptr) {
				error = "a render link names a node that no longer exists";
				return false;
			}
			const NodeKindSpec *fromKind = NodeCatalogue::Find(KindOf(*from));
			const NodeKindSpec *toKind = NodeCatalogue::Find(KindOf(*to));
			const PortSpec *fromPort =
				fromKind == nullptr ? nullptr : PortNamed(fromKind->Outputs, link.FromPort);
			const PortSpec *toPort = toKind == nullptr ? nullptr : PortNamed(toKind->Inputs, link.ToPort);
			if (fromPort == nullptr || toPort == nullptr) {
				error = "a render link names a socket the pass no longer declares";
				return false;
			}
			if (!PortsCompatible(fromPort->Kind, toPort->Kind)) {
				error = std::string(WhyIncompatible(fromPort->Kind, toPort->Kind));
				return false;
			}
		}

		const auto ensureResource = [&](Name name, const PortSpec &port) {
			if (resources.contains(name.Id())) {
				return;
			}
			Edit resource;
			resource.Kind = EditKind::AddResource;
			resource.Name = name;
			resource.Resource = port.Kind;
			resource.Format = port.Format;
			resources.emplace(name.Id(), resource);
			resourceOrder.push_back(name);
		};

		for (const nodegraph::Node &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			const NodeKindSpec *spec = NodeCatalogue::Find(kind);
			if (spec == nullptr) {
				error = "the render catalogue no longer contains " + std::string(kind.Text());
				return false;
			}
			for (const PortSpec &port : spec->Outputs) {
				std::string resource = TextOf(node, Key("write", port.Name.Text()));
				if (resource.empty()) {
					resource = std::string(names[node.Id].Text()) + "." + std::string(port.Name.Text());
				}
				ensureResource(Name(resource), port);
				Edit &settings = resources.at(Name(resource).Id());
				const std::string output(port.Name.Text());
				settings.External =
					SelectOf(node, ResourceKey("lifetime", output).c_str(), "transient") == "external";
				const std::string resolution =
					SelectOf(node, ResourceKey("resolution", output).c_str(), "full");
				settings.Divisor = resolution == "eighth"	 ? 8
								   : resolution == "quarter" ? 4
								   : resolution == "half"	 ? 2
															 : 1;
				if (resolution == "fixed") {
					settings.Width = NumberOf(node, ResourceKey("width", output), 1920);
					settings.Height = NumberOf(node, ResourceKey("height", output), 1080);
				} else {
					settings.Width = 0;
					settings.Height = 0;
				}
			}
		}

		for (const Name resource : resourceOrder) {
			document.Record(resources.at(resource.Id()));
		}

		std::vector<Edit> enables;
		std::vector<Edit> moves;
		for (const nodegraph::Node &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			const NodeKindSpec &spec = *NodeCatalogue::Find(kind);
			const Name name = names.at(node.Id);

			Edit add;
			add.Kind = EditKind::AddNode;
			add.Name = name;
			add.NodeKind = kind;
			const std::string scope = SelectOf(node, "scope", Describe(spec.Scope));
			add.Scope = scope == "world"   ? NodeScope::World
						: scope == "frame" ? NodeScope::Frame
										   : NodeScope::View;
			add.Optional = ToggleOf(node, "optional", false);
			document.Record(add);

			for (const PortSpec &port : spec.Inputs) {
				Name target;
				if (const nodegraph::Link *link = graph.LinkInto(node.Id, std::string(port.Name.Text()));
					link != nullptr) {
					const nodegraph::Node *producer = graph.Find(link->From);
					if (producer != nullptr) {
						std::string resource = TextOf(*producer, Key("write", link->FromPort));
						if (resource.empty()) {
							resource = std::string(names.at(producer->Id).Text()) + "." + link->FromPort;
						}
						target = Name(resource);
					}
				} else if (TextOf(node, Key("external", port.Name.Text())) == "yes") {
					target = Name(TextOf(node, Key("read", port.Name.Text())));
				}
				if (target.IsValid()) {
					Edit read;
					read.Kind = EditKind::Reads;
					read.Key = port.Name;
					read.Target = target;
					document.Record(read);
				}
			}

			for (const PortSpec &port : spec.Outputs) {
				std::string resource = TextOf(node, Key("write", port.Name.Text()));
				if (resource.empty()) {
					resource = std::string(name.Text()) + "." + std::string(port.Name.Text());
				}
				Edit write;
				write.Kind = EditKind::Writes;
				write.Key = port.Name;
				write.Target = Name(resource);
				document.Record(write);
			}

			for (const auto &[key, value] : node.Widgets) {
				if (!key.starts_with(std::string(INTERNAL_PREFIX) + "parameter.")) {
					continue;
				}
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name(std::string_view(key).substr(INTERNAL_PREFIX.size() + 10));
				set.Value = value.Text;
				document.Record(set);
			}
			for (const char *key : {"queue", "async"}) {
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name(key);
				set.Value = SelectOf(node, key, "auto");
				document.Record(set);
			}
			if (node.Widgets.contains("culling")) {
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name("culling");
				set.Value = SelectOf(node, "culling", "inherit");
				document.Record(set);
			}
			if (kind == Name("cull-distance") || kind == Name("filter-tag")) {
				const char *key = kind == Name("cull-distance") ? "radius" : "mask";
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name(key);
				set.Value = TextOf(node, key);
				document.Record(set);
			}
			if (kind == Name("raster") || kind == Name("dispatch")) {
				for (const char *key : {"shader", "source"}) {
					Edit set;
					set.Kind = EditKind::Set;
					set.Key = Name(key);
					set.Value = TextOf(node, key);
					document.Record(set);
				}
			}
			if (kind == Name("raster")) {
				Edit load;
				load.Kind = EditKind::Set;
				load.Key = Name("load");
				load.Value = SelectOf(node, "load", "clear");
				document.Record(load);
			}
			if (kind == Name("dispatch")) {
				Edit mode;
				mode.Kind = EditKind::Set;
				mode.Key = Name("dispatch.mode");
				mode.Value = SelectOf(node, "dispatch.mode", "target");
				document.Record(mode);
				for (const char *key :
					 {"local.x", "local.y", "local.z", "dispatch.x", "dispatch.y", "dispatch.z"}) {
					Edit set;
					set.Kind = EditKind::Set;
					set.Key = Name(key);
					set.Value = NumberText(node, key);
					document.Record(set);
				}
			}
			if (kind == Name("viewer") || kind == Name("capture")) {
				Edit view;
				view.Kind = EditKind::Set;
				view.Key = Name("view");
				view.Value = NumberText(node, "view", 0);
				document.Record(view);
			}
			if (kind == Name("capture")) {
				Edit path;
				path.Kind = EditKind::Set;
				path.Key = Name("path");
				path.Value = TextOf(node, "path");
				document.Record(path);
				Edit mode;
				mode.Kind = EditKind::Set;
				mode.Key = Name("capture.mode");
				mode.Value = SelectOf(node, "capture.mode", "once");
				document.Record(mode);
			}

			Edit enable;
			enable.Kind = EditKind::Enable;
			enable.Name = name;
			enable.Enabled = ToggleOf(node, "enabled", true);
			enables.push_back(enable);

			Edit move;
			move.Kind = EditKind::Move;
			move.Name = name;
			move.X = node.X;
			move.Y = node.Y;
			moves.push_back(move);
		}

		for (Edit &edit : enables) {
			document.Record(std::move(edit));
		}
		for (Edit &edit : moves) {
			document.Record(std::move(edit));
		}

		RenderGraph built;
		Name offender;
		const PipelineDocumentStatus builtStatus = Build(document, built, offender);
		if (builtStatus != PipelineDocumentStatus::Ok) {
			error = std::string(Describe(builtStatus)) + ": " + std::string(offender.Text());
			return false;
		}

		ExecutionSchedule schedule;
		const ScheduleStatus scheduled = CompileSchedule(built, schedule, offender);
		if (scheduled != ScheduleStatus::Ok) {
			error = std::string(Describe(scheduled)) + ": " + std::string(offender.Text());
			return false;
		}
		return true;
	}
}
