#include <engine/ang/Compile.hpp>
#include <engine/imagegraph/AngGraph.hpp>
#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace engine::imagegraph {
	namespace {
		// Real numbers in [0, 1] get a slider; every other number is typed.
		bool GraphSlides(const PropertySpec &spec) {
			return spec.Kind == PropertyKind::Number && spec.Minimum == 0 && spec.Maximum == 1;
		}

		std::string GraphLabel(std::string_view key) {
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

		// The ang value a property holds on the canvas, and the widget hint for it.
		ang::Value GraphParamValue(const PropertySpec &spec, const InputValue &input) {
			switch (spec.Kind) {
			case PropertyKind::Dimension:
			case PropertyKind::Count:
			case PropertyKind::Offset:
			case PropertyKind::Number:
				return ang::Value(std::get<double>(input));
			case PropertyKind::Flag:
				return ang::Value(std::get<bool>(input));
			case PropertyKind::Colour: {
				const auto &colour = std::get<std::array<uint8_t, 4>>(input);
				return ang::Value(
					ang::Vector4{colour[0] / 255.f, colour[1] / 255.f, colour[2] / 255.f, colour[3] / 255.f}
				);
			}
			default:
				return ang::Value(std::get<std::string>(input));
			}
		}

		const char *GraphWidget(const PropertySpec &spec) {
			switch (spec.Kind) {
			case PropertyKind::Dimension:
			case PropertyKind::Count:
			case PropertyKind::Offset:
			case PropertyKind::Number:
				return GraphSlides(spec) ? "slider" : "number";
			case PropertyKind::Flag:
				return "toggle";
			case PropertyKind::Choice:
				return "select";
			case PropertyKind::Colour:
				return "colour";
			default:
				return "text";
			}
		}

		ang::Registry BuildGraphTypes() {
			ang::Registry types = ang::Registry::WithScalars();
			types.DefineType({.Id = std::string(IMAGE_PORT_TYPE), .Label = "Image"});
			for (const auto kind : NODE_KIND_IDS) {
				const auto *schema = FindNodeSchema(kind);
				Operation defaults;
				if (schema == nullptr || !DefaultOperation(kind, defaults)) continue;
				ang::NodeDef def{
					.Id = std::string(kind),
					.Category = "Image Composer",
					.Label = GraphLabel(kind.substr(std::string_view("image.").size())),
				};
				for (const auto &port : schema->Ports) {
					auto &ports =
						port.Direction == graphcore::PortDirection::Input ? def.Inputs : def.Outputs;
					ports.push_back({.Name = port.Id, .Type = port.ValueType, .Optional = port.Optional});
				}
				for (const auto &spec : DescribeKind(kind)) {
					InputValue input;
					if (!GetProperty(defaults, spec.Key, input)) continue;
					ang::Value value = GraphParamValue(spec, input);
					// A placed node starts at a useful size and with a nameable source.
					if (spec.Kind == PropertyKind::Dimension) value = ang::Value(256.0);
					if (spec.Key == "path" && value.Get<std::string>()->empty())
						value = ang::Value("source.png");
					def.Params.push_back({
						.Name = spec.Key,
						.Type = value.TypeName(),
						.Default = value,
						.Label = GraphLabel(spec.Key),
						.Widget = GraphWidget(spec),
						.Minimum = GraphSlides(spec) ? spec.Minimum : 0.0,
						.Maximum = GraphSlides(spec) ? spec.Maximum : 0.0,
						.Choices = spec.Choices,
					});
				}
				types.DefineNode(std::move(def));
			}
			return types;
		}

		// The value a node holds for a param, or the definition default.
		ang::Value
		GraphParamOf(const ang::NodeInstance &node, const ang::NodeDef &def, std::string_view key) {
			if (const auto held = node.Params.find(std::string(key)); held != node.Params.end())
				return held->second;
			const auto *param = def.FindParam(key);
			return param != nullptr ? param->Default : ang::Value{};
		}

		const ang::Link *
		GraphLinkInto(const ang::Graph &graph, std::string_view node, std::string_view port) {
			for (const ang::Link &link : graph.Links())
				if (link.ToNode == node && link.ToPort == port) return &link;
			return nullptr;
		}
	}

	bool ReadGraphOperation(const ang::NodeInstance &node, Operation &out, Diagnostic &diagnostic) {
		const auto fail = [&](std::string why) {
			diagnostic = {node.Id, std::move(why)};
			return false;
		};
		const auto def = GraphTypes().FindNode(node.Def);
		Operation operation;
		if (def == nullptr || def->Category != "Image Composer" || !DefaultOperation(node.Def, operation))
			return fail("unsupported composer node");
		for (const auto &[key, value] : node.Params) {
			const auto *param = def->FindParam(key);
			if (param == nullptr || value.TypeName() != param->Type)
				return fail("unsupported node control " + key);
		}
		for (const auto &spec : DescribeKind(node.Def)) {
			const ang::Value value = GraphParamOf(node, *def, spec.Key);
			InputValue input;
			if (const auto *number = value.Get<double>()) {
				if (!std::isfinite(*number)) return fail("non-finite node control " + spec.Key);
				input = *number;
			} else if (const auto *flag = value.Get<bool>()) {
				input = *flag;
			} else if (const auto *tint = value.Get<ang::Vector4>()) {
				std::array<uint8_t, 4> colour{};
				const std::array<float, 4> rgba{tint->X, tint->Y, tint->Z, tint->W};
				for (size_t channel = 0; channel < rgba.size(); ++channel) {
					if (!std::isfinite(rgba[channel]) || rgba[channel] < 0 || rgba[channel] > 1)
						return fail("colour must be within 0..1");
					colour[channel] = static_cast<uint8_t>(std::lround(rgba[channel] * 255.f));
				}
				input = colour;
			} else if (const auto *text = value.Get<std::string>()) {
				input = *text;
			} else {
				continue;
			}
			if (!SetProperty(operation, spec.Key, input)) return fail("invalid node control " + spec.Key);
		}
		out = std::move(operation);
		return true;
	}

	const ang::Registry &GraphTypes() {
		static const ang::Registry TYPES = BuildGraphTypes();
		return TYPES;
	}

	ang::Value GraphParam(const ang::NodeInstance &node, std::string_view key) {
		const auto def = GraphTypes().FindNode(node.Def);
		return def != nullptr ? GraphParamOf(node, *def, key) : ang::Value{};
	}

	bool ToAngGraph(const Document &document, ang::Graph &out, Diagnostic &diagnostic) {
		Plan checked;
		if (!Compile(document, checked, diagnostic)) return false;
		const auto &types = GraphTypes();
		ang::Graph candidate;
		for (const auto &node : document.Nodes) {
			if (std::abs(node.Position[0]) > std::numeric_limits<float>::max() ||
				std::abs(node.Position[1]) > std::numeric_limits<float>::max()) {
				diagnostic = {node.Id, "canvas position exceeds its numeric range"};
				return false;
			}
			const std::string kind(Kind(node.Value));
			if (types.FindNode(kind) == nullptr) {
				diagnostic = {node.Id, "composer node type is unavailable"};
				return false;
			}
			ang::NodeInstance placed{
				.Id = node.Id,
				.Def = kind,
				.Position = {static_cast<float>(node.Position[0]), static_cast<float>(node.Position[1])},
			};
			for (const auto &spec : DescribeKind(kind)) {
				InputValue input;
				if (GetProperty(node.Value, spec.Key, input))
					placed.Params[spec.Key] = GraphParamValue(spec, input);
			}
			if (!candidate.Apply(ang::edit::AddNode{std::move(placed)}).Ok) {
				diagnostic = {node.Id, "composer node id is repeated"};
				return false;
			}
		}
		for (const auto &node : document.Nodes) {
			const auto def = types.FindNode(std::string(Kind(node.Value)));
			for (size_t input = 0; input < node.Inputs.size(); ++input) {
				const ang::Link link{node.Inputs[input], "Image", node.Id, def->Inputs[input].Name};
				if (!ang::CheckNewLink(candidate, types, link).Ok ||
					!candidate.Apply(ang::edit::AddLink{link}).Ok) {
					diagnostic = {node.Id, "composer link was refused"};
					return false;
				}
			}
		}
		out = std::move(candidate);
		return true;
	}

	bool FromAngGraph(
		const ang::Graph &graph, std::span<const Output> outputs, Document &out, Diagnostic &diagnostic
	) {
		Document candidate;
		candidate.Outputs.assign(outputs.begin(), outputs.end());
		if (graph.Nodes().size() > Limits::MaximumNodes) {
			diagnostic = {{}, "composer node budget exceeded"};
			return false;
		}
		for (const auto &placed : graph.Nodes()) {
			Node node;
			node.Id = placed.Id;
			node.Position = {placed.Position.X, placed.Position.Y};
			if (!ReadGraphOperation(placed, node.Value, diagnostic)) return false;
			const auto def = GraphTypes().FindNode(placed.Def);
			size_t required = 0, accepted = 0;
			(void)InputRange(placed.Def, required, accepted);
			bool skipped = false;
			for (size_t index = 0; index < def->Inputs.size(); ++index) {
				const auto &port = def->Inputs[index];
				const auto *link = GraphLinkInto(graph, placed.Id, port.Name);
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
				if (graph.FindNode(link->FromNode) == nullptr || link->FromPort != "Image") {
					diagnostic = {node.Id, "invalid image link"};
					return false;
				}
				node.Inputs.push_back(link->FromNode);
			}
			candidate.Nodes.push_back(std::move(node));
		}
		Plan checked;
		if (!Compile(candidate, checked, diagnostic)) return false;
		out = std::move(candidate);
		return true;
	}
}
