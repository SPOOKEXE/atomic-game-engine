#include "ImageComposerGraph.hpp"

#include "ImageComposer.hpp"

#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <nodegraph/Layout.hpp>
#include <nodegraph/Registry.hpp>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
		constexpr const char *IDENTITY = "__composer.id";

		nodegraph::Value Value(const nodegraph::Node &node, const std::string &key) {
			if (const auto held = node.Widgets.find(key); held != node.Widgets.end()) return held->second;
			if (const auto *type = nodegraph::NodeTypes::Find(node.Type))
				for (const auto &spec : type->Widgets)
					if (spec.Key == key) return spec.Default;
			return {};
		}
		std::string Id(const nodegraph::Node &node) {
			const auto held = node.Widgets.find(IDENTITY);
			return held == node.Widgets.end() ? "node-" + std::to_string(node.Id) : held->second.Text;
		}
		std::unordered_map<nodegraph::NodeId, std::string>
		Identities(const nodegraph::Graph &graph, std::span<const Output> outputs) {
			std::unordered_map<nodegraph::NodeId, std::string> names;
			std::unordered_set<std::string> used;
			// Copied widgets carry their source identity. Preserve the first owner and name each copy anew.
			for (const auto &node : graph.Nodes()) {
				const auto identity = node.Widgets.find(IDENTITY);
				if (identity != node.Widgets.end() && used.insert(identity->second.Text).second)
					names.emplace(node.Id, identity->second.Text);
			}
			// A deleted node's output remains dangling rather than silently binding a later node.
			for (const auto &output : outputs)
				used.insert(output.Node);
			for (const auto &node : graph.Nodes()) {
				if (names.contains(node.Id)) continue;
				std::string name = "node-" + std::to_string(node.Id);
				while (!used.insert(name).second)
					name += "-copy";
				names.emplace(node.Id, std::move(name));
			}
			return names;
		}

		void Text(nodegraph::Node &node, const char *key, std::string text) {
			auto &value = node.Widgets[key];
			value.Kind = nodegraph::WidgetKind::Text;
			value.Text = std::move(text);
		}
		// Real numbers in [0, 1] get a slider; every other number is typed.
		bool Slides(const PropertySpec &spec) {
			return spec.Kind == PropertyKind::Number && spec.Minimum == 0 && spec.Maximum == 1;
		}
		std::string Label(std::string_view key) {
			std::string label;
			bool start = true;
			for (char character : key) {
				if (character == '_') {
					label += ' ';
					start = true;
					continue;
				}
				label += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
							   : character;
				start = false;
			}
			return label;
		}
		// Widget value for one property, in the shape the composer's widget kinds hold.
		nodegraph::Value Widget(const PropertySpec &spec, const InputValue &input) {
			nodegraph::Value value;
			switch (spec.Kind) {
			case PropertyKind::Dimension:
			case PropertyKind::Count:
			case PropertyKind::Offset:
			case PropertyKind::Number:
				value.Kind = Slides(spec) ? nodegraph::WidgetKind::Slider : nodegraph::WidgetKind::Number;
				value.Number = std::get<double>(input);
				break;
			case PropertyKind::Flag:
				value.Kind = nodegraph::WidgetKind::Toggle;
				value.Flag = std::get<bool>(input);
				break;
			case PropertyKind::Choice: {
				value.Kind = nodegraph::WidgetKind::Select;
				value.Text = std::get<std::string>(input);
				const auto found = std::find(spec.Choices.begin(), spec.Choices.end(), value.Text);
				value.Number = static_cast<double>(found - spec.Choices.begin());
				break;
			}
			case PropertyKind::Colour: {
				const auto &colour = std::get<std::array<uint8_t, 4>>(input);
				value.Kind = nodegraph::WidgetKind::Colour;
				value.Tint = {colour[0] / 255.f, colour[1] / 255.f, colour[2] / 255.f, colour[3] / 255.f};
				break;
			}
			default:
				value.Kind = nodegraph::WidgetKind::Text;
				value.Text = std::get<std::string>(input);
				break;
			}
			return value;
		}
		void Controls(nodegraph::Node &node, const Operation &operation) {
			for (const auto &spec : DescribeKind(Kind(operation))) {
				InputValue input;
				if (GetProperty(operation, spec.Key, input)) node.Widgets[spec.Key] = Widget(spec, input);
			}
		}
		bool ReadOperation(const nodegraph::Node &node, Operation &out, Diagnostic &diagnostic) {
			const auto *type = nodegraph::NodeTypes::Find(node.Type);
			const auto fail = [&](std::string why) {
				diagnostic = {Id(node), std::move(why)};
				return false;
			};
			Operation operation;
			if (type == nullptr || type->Category != "Image Composer" ||
				!DefaultOperation(node.Type, operation))
				return fail("unsupported composer node");
			for (const auto &[key, value] : node.Widgets) {
				if (key == IDENTITY) {
					if (value.Kind != nodegraph::WidgetKind::Text)
						return fail("invalid node control identity");
					continue;
				}
				const auto spec = std::find_if(
					type->Widgets.begin(), type->Widgets.end(), [&widgetKey = key](const auto &entry) {
						return entry.Key == widgetKey;
					}
				);
				if (spec == type->Widgets.end() || value.Kind != spec->Kind)
					return fail("unsupported node control " + key);
			}
			for (const auto &spec : DescribeKind(node.Type)) {
				const auto value = Value(node, spec.Key);
				InputValue input;
				switch (value.Kind) {
				case nodegraph::WidgetKind::Number:
				case nodegraph::WidgetKind::Slider:
					if (!std::isfinite(value.Number)) return fail("non-finite node control " + spec.Key);
					input = value.Number;
					break;
				case nodegraph::WidgetKind::Toggle:
					input = value.Flag;
					break;
				case nodegraph::WidgetKind::Colour: {
					std::array<uint8_t, 4> colour{};
					const std::array<float, 4> rgba{value.Tint.R, value.Tint.G, value.Tint.B, value.Tint.A};
					for (size_t channel = 0; channel < rgba.size(); ++channel) {
						if (!std::isfinite(rgba[channel]) || rgba[channel] < 0 || rgba[channel] > 1)
							return fail("colour must be within 0..1");
						colour[channel] = static_cast<uint8_t>(std::lround(rgba[channel] * 255.f));
					}
					input = colour;
					break;
				}
				default:
					input = value.Text;
					break;
				}
				if (!SetProperty(operation, spec.Key, input)) return fail("invalid node control " + spec.Key);
			}
			out = std::move(operation);
			return true;
		}
	}

	void AssignImageComposerIdentities(
		nodegraph::Graph &graph, std::span<const engine::imagegraph::Output> outputs
	) {
		const auto names = Identities(graph, outputs);
		for (auto &node : graph.Nodes())
			Text(node, IDENTITY, names.at(node.Id));
	}

	// Every image kind becomes a canvas node whose widgets are its described properties.
	void RegisterImageComposerNodeTypes() {
		if (nodegraph::NodeTypes::Find("image.source") != nullptr) return;
		nodegraph::DataType image;
		image.Id = std::string(IMAGE_PORT_TYPE);
		image.Label = "Image";
		image.Tint = {1, 1, 1, 1};
		nodegraph::DataTypes::Register(image);
		for (const auto kind : NODE_KIND_IDS) {
			const auto *schema = FindNodeSchema(kind);
			Operation defaults;
			if (schema == nullptr || !DefaultOperation(kind, defaults)) continue;
			nodegraph::NodeType type;
			type.Id = kind;
			type.Title = Label(kind.substr(std::string_view("image.").size()));
			type.Category = "Image Composer";
			type.Accent = {1, 1, 1, 1};
			type.Width = 200;
			for (const auto &port : schema->Ports) {
				auto &ports =
					port.Direction == engine::graphcore::PortDirection::Input ? type.Inputs : type.Outputs;
				ports.push_back(nodegraph::Port(port.Id, port.ValueType));
			}
			for (const auto &spec : DescribeKind(kind)) {
				InputValue input;
				if (!GetProperty(defaults, spec.Key, input)) continue;
				nodegraph::Value value = Widget(spec, input);
				// A placed node starts at a useful size and with a nameable source.
				if (spec.Kind == PropertyKind::Dimension) value.Number = 256;
				if (spec.Key == "path" && value.Text.empty()) value.Text = "source.png";
				nodegraph::WidgetSpec widget;
				widget.Key = spec.Key;
				widget.Label = Label(spec.Key);
				widget.Kind = value.Kind;
				if (Slides(spec)) {
					widget.Minimum = spec.Minimum;
					widget.Maximum = spec.Maximum;
				}
				widget.Options = spec.Choices;
				widget.Default = value;
				type.Widgets.push_back(std::move(widget));
			}
			nodegraph::NodeTypes::Register(type);
		}
	}

	bool LoadImageComposerGraph(
		const engine::imagegraph::Document &document,
		nodegraph::Graph &out,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		Plan checked;
		if (!Compile(document, checked, diagnostic)) return false;
		RegisterImageComposerNodeTypes();
		nodegraph::Graph candidate;
		std::unordered_map<std::string, nodegraph::NodeId> ids;
		for (const auto &node : document.Nodes) {
			if (std::abs(node.Position[0]) > std::numeric_limits<float>::max() ||
				std::abs(node.Position[1]) > std::numeric_limits<float>::max()) {
				diagnostic = {node.Id, "canvas position exceeds its numeric range"};
				return false;
			}
			const auto id = candidate.Add(
				std::string(Kind(node.Value)),
				static_cast<float>(node.Position[0]),
				static_cast<float>(node.Position[1])
			);
			if (id == nodegraph::NO_NODE) {
				diagnostic = {node.Id, "composer node type is unavailable"};
				return false;
			}
			ids.emplace(node.Id, id);
			Controls(*candidate.Find(id), node.Value);
			Text(*candidate.Find(id), IDENTITY, node.Id);
		}
		for (const auto &node : document.Nodes) {
			const auto *type = nodegraph::NodeTypes::Find(std::string(Kind(node.Value)));
			for (size_t input = 0; input < node.Inputs.size(); ++input)
				if (candidate.Connect(
						ids.at(node.Inputs[input]), "Image", ids.at(node.Id), type->Inputs[input].Name
					) != nodegraph::LinkResult::Made) {
					diagnostic = {node.Id, "composer link was refused"};
					return false;
				}
		}
		out = std::move(candidate);
		return true;
	}
	bool SaveImageComposerGraph(
		const nodegraph::Graph &graph,
		std::span<const engine::imagegraph::Output> outputs,
		engine::imagegraph::Document &out,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		Document candidate;
		candidate.Outputs.assign(outputs.begin(), outputs.end());
		if (graph.Nodes().size() > Limits::MaximumNodes) {
			diagnostic = {{}, "composer node budget exceeded"};
			return false;
		}
		for (const auto &placed : graph.Nodes()) {
			Node node;
			node.Id = Id(placed);
			node.Position = {placed.X, placed.Y};
			if (!ReadOperation(placed, node.Value, diagnostic)) return false;
			const auto *type = nodegraph::NodeTypes::Find(placed.Type);
			size_t required = 0, accepted = 0;
			(void)InputRange(placed.Type, required, accepted);
			bool skipped = false;
			for (size_t index = 0; index < type->Inputs.size(); ++index) {
				const auto &port = type->Inputs[index];
				const auto *link = graph.LinkInto(placed.Id, port.Name);
				if (link == nullptr) {
					if (index < required) {
						diagnostic = {node.Id, "connect " + port.Name};
						return false;
					}
					skipped = true;
					continue;
				}
				// Optional inputs are positional, so a later one needs every earlier one.
				if (skipped) {
					diagnostic = {node.Id, "connect the optional inputs before " + port.Name};
					return false;
				}
				const auto *source = graph.Find(link->From);
				if (source == nullptr || link->FromPort != "Image") {
					diagnostic = {node.Id, "invalid image link"};
					return false;
				}
				node.Inputs.push_back(Id(*source));
			}
			candidate.Nodes.push_back(std::move(node));
		}
		Plan checked;
		if (!Compile(candidate, checked, diagnostic)) return false;
		out = std::move(candidate);
		return true;
	}
	bool SaveImageComposerDocument(const ImageComposerState &state, Document &out, Diagnostic &diagnostic) {
		Document candidate;
		if (!SaveImageComposerGraph(state.Graph, state.Outputs, candidate, diagnostic)) return false;
		candidate.Parameters = state.Parameters;
		candidate.Bindings = state.Bindings;
		candidate.Storage = state.Storage;
		Plan plan;
		if (!Compile(candidate, plan, diagnostic)) return false;
		out = std::move(candidate);
		return true;
	}
	bool BindImageComposerInput(
		ImageComposerState &state,
		nodegraph::NodeId id,
		std::string_view property,
		std::string_view input,
		Diagnostic &diagnostic
	) {
		AssignImageComposerIdentities(state.Graph, state.Outputs);
		const auto *node = state.Graph.Find(id);
		if (node == nullptr || input.empty()) {
			diagnostic = {{}, "select a node and name its input"};
			return false;
		}
		Operation operation;
		if (!ReadOperation(*node, operation, diagnostic)) return false;
		const int kind = engine::imagegraph::PropertyType(operation, property);
		if (kind < 0) {
			diagnostic = {Id(*node), "property cannot be bound"};
			return false;
		}
		const auto value = Value(*node, std::string(property));
		InputValue initial;
		switch (kind) {
		case 0:
			initial = value.Number;
			break;
		case 1:
			initial = value.Flag;
			break;
		case 2: {
			std::array<uint8_t, 4> colour{};
			const std::array<float, 4> rgba{value.Tint.R, value.Tint.G, value.Tint.B, value.Tint.A};
			for (size_t channel = 0; channel < colour.size(); ++channel)
				colour[channel] = static_cast<uint8_t>(std::lround(rgba[channel] * 255.f));
			initial = colour;
			break;
		}
		case 3:
			initial = value.Text;
			break;
		default:
			return false;
		}
		Document candidate;
		if (!SaveImageComposerDocument(state, candidate, diagnostic)) return false;
		if (std::none_of(
				candidate.Parameters.begin(), candidate.Parameters.end(), [&](const auto &parameter) {
					return parameter.Name == input;
				}
			))
			candidate.Parameters.push_back({std::string(input), std::move(initial)});
		std::erase_if(candidate.Bindings, [&](const auto &binding) {
			return binding.Node == Id(*node) && binding.Property == property;
		});
		candidate.Bindings.push_back({Id(*node), std::string(property), std::string(input)});
		Plan plan;
		if (!Compile(candidate, plan, diagnostic)) return false;
		state.Parameters = std::move(candidate.Parameters);
		state.Bindings = std::move(candidate.Bindings);
		CommitImageComposer(state);
		return true;
	}

	bool SetImageComposerOutput(ImageComposerState &state, std::string_view name, nodegraph::NodeId id) {
		const auto *node = state.Graph.Find(id);
		if (node == nullptr || name.empty()) return false;
		AssignImageComposerIdentities(state.Graph, state.Outputs);
		const auto found = std::find_if(state.Outputs.begin(), state.Outputs.end(), [&](const auto &output) {
			return output.Name == name;
		});
		if (found != state.Outputs.end())
			found->Node = Id(*node);
		else {
			if (state.Outputs.size() >= engine::imagegraph::Limits::MaximumOutputs) return false;
			state.Outputs.push_back({std::string(name), Id(*node)});
		}
		state.SelectedOutput = name;
		CommitImageComposer(state);
		return true;
	}
}
