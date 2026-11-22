#include "Inputs.hpp"
#include "Properties.hpp"

#include <engine/graphcore/Compile.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace engine::imagegraph {
	namespace {
		using nlohmann::json;
		bool Fail(Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			return false;
		}
		bool NameValid(std::string_view name) {
			return !name.empty() && name.size() <= 128 &&
				   std::none_of(name.begin(), name.end(), [](unsigned char character) {
					   return character < 32;
				   });
		}
		bool Dimensions(uint32_t width, uint32_t height) {
			return width > 0 && height > 0 && width <= Limits::MaximumDimension &&
				   height <= Limits::MaximumDimension &&
				   uint64_t(width) * height * 4 <= Limits::MaximumImageBytes;
		}
		void Fields(const json &object, std::initializer_list<std::string_view> expected) {
			if (!object.is_object() || object.size() != expected.size())
				throw std::runtime_error("object fields do not match the project schema");
			for (auto field : expected)
				if (!object.contains(std::string(field)))
					throw std::runtime_error("missing project field: " + std::string(field));
		}
		int64_t Integer(const json &value, int64_t minimum, int64_t maximum) {
			if (!value.is_number_integer()) throw std::runtime_error("integer property required");
			if (value.is_number_unsigned()) {
				const uint64_t number = value.get<uint64_t>();
				if ((minimum > 0 && number < static_cast<uint64_t>(minimum)) ||
					number > static_cast<uint64_t>(maximum))
					throw std::runtime_error("integer property out of range");
				return static_cast<int64_t>(number);
			}
			const int64_t number = value.get<int64_t>();
			if (number < minimum || number > maximum)
				throw std::runtime_error("integer property out of range");
			return number;
		}
		double Number(const json &value) {
			if (!value.is_number()) throw std::runtime_error("numeric property required");
			const double number = value.get<double>();
			if (!std::isfinite(number)) throw std::runtime_error("finite numeric property required");
			return number;
		}
		constexpr std::array<std::string_view, 3> FORMATS{"rgba8", "r16f", "rgba16f"};
		bool BoundedJson(std::string_view encoded) {
			if (encoded.size() > Limits::MaximumDocumentBytes) return false;
			size_t depth = 0;
			bool string = false;
			bool escape = false;
			for (char character : encoded) {
				if (string) {
					if (escape)
						escape = false;
					else if (character == '\\')
						escape = true;
					else if (character == '"')
						string = false;
				} else if (character == '"')
					string = true;
				else if (character == '{' || character == '[') {
					if (++depth > Limits::MaximumJsonDepth) return false;
				} else if (character == '}' || character == ']') {
					if (depth == 0) return false;
					--depth;
				}
			}
			return depth == 0 && !string;
		}
	}

	size_t BytesPerPixel(OutputFormat format) noexcept {
		switch (format) {
		case OutputFormat::RGBA8:
			return 4;
		case OutputFormat::R16F:
			return 2;
		case OutputFormat::RGBA16F:
			return 8;
		}
		return 0;
	}
	bool Image::IsValid() const noexcept {
		const size_t bytes = BytesPerPixel(Format);
		return (Space == OutputSpace::SRGB || Space == OutputSpace::Linear) && bytes != 0 &&
			   Dimensions(Width, Height) && Pixels.size() == uint64_t(Width) * Height * bytes;
	}
	std::string_view Kind(const Operation &operation) noexcept {
		return operation.index() < NODE_KIND_IDS.size() ? NODE_KIND_IDS[operation.index()]
														: std::string_view{};
	}
	std::string_view SamplingName(Sampling sampling) noexcept {
		switch (sampling) {
		case Sampling::Nearest:
			return "nearest";
		case Sampling::Bilinear:
			return "bilinear";
		}
		return {};
	}
	bool detail::CompileResolved(const Document &document, Plan &out, Diagnostic &diagnostic) {
		if (document.Nodes.empty() || document.Nodes.size() > Limits::MaximumNodes ||
			document.Outputs.empty() || document.Outputs.size() > Limits::MaximumOutputs)
			return Fail(diagnostic, {}, "graph node or output count exceeds limits");
		try {
			Plan plan;
			plan.Inputs.resize(document.Nodes.size());
			std::vector<graphcore::Node> nodes;
			std::vector<graphcore::Connection> connections;
			nodes.reserve(document.Nodes.size());
			connections.reserve(document.Nodes.size() * Limits::MaximumInputs);
			size_t authoredBytes = 0;
			for (size_t index = 0; index < document.Nodes.size(); index++) {
				const Node &node = document.Nodes[index];
				if (!NameValid(node.Id))
					return Fail(diagnostic, node.Id, "node identity is empty, invalid or duplicated");
				if (!std::isfinite(node.Position[0]) || !std::isfinite(node.Position[1]))
					return Fail(diagnostic, node.Id, "canvas position must be finite");
				const std::string failure = detail::Validate(node.Value);
				if (!failure.empty()) return Fail(diagnostic, node.Id, failure);
				const auto *schema = FindNodeSchema(Kind(node.Value));
				if (schema == nullptr) return Fail(diagnostic, node.Id, "image operation has no node schema");
				size_t minimum = 0, maximum = 0;
				(void)InputRange(schema->Id, minimum, maximum);
				if (node.Inputs.size() < minimum || node.Inputs.size() > maximum)
					return Fail(diagnostic, node.Id, "input count does not match node kind");
				if (const auto *channels = std::get_if<Channels>(&node.Value))
					for (const auto &pick : channels->Picks)
						if (pick.Input >= 0 && size_t(pick.Input) >= node.Inputs.size())
							return Fail(diagnostic, node.Id, "channel pick names an unconnected input");
				nodes.push_back({node.Id, schema->Id});
				authoredBytes += node.Id.size();
				if (const auto *source = std::get_if<Source>(&node.Value))
					authoredBytes += source->Path.size();
				if (const auto *mesh = std::get_if<MeshAttribute>(&node.Value))
					authoredBytes += mesh->Mesh.size();
				if (const auto *ramp = std::get_if<Ramp>(&node.Value))
					authoredBytes += ramp->Stops.size() * sizeof(RampStop);
				for (const auto &input : node.Inputs) {
					if (!NameValid(input)) return Fail(diagnostic, node.Id, "input identity is invalid");
					authoredBytes += input.size();
				}
				size_t inputIndex = 0;
				for (const auto &port : schema->Ports) {
					if (port.Direction == graphcore::PortDirection::Input && inputIndex < node.Inputs.size())
						connections.push_back({node.Inputs[inputIndex++], "Image", node.Id, port.Id});
				}
			}
			const auto schemas = NodeSchemas();
			size_t schemaPorts = 0;
			for (const auto &schema : schemas)
				schemaPorts += schema.Ports.size();
			const graphcore::Limits limits{
				.MaximumNodes = Limits::MaximumNodes,
				.MaximumConnections = Limits::MaximumNodes * Limits::MaximumInputs,
				.MaximumSchemas = schemas.size(),
				.MaximumPorts = schemaPorts,
				.MaximumIdentityBytes = Limits::MaximumDocumentBytes,
			};
			graphcore::CompiledGraph compiled;
			graphcore::Diagnostic sharedDiagnostic;
			const auto status =
				graphcore::CompileGraph(nodes, connections, schemas, limits, compiled, sharedDiagnostic);
			if (status != graphcore::CompileStatus::Ok) {
				if (status == graphcore::CompileStatus::DuplicateNode)
					return Fail(
						diagnostic, sharedDiagnostic.Node, "node identity is empty, invalid or duplicated"
					);
				if (status == graphcore::CompileStatus::UnknownNode) {
					const auto failed =
						std::find_if(connections.begin(), connections.end(), [&](const auto &connection) {
							return connection.FromNode == sharedDiagnostic.Node;
						});
					if (failed != connections.end())
						return Fail(
							diagnostic, failed->ToNode, "input names a missing node: " + failed->FromNode
						);
				}
				if (status == graphcore::CompileStatus::Cycle) {
					const auto blocked = sharedDiagnostic.Blocked;
					return Fail(
						diagnostic,
						blocked < document.Nodes.size() ? document.Nodes[blocked].Id : std::string_view{},
						"graph contains a dependency cycle"
					);
				}
				if (status == graphcore::CompileStatus::AllocationFailed)
					return Fail(diagnostic, {}, "graph allocation refused");
				return Fail(diagnostic, sharedDiagnostic.Node, graphcore::Describe(status));
			}
			for (const auto &connection : compiled.Connections)
				plan.Inputs[connection.ToNode].push_back(connection.FromNode);
			plan.Order.assign(compiled.Dependencies.Order.begin(), compiled.Dependencies.Order.end());
			std::unordered_set<std::string_view> outputs;
			for (const auto &output : document.Outputs) {
				if (output.Space != OutputSpace::SRGB && output.Space != OutputSpace::Linear)
					return Fail(diagnostic, output.Node, "unsupported output space");
				if (BytesPerPixel(output.Format) == 0)
					return Fail(diagnostic, output.Node, "unsupported output format");
				if (!NameValid(output.Name) || !NameValid(output.Node) ||
					!outputs.emplace(output.Name).second)
					return Fail(diagnostic, output.Node, "output identity is empty, invalid or duplicated");
				const auto found =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						return node.Id == output.Node;
					});
				if (found == document.Nodes.end())
					return Fail(diagnostic, output.Node, "output names a missing node");
				plan.Outputs.push_back(static_cast<size_t>(found - document.Nodes.begin()));
				authoredBytes += output.Name.size() + output.Node.size();
			}
			if (authoredBytes > Limits::MaximumDocumentBytes)
				return Fail(diagnostic, {}, "authored data exceeds document byte limit");
			if (document.Storage != Precision::Byte && document.Storage != Precision::Wide)
				return Fail(diagnostic, {}, "unsupported intermediate precision");
			out = std::move(plan);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, {}, "graph allocation refused");
		}
	}
	bool Read(std::string_view encoded, Document &out, Diagnostic &diagnostic) {
		if (!BoundedJson(encoded))
			return Fail(diagnostic, {}, "project byte, nesting or structure limit refused");
		std::string nodeId;
		try {
			bool duplicateField = false;
			std::vector<std::unordered_set<std::string>> objectFields;
			const json root = json::parse(encoded, [&](int, json::parse_event_t event, json &value) {
				if (event == json::parse_event_t::object_start)
					objectFields.emplace_back();
				else if (event == json::parse_event_t::object_end)
					objectFields.pop_back();
				else if (event == json::parse_event_t::key &&
						 !objectFields.back().insert(value.get<std::string>()).second)
					duplicateField = true;
				return true;
			});
			if (duplicateField) return Fail(diagnostic, {}, "duplicate project object field");
			const int64_t version = Integer(root.at("version"), 1, 3);
			const bool live = version >= 2;
			const bool precise = version == 3;
			if (precise)
				Fields(root, {"version", "nodes", "outputs", "parameters", "bindings", "precision"});
			else if (live)
				Fields(root, {"version", "nodes", "outputs", "parameters", "bindings"});
			else
				Fields(root, {"version", "nodes", "outputs"});
			const auto &nodes = root.at("nodes");
			const auto &outputs = root.at("outputs");
			if (!nodes.is_array() || nodes.empty() || nodes.size() > Limits::MaximumNodes ||
				!outputs.is_array() || outputs.empty() || outputs.size() > Limits::MaximumOutputs)
				return Fail(diagnostic, {}, "graph node or output count exceeds limits");
			Document document;
			if (precise) {
				const auto precision = root.at("precision").get<std::string>();
				if (precision != "byte" && precision != "wide")
					throw std::runtime_error("unsupported intermediate precision");
				document.Storage = precision == "byte" ? Precision::Byte : Precision::Wide;
			}
			for (const auto &encodedNode : nodes) {
				Fields(encodedNode, {"id", "kind", "inputs", "position", "properties"});
				nodeId = encodedNode.at("id").get<std::string>();
				Node node;
				node.Id = nodeId;
				node.Value = detail::DecodeOperation(
					encodedNode.at("kind").get<std::string>(), encodedNode.at("properties"), live
				);
				const auto &inputs = encodedNode.at("inputs");
				const auto &position = encodedNode.at("position");
				if (!inputs.is_array() || inputs.size() > Limits::MaximumInputs || !position.is_array() ||
					position.size() != 2)
					throw std::runtime_error("invalid inputs or canvas position");
				for (const auto &input : inputs)
					node.Inputs.push_back(input.get<std::string>());
				node.Position = {Number(position[0]), Number(position[1])};
				document.Nodes.push_back(std::move(node));
			}
			for (const auto &encodedOutput : outputs) {
				if (precise)
					Fields(encodedOutput, {"name", "node", "space", "format"});
				else if (live)
					Fields(encodedOutput, {"name", "node", "space"});
				else
					Fields(encodedOutput, {"name", "node"});
				Output output{
					encodedOutput.at("name").get<std::string>(), encodedOutput.at("node").get<std::string>()
				};
				if (live) {
					const auto space = encodedOutput.at("space").get<std::string>();
					if (space != "srgb" && space != "linear")
						throw std::runtime_error("unsupported output space");
					output.Space = space == "srgb" ? OutputSpace::SRGB : OutputSpace::Linear;
				}
				if (precise) {
					const auto format = encodedOutput.at("format").get<std::string>();
					const auto found = std::find(FORMATS.begin(), FORMATS.end(), format);
					if (found == FORMATS.end()) throw std::runtime_error("unsupported output format");
					output.Format = static_cast<OutputFormat>(found - FORMATS.begin());
				}
				document.Outputs.push_back(std::move(output));
			}
			if (live) {
				const auto &parameters = root.at("parameters");
				const auto &bindings = root.at("bindings");
				if (!parameters.is_array() || parameters.size() > Limits::MaximumParameters ||
					!bindings.is_array() || bindings.size() > Limits::MaximumBindings)
					throw std::runtime_error("input or binding count exceeds limits");
				for (const auto &parameter : parameters) {
					Fields(parameter, {"name", "type", "default"});
					const auto type = parameter.at("type").get<std::string>();
					const auto &value = parameter.at("default");
					InputValue decoded;
					if (type == "number")
						decoded = Number(value);
					else if (type == "boolean")
						decoded = value.get<bool>();
					else if (type == "string")
						decoded = value.get<std::string>();
					else if (type == "colour") {
						if (!value.is_array() || value.size() != 4)
							throw std::runtime_error("input colour requires four channels");
						std::array<uint8_t, 4> colour{};
						for (size_t channel = 0; channel < 4; ++channel)
							colour[channel] = static_cast<uint8_t>(Integer(value[channel], 0, 255));
						decoded = colour;
					} else
						throw std::runtime_error("unsupported input type");
					document.Parameters.push_back(
						{parameter.at("name").get<std::string>(), std::move(decoded)}
					);
				}
				for (const auto &binding : bindings) {
					Fields(binding, {"node", "property", "input"});
					document.Bindings.push_back(
						{binding.at("node").get<std::string>(),
						 binding.at("property").get<std::string>(),
						 binding.at("input").get<std::string>()}
					);
				}
			}
			Plan plan;
			if (!Compile(document, plan, diagnostic)) return false;
			out = std::move(document);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return Fail(diagnostic, nodeId, error.what());
		}
	}
	bool Write(const Document &document, std::string &out, Diagnostic &diagnostic) {
		Plan plan;
		if (!Compile(document, plan, diagnostic)) return false;
		std::string_view activeNode;
		try {
			const bool precise =
				document.Storage != Precision::Byte ||
				std::any_of(document.Outputs.begin(), document.Outputs.end(), [](const Output &output) {
					return output.Format != OutputFormat::RGBA8;
				});
			const bool live =
				precise || !document.Parameters.empty() || !document.Bindings.empty() ||
				std::any_of(
					document.Nodes.begin(),
					document.Nodes.end(),
					[](const Node &node) {
						const auto *source = std::get_if<Source>(&node.Value);
						return source && source->Interpretation != SourceInterpretation::Colour;
					}
				) ||
				std::any_of(document.Outputs.begin(), document.Outputs.end(), [](const Output &output) {
					return output.Space != OutputSpace::SRGB;
				});
			json root{
				{"version",
				 precise ? 3
				 : live	 ? 2
						 : 1},
				{"nodes", json::array()},
				{"outputs", json::array()}
			};
			if (precise) root["precision"] = document.Storage == Precision::Byte ? "byte" : "wide";
			for (const auto &node : document.Nodes) {
				activeNode = node.Id;
				json properties = detail::EncodeOperation(node.Value, live);
				// A binding may replace an invalid fallback in a transient snapshot. Never
				// save a fallback that the strict project reader could not reconstruct.
				(void)detail::DecodeOperation(Kind(node.Value), properties, live);
				root["nodes"].push_back(
					{{"id", node.Id},
					 {"kind", Kind(node.Value)},
					 {"inputs", node.Inputs},
					 {"position", node.Position},
					 {"properties", std::move(properties)}}
				);
			}
			for (const auto &output : document.Outputs) {
				json encoded{{"name", output.Name}, {"node", output.Node}};
				if (live) encoded["space"] = output.Space == OutputSpace::SRGB ? "srgb" : "linear";
				if (precise) encoded["format"] = FORMATS[static_cast<size_t>(output.Format)];
				root["outputs"].push_back(std::move(encoded));
			}
			if (live) {
				root["parameters"] = json::array();
				root["bindings"] = json::array();
				constexpr std::array<std::string_view, 4> types{"number", "boolean", "colour", "string"};
				for (const auto &parameter : document.Parameters) {
					json value =
						std::visit([](const auto &input) -> json { return input; }, parameter.Default);
					root["parameters"].push_back(
						{{"name", parameter.Name},
						 {"type", types[parameter.Default.index()]},
						 {"default", std::move(value)}}
					);
				}
				for (const auto &binding : document.Bindings)
					root["bindings"].push_back(
						{{"node", binding.Node}, {"property", binding.Property}, {"input", binding.Input}}
					);
			}
			std::string encoded = root.dump(2);
			if (encoded.size() > Limits::MaximumDocumentBytes)
				return Fail(diagnostic, {}, "encoded project exceeds byte limit");
			out = std::move(encoded);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return Fail(diagnostic, activeNode, error.what());
		}
	}
}
