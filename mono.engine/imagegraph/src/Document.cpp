#include "Inputs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
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
		bool SourceValid(std::string_view path) {
			if (path.empty() || path.size() > 4096 || path.front() == '/' ||
				path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
				return false;
			for (size_t begin = 0; begin <= path.size();) {
				const size_t end = path.find('/', begin);
				const auto segment =
					path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
				if (segment.empty() || segment == "." || segment == ".." ||
					std::any_of(segment.begin(), segment.end(), [](unsigned char character) {
						return character < 32;
					}))
					return false;
				if (end == std::string_view::npos) return true;
				begin = end + 1;
			}
			return false;
		}
		bool Dimensions(uint32_t width, uint32_t height) {
			return width > 0 && height > 0 && width <= Limits::MaximumDimension &&
				   height <= Limits::MaximumDimension &&
				   uint64_t(width) * height * 4 <= Limits::MaximumImageBytes;
		}
		bool FilterValid(Sampling sampling) {
			return !SamplingName(sampling).empty();
		}
		std::string Validate(const Operation &operation) {
			if (operation.valueless_by_exception()) return "node has no operation";
			return std::visit(
				[](const auto &value) -> std::string {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Source>) {
						if (value.Interpretation != SourceInterpretation::Colour &&
							value.Interpretation != SourceInterpretation::Data)
							return "unsupported source interpretation";
						if (!SourceValid(value.Path))
							return "source must use a bounded relative path without parent segments";
					} else if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
										 std::is_same_v<T, Crop> || std::is_same_v<T, Transform>) {
						if (!Dimensions(value.Width, value.Height))
							return "output dimensions exceed image limits";
					}
					if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform>) {
						if (!FilterValid(value.Filter)) return "unsupported sampling filter";
					}
					if constexpr (std::is_same_v<T, Transform>) {
						const std::array controls{
							value.TranslateX,
							value.TranslateY,
							value.ScaleX,
							value.ScaleY,
							value.Degrees,
							value.PivotX,
							value.PivotY
						};
						if (!std::all_of(
								controls.begin(),
								controls.end(),
								[](double control) { return std::isfinite(control); }
							) ||
							value.ScaleX == 0 || value.ScaleY == 0)
							return "transform controls must be finite and scales must be nonzero";
					}
					if constexpr (std::is_same_v<T, Blend>) {
						if (!std::isfinite(value.Opacity) || value.Opacity < 0 || value.Opacity > 1)
							return "blend opacity must be in [0,1]";
					}
					return {};
				},
				operation
			);
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
		uint32_t Dimension(const json &value) {
			return static_cast<uint32_t>(Integer(value, 1, Limits::MaximumDimension));
		}
		double Number(const json &value) {
			if (!value.is_number()) throw std::runtime_error("numeric property required");
			const double number = value.get<double>();
			if (!std::isfinite(number)) throw std::runtime_error("finite numeric property required");
			return number;
		}
		Sampling Filter(const json &value) {
			const auto name = value.get<std::string>();
			if (name == "nearest") return Sampling::Nearest;
			if (name == "bilinear") return Sampling::Bilinear;
			throw std::runtime_error("unsupported sampling filter");
		}
		Operation DecodeOperation(std::string_view kind, const json &properties, bool live) {
			if (kind == "image.source") {
				if (live)
					Fields(properties, {"path", "interpretation"});
				else
					Fields(properties, {"path"});
				Source source{properties.at("path").get<std::string>()};
				if (live) {
					const auto name = properties.at("interpretation").get<std::string>();
					if (name != "colour" && name != "data")
						throw std::runtime_error("unsupported source interpretation");
					source.Interpretation =
						name == "colour" ? SourceInterpretation::Colour : SourceInterpretation::Data;
				}
				return source;
			}
			if (kind == "image.solid") {
				Fields(properties, {"width", "height", "colour"});
				Solid value{Dimension(properties.at("width")), Dimension(properties.at("height"))};
				const auto &colour = properties.at("colour");
				if (!colour.is_array() || colour.size() != 4)
					throw std::runtime_error("solid colour requires four channels");
				for (size_t channel = 0; channel < 4; channel++)
					value.Colour[channel] = static_cast<uint8_t>(Integer(colour[channel], 0, 255));
				return value;
			}
			if (kind == "image.resize") {
				Fields(properties, {"width", "height", "filter"});
				return Resize{
					Dimension(properties.at("width")),
					Dimension(properties.at("height")),
					Filter(properties.at("filter"))
				};
			}
			if (kind == "image.crop") {
				Fields(properties, {"x", "y", "width", "height"});
				return Crop{
					static_cast<int32_t>(Integer(properties.at("x"), INT32_MIN, INT32_MAX)),
					static_cast<int32_t>(Integer(properties.at("y"), INT32_MIN, INT32_MAX)),
					Dimension(properties.at("width")),
					Dimension(properties.at("height"))
				};
			}
			if (kind == "image.transform") {
				Fields(
					properties,
					{"width",
					 "height",
					 "translate_x",
					 "translate_y",
					 "scale_x",
					 "scale_y",
					 "degrees",
					 "pivot_x",
					 "pivot_y",
					 "filter"}
				);
				return Transform{
					Dimension(properties.at("width")),
					Dimension(properties.at("height")),
					Number(properties.at("translate_x")),
					Number(properties.at("translate_y")),
					Number(properties.at("scale_x")),
					Number(properties.at("scale_y")),
					Number(properties.at("degrees")),
					Number(properties.at("pivot_x")),
					Number(properties.at("pivot_y")),
					Filter(properties.at("filter"))
				};
			}
			if (kind == "image.flip") {
				Fields(properties, {"horizontal", "vertical"});
				return Flip{properties.at("horizontal").get<bool>(), properties.at("vertical").get<bool>()};
			}
			if (kind == "image.blend") {
				Fields(properties, {"opacity"});
				return Blend{Number(properties.at("opacity"))};
			}
			throw std::runtime_error("unsupported node kind: " + std::string(kind));
		}
		json EncodeOperation(const Operation &operation, bool live) {
			return std::visit(
				[live](const auto &value) -> json {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Source>) {
						json result{{"path", value.Path}};
						if (live)
							result["interpretation"] =
								value.Interpretation == SourceInterpretation::Colour ? "colour" : "data";
						return result;
					}
					if constexpr (std::is_same_v<T, Solid>)
						return {{"width", value.Width}, {"height", value.Height}, {"colour", value.Colour}};
					if constexpr (std::is_same_v<T, Resize>)
						return {
							{"width", value.Width},
							{"height", value.Height},
							{"filter", SamplingName(value.Filter)}
						};
					if constexpr (std::is_same_v<T, Crop>)
						return {
							{"x", value.X}, {"y", value.Y}, {"width", value.Width}, {"height", value.Height}
						};
					if constexpr (std::is_same_v<T, Transform>)
						return {
							{"width", value.Width},
							{"height", value.Height},
							{"translate_x", value.TranslateX},
							{"translate_y", value.TranslateY},
							{"scale_x", value.ScaleX},
							{"scale_y", value.ScaleY},
							{"degrees", value.Degrees},
							{"pivot_x", value.PivotX},
							{"pivot_y", value.PivotY},
							{"filter", SamplingName(value.Filter)}
						};
					if constexpr (std::is_same_v<T, Flip>)
						return {{"horizontal", value.Horizontal}, {"vertical", value.Vertical}};
					if constexpr (std::is_same_v<T, Blend>) return {{"opacity", value.Opacity}};
				},
				operation
			);
		}
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

	bool Image::IsValid() const noexcept {
		return (Space == OutputSpace::SRGB || Space == OutputSpace::Linear) && Dimensions(Width, Height) &&
			   Pixels.size() == uint64_t(Width) * Height * 4;
	}
	std::string_view Kind(const Operation &operation) noexcept {
		constexpr std::array<std::string_view, 7> kinds{
			"image.source",
			"image.solid",
			"image.resize",
			"image.crop",
			"image.transform",
			"image.flip",
			"image.blend"
		};
		return operation.index() < kinds.size() ? kinds[operation.index()] : std::string_view{};
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
			std::unordered_map<std::string_view, size_t> names;
			size_t authoredBytes = 0;
			for (size_t index = 0; index < document.Nodes.size(); index++) {
				const Node &node = document.Nodes[index];
				if (!NameValid(node.Id) || !names.emplace(node.Id, index).second)
					return Fail(diagnostic, node.Id, "node identity is empty, invalid or duplicated");
				if (!std::isfinite(node.Position[0]) || !std::isfinite(node.Position[1]))
					return Fail(diagnostic, node.Id, "canvas position must be finite");
				const std::string failure = Validate(node.Value);
				if (!failure.empty()) return Fail(diagnostic, node.Id, failure);
				const size_t expected =
					std::holds_alternative<Source>(node.Value) || std::holds_alternative<Solid>(node.Value)
						? 0
					: std::holds_alternative<Blend>(node.Value) ? 2
																: 1;
				if (node.Inputs.size() != expected)
					return Fail(diagnostic, node.Id, "input count does not match node kind");
				authoredBytes += node.Id.size();
				if (const auto *source = std::get_if<Source>(&node.Value))
					authoredBytes += source->Path.size();
				for (const auto &input : node.Inputs) {
					if (!NameValid(input)) return Fail(diagnostic, node.Id, "input identity is invalid");
					authoredBytes += input.size();
				}
			}
			for (size_t index = 0; index < document.Nodes.size(); index++) {
				for (const auto &input : document.Nodes[index].Inputs) {
					const auto found = names.find(input);
					if (found == names.end())
						return Fail(
							diagnostic, document.Nodes[index].Id, "input names a missing node: " + input
						);
					plan.Inputs[index].push_back(found->second);
				}
			}
			std::unordered_set<std::string_view> outputs;
			for (const auto &output : document.Outputs) {
				if (output.Space != OutputSpace::SRGB && output.Space != OutputSpace::Linear)
					return Fail(diagnostic, output.Node, "unsupported output space");
				if (!NameValid(output.Name) || !NameValid(output.Node) ||
					!outputs.emplace(output.Name).second)
					return Fail(diagnostic, output.Node, "output identity is empty, invalid or duplicated");
				const auto found = names.find(output.Node);
				if (found == names.end()) return Fail(diagnostic, output.Node, "output names a missing node");
				plan.Outputs.push_back(found->second);
				authoredBytes += output.Name.size() + output.Node.size();
			}
			if (authoredBytes > Limits::MaximumDocumentBytes)
				return Fail(diagnostic, {}, "authored data exceeds document byte limit");
			std::vector<uint8_t> visited(document.Nodes.size());
			while (plan.Order.size() < document.Nodes.size()) {
				bool progressed = false;
				for (size_t index = 0; index < document.Nodes.size(); index++) {
					if (visited[index] ||
						!std::all_of(plan.Inputs[index].begin(), plan.Inputs[index].end(), [&](size_t input) {
							return visited[input];
						}))
						continue;
					visited[index] = true;
					plan.Order.push_back(index);
					progressed = true;
				}
				if (!progressed) {
					const auto unvisited = std::find(visited.begin(), visited.end(), uint8_t{0});
					return Fail(
						diagnostic,
						document.Nodes[static_cast<size_t>(unvisited - visited.begin())].Id,
						"graph contains a dependency cycle"
					);
				}
			}
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
			const bool live = Integer(root.at("version"), 1, 2) == 2;
			if (live)
				Fields(root, {"version", "nodes", "outputs", "parameters", "bindings"});
			else
				Fields(root, {"version", "nodes", "outputs"});
			const auto &nodes = root.at("nodes");
			const auto &outputs = root.at("outputs");
			if (!nodes.is_array() || nodes.empty() || nodes.size() > Limits::MaximumNodes ||
				!outputs.is_array() || outputs.empty() || outputs.size() > Limits::MaximumOutputs)
				return Fail(diagnostic, {}, "graph node or output count exceeds limits");
			Document document;
			for (const auto &encodedNode : nodes) {
				Fields(encodedNode, {"id", "kind", "inputs", "position", "properties"});
				nodeId = encodedNode.at("id").get<std::string>();
				Node node;
				node.Id = nodeId;
				node.Value = DecodeOperation(
					encodedNode.at("kind").get<std::string>(), encodedNode.at("properties"), live
				);
				const auto &inputs = encodedNode.at("inputs");
				const auto &position = encodedNode.at("position");
				if (!inputs.is_array() || inputs.size() > 2 || !position.is_array() || position.size() != 2)
					throw std::runtime_error("invalid inputs or canvas position");
				for (const auto &input : inputs)
					node.Inputs.push_back(input.get<std::string>());
				node.Position = {Number(position[0]), Number(position[1])};
				document.Nodes.push_back(std::move(node));
			}
			for (const auto &encodedOutput : outputs) {
				if (live)
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
			const bool live =
				!document.Parameters.empty() || !document.Bindings.empty() ||
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
			json root{{"version", live ? 2 : 1}, {"nodes", json::array()}, {"outputs", json::array()}};
			for (const auto &node : document.Nodes) {
				activeNode = node.Id;
				json properties = EncodeOperation(node.Value, live);
				// A binding may replace an invalid fallback in a transient snapshot. Never
				// save a fallback that the strict project reader could not reconstruct.
				(void)DecodeOperation(Kind(node.Value), properties, live);
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
