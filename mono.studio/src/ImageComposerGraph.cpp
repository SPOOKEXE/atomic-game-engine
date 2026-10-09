#include "ImageComposerGraph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nodegraph/Layout.hpp>
#include <nodegraph/Registry.hpp>
#include <studio/ImageComposer.hpp>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
		constexpr const char *IMAGE = "composer.image";
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

		void Real(nodegraph::Node &node, const char *key, double number) {
			auto &value = node.Widgets[key];
			value.Kind = nodegraph::WidgetKind::Number;
			value.Number = number;
		}
		void Text(nodegraph::Node &node, const char *key, std::string text) {
			auto &value = node.Widgets[key];
			value.Kind = nodegraph::WidgetKind::Text;
			value.Text = std::move(text);
		}
		void Filter(nodegraph::Node &node, Sampling filter) {
			auto &value = node.Widgets["filter"];
			value.Kind = nodegraph::WidgetKind::Select;
			value.Text = SamplingName(filter);
			value.Number = filter == Sampling::Bilinear ? 1 : 0;
		}
		void Controls(nodegraph::Node &node, const Operation &operation) {
			std::visit(
				[&](const auto &op) {
					using T = std::decay_t<decltype(op)>;
					if constexpr (std::is_same_v<T, Source>) {
						Text(node, "path", op.Path);
						auto &choice = node.Widgets["interpretation"];
						choice.Kind = nodegraph::WidgetKind::Select;
						choice.Text = op.Interpretation == SourceInterpretation::Colour ? "colour" : "data";
					}
					if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
								  std::is_same_v<T, Crop> || std::is_same_v<T, Transform>) {
						Real(node, "width", op.Width);
						Real(node, "height", op.Height);
					}
					if constexpr (std::is_same_v<T, Solid>) {
						auto &colour = node.Widgets["colour"];
						colour.Kind = nodegraph::WidgetKind::Colour;
						colour.Tint = {
							op.Colour[0] / 255.f,
							op.Colour[1] / 255.f,
							op.Colour[2] / 255.f,
							op.Colour[3] / 255.f
						};
					}
					if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform>)
						Filter(node, op.Filter);
					if constexpr (std::is_same_v<T, Crop>) {
						Real(node, "x", op.X);
						Real(node, "y", op.Y);
					}
					if constexpr (std::is_same_v<T, Transform>) {
						Real(node, "translate_x", op.TranslateX);
						Real(node, "translate_y", op.TranslateY);
						Real(node, "scale_x", op.ScaleX);
						Real(node, "scale_y", op.ScaleY);
						Real(node, "degrees", op.Degrees);
						Real(node, "pivot_x", op.PivotX);
						Real(node, "pivot_y", op.PivotY);
					}
					if constexpr (std::is_same_v<T, Flip>) {
						node.Widgets["horizontal"].Kind = nodegraph::WidgetKind::Toggle;
						node.Widgets["horizontal"].Flag = op.Horizontal;
						node.Widgets["vertical"].Kind = nodegraph::WidgetKind::Toggle;
						node.Widgets["vertical"].Flag = op.Vertical;
					}
					if constexpr (std::is_same_v<T, Blend>) {
						node.Widgets["opacity"].Kind = nodegraph::WidgetKind::Slider;
						node.Widgets["opacity"].Number = op.Opacity;
					}
				},
				operation
			);
		}
		bool ReadOperation(const nodegraph::Node &node, Operation &out, Diagnostic &diagnostic) {
			const auto *type = nodegraph::NodeTypes::Find(node.Type);
			const auto fail = [&](std::string why) {
				diagnostic = {Id(node), std::move(why)};
				return false;
			};
			if (type == nullptr || type->Category != "Image Composer")
				return fail("unsupported composer node");
			for (const auto &[key, value] : node.Widgets) {
				if (key == IDENTITY) {
					if (value.Kind != nodegraph::WidgetKind::Text) return fail("invalid node identity");
					continue;
				}
				const auto spec =
					std::find_if(type->Widgets.begin(), type->Widgets.end(), [&](const auto &entry) {
						return entry.Key == key;
					});
				if (spec == type->Widgets.end() || value.Kind != spec->Kind)
					return fail("unsupported node control " + key);
			}
			const auto number = [&](const char *key) { return Value(node, key).Number; };
			for (const auto &spec : type->Widgets) {
				const auto value = Value(node, spec.Key);
				if ((spec.Kind == nodegraph::WidgetKind::Number ||
					 spec.Kind == nodegraph::WidgetKind::Slider) &&
					!std::isfinite(value.Number))
					return fail("non-finite node control " + spec.Key);
			}
			const auto dimension = [&](const char *key) {
				const double n = number(key);
				return n >= 1 && n <= Limits::MaximumDimension && std::floor(n) == n;
			};
			if (node.Type == "image.solid" || node.Type == "image.resize" || node.Type == "image.crop" ||
				node.Type == "image.transform")
				if (!dimension("width") || !dimension("height"))
					return fail("dimensions must be positive bounded integers");
			const auto width = static_cast<uint32_t>(
				node.Type == "image.source" || node.Type == "image.flip" || node.Type == "image.blend"
					? 1
					: number("width")
			);
			const auto height = static_cast<uint32_t>(
				node.Type == "image.source" || node.Type == "image.flip" || node.Type == "image.blend"
					? 1
					: number("height")
			);
			Sampling filter = Sampling::Nearest;
			if (node.Type == "image.resize" || node.Type == "image.transform") {
				const auto chosen = Value(node, "filter").Text;
				if (chosen == "bilinear")
					filter = Sampling::Bilinear;
				else if (chosen != "nearest")
					return fail("unsupported sampling option");
			}
			if (node.Type == "image.source") {
				const auto chosen = Value(node, "interpretation").Text;
				if (chosen != "colour" && chosen != "data") return fail("unsupported source interpretation");
				out = Source{
					Value(node, "path").Text,
					chosen == "colour" ? SourceInterpretation::Colour : SourceInterpretation::Data
				};
			} else if (node.Type == "image.solid") {
				const auto colour = Value(node, "colour").Tint;
				const std::array<float, 4> rgba{colour.R, colour.G, colour.B, colour.A};
				Solid solid{width, height};
				for (size_t i = 0; i < rgba.size(); ++i) {
					if (!std::isfinite(rgba[i]) || rgba[i] < 0 || rgba[i] > 1)
						return fail("colour must be within 0..1");
					solid.Colour[i] = static_cast<uint8_t>(std::lround(rgba[i] * 255.f));
				}
				out = solid;
			} else if (node.Type == "image.resize")
				out = Resize{width, height, filter};
			else if (node.Type == "image.crop") {
				for (const char *key : {"x", "y"}) {
					const double n = number(key);
					if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max() ||
						std::floor(n) != n)
						return fail("crop coordinates must be integers");
				}
				out =
					Crop{static_cast<int32_t>(number("x")), static_cast<int32_t>(number("y")), width, height};
			} else if (node.Type == "image.transform")
				out = Transform{
					width,
					height,
					number("translate_x"),
					number("translate_y"),
					number("scale_x"),
					number("scale_y"),
					number("degrees"),
					number("pivot_x"),
					number("pivot_y"),
					filter
				};
			else if (node.Type == "image.flip")
				out = Flip{Value(node, "horizontal").Flag, Value(node, "vertical").Flag};
			else if (node.Type == "image.blend")
				out = Blend{number("opacity")};
			else
				return fail("unsupported composer node");
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

	void RegisterImageComposerNodeTypes() {
		if (nodegraph::NodeTypes::Find("image.source") != nullptr) return;
		nodegraph::DataType image;
		image.Id = IMAGE;
		image.Label = "Image";
		image.Tint = {1, 1, 1, 1};
		nodegraph::DataTypes::Register(image);
		const auto registerType = [](const char *kind,
									 const char *title,
									 size_t inputs,
									 std::vector<nodegraph::WidgetSpec> widgets) {
			nodegraph::NodeType type;
			type.Id = kind;
			type.Title = title;
			type.Category = "Image Composer";
			type.Accent = {1, 1, 1, 1};
			type.Width = 200;
			if (inputs == 1) type.Inputs = {nodegraph::Port("Image", IMAGE)};
			if (inputs == 2)
				type.Inputs = {nodegraph::Port("Background", IMAGE), nodegraph::Port("Foreground", IMAGE)};
			type.Outputs = {nodegraph::Port("Image", IMAGE)};
			type.Widgets = std::move(widgets);
			nodegraph::NodeTypes::Register(type);
		};
		const auto dimensions = [] {
			return std::vector<nodegraph::WidgetSpec>{
				nodegraph::Number("width", "Width", 256), nodegraph::Number("height", "Height", 256)
			};
		};
		const auto filter = [] {
			return nodegraph::Select("filter", "Sampling", {"nearest", "bilinear"}, 0);
		};
		registerType(
			"image.source",
			"Source Image",
			0,
			{nodegraph::Text("path", "Source", "source.png"),
			 nodegraph::Select("interpretation", "Interpretation", {"colour", "data"}, 0)}
		);
		auto solid = dimensions();
		nodegraph::WidgetSpec colour;
		colour.Key = "colour";
		colour.Label = "RGBA";
		colour.Kind = colour.Default.Kind = nodegraph::WidgetKind::Colour;
		solid.push_back(colour);
		registerType("image.solid", "Solid", 0, solid);
		auto resize = dimensions();
		resize.push_back(filter());
		registerType("image.resize", "Resize", 1, resize);
		auto crop = dimensions();
		crop.push_back(nodegraph::Number("x", "X", 0));
		crop.push_back(nodegraph::Number("y", "Y", 0));
		registerType("image.crop", "Crop", 1, crop);
		auto transform = dimensions();
		for (const auto &[key, label, number] : std::array<std::tuple<const char *, const char *, double>, 7>{
				 {{"translate_x", "Translate X", 0},
				  {"translate_y", "Translate Y", 0},
				  {"scale_x", "Scale X", 1},
				  {"scale_y", "Scale Y", 1},
				  {"degrees", "Clockwise Degrees", 0},
				  {"pivot_x", "Pivot X", 0},
				  {"pivot_y", "Pivot Y", 0}}
			 })
			transform.push_back(nodegraph::Number(key, label, number));
		transform.push_back(filter());
		registerType("image.transform", "Transform 2D", 1, transform);
		registerType(
			"image.flip",
			"Flip",
			1,
			{nodegraph::Toggle("horizontal", "Horizontal", false),
			 nodegraph::Toggle("vertical", "Vertical", false)}
		);
		registerType("image.blend", "Blend", 2, {nodegraph::Slider("opacity", "Opacity", 0, 1, 1)});
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
			for (const auto &port : type->Inputs) {
				const auto *link = graph.LinkInto(placed.Id, port.Name);
				if (link == nullptr) {
					diagnostic = {node.Id, "connect " + port.Name};
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
