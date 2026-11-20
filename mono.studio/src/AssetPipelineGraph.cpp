#include "AssetPipelineGraph.hpp"

#include <engine/bake/GraphDocument.hpp>
#include <engine/bakegraph/Schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nodegraph/Registry.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

namespace studio {
	namespace {
		using engine::bake::NodeKind;
		using engine::bake::Operation;
		using engine::bake::OperationKind;

		constexpr const char *PREFIX = "asset.";
		constexpr NodeKind LAST_KIND = NodeKind::Flipbook;
		constexpr float COLUMN = 240.0f;
		constexpr float ROW = 150.0f;

		std::string TypeId(NodeKind kind) {
			const auto *schema = engine::bake::NodeSchemaFor(kind);
			return schema == nullptr ? std::string{} : PREFIX + schema->Id;
		}

		std::optional<NodeKind> KindOf(const std::string &type) {
			for (int ordinal = 0; ordinal <= static_cast<int>(LAST_KIND); ++ordinal) {
				const auto kind = static_cast<NodeKind>(ordinal);
				if (TypeId(kind) == type) return kind;
			}
			return std::nullopt;
		}

		// The node a node operation adds, or nothing for `Connect`.
		std::optional<NodeKind> KindOf(const Operation &operation) {
			switch (operation.Kind) {
			case OperationKind::AddSource:
				return NodeKind::Source;
			case OperationKind::AddBuiltin:
				return NodeKind::Builtin;
			case OperationKind::AddNode:
				return operation.Node;
			case OperationKind::AddFit:
				return NodeKind::Fit;
			case OperationKind::AddScale:
				return NodeKind::Scale;
			case OperationKind::AddResize:
				return NodeKind::Resize;
			case OperationKind::AddRasterize:
				return NodeKind::Rasterize;
			case OperationKind::AddRetime:
				return NodeKind::Retime;
			case OperationKind::AddWrite:
				return NodeKind::Write;
			case OperationKind::AddDecimate:
				return NodeKind::Decimate;
			case OperationKind::AddFlipbook:
				return NodeKind::Flipbook;
			case OperationKind::Connect:
				return std::nullopt;
			}
			return std::nullopt;
		}

		std::vector<nodegraph::WidgetSpec> Widgets(NodeKind kind) {
			switch (kind) {
			case NodeKind::Source:
				return {nodegraph::Text("name", "Raw file", "")};
			case NodeKind::Builtin:
				return {nodegraph::Text("name", "Built-in", "cube")};
			case NodeKind::Fit:
				return {nodegraph::Number("size", "Size (m)", 1.0)};
			case NodeKind::Scale:
				return {
					nodegraph::Number("x", "X", 1.0),
					nodegraph::Number("y", "Y", 1.0),
					nodegraph::Number("z", "Z", 1.0)
				};
			case NodeKind::Resize:
			case NodeKind::Rasterize:
				return {nodegraph::Number("width", "Width", 256), nodegraph::Number("height", "Height", 256)};
			case NodeKind::Retime:
				return {nodegraph::Number("fps", "Frames/s", 30)};
			case NodeKind::Decimate:
				return {nodegraph::Slider("ratio", "Keep", 0.01, 1.0, 0.5)};
			case NodeKind::Flipbook:
				return {
					nodegraph::Number("side", "Grid side", 4),
					nodegraph::Number("frames", "Frames", 16),
					nodegraph::Number("fps", "Frames/s", 12)
				};
			case NodeKind::Write:
				return {nodegraph::Text("name", "Asset name", "")};
			default:
				return {};
			}
		}

		const char *Title(NodeKind kind) {
			switch (kind) {
			case NodeKind::Source:
				return "Source";
			case NodeKind::Builtin:
				return "Built-in Mesh";
			case NodeKind::Import:
				return "Import";
			case NodeKind::Fit:
				return "Fit";
			case NodeKind::Scale:
				return "Scale";
			case NodeKind::Smooth:
				return "Smooth Normals";
			case NodeKind::Resize:
				return "Resize";
			case NodeKind::Opaque:
				return "Opaque";
			case NodeKind::Mipmap:
				return "Mipmap";
			case NodeKind::Retime:
				return "Retime";
			case NodeKind::Write:
				return "Write";
			case NodeKind::Rasterize:
				return "Rasterize";
			case NodeKind::Decimate:
				return "Decimate";
			case NodeKind::Flipbook:
				return "Flipbook";
			}
			return "?";
		}

		nodegraph::Value Read(const nodegraph::Node &node, const std::string &key) {
			if (const auto held = node.Widgets.find(key); held != node.Widgets.end()) return held->second;
			if (const auto *type = nodegraph::NodeTypes::Find(node.Type))
				for (const auto &spec : type->Widgets)
					if (spec.Key == key) return spec.Default;
			return {};
		}

		uint32_t Whole(const nodegraph::Node &node, const std::string &key) {
			const double value = Read(node, key).Number;
			return value <= 0.0 ? 0u : static_cast<uint32_t>(std::lround(value));
		}

		void SetNumber(nodegraph::Node &node, const char *key, double number) {
			auto &value = node.Widgets[key];
			value.Kind = nodegraph::WidgetKind::Number;
			value.Number = number;
		}

		void SetText(nodegraph::Node &node, const char *key, const std::string &text) {
			auto &value = node.Widgets[key];
			value.Kind = nodegraph::WidgetKind::Text;
			value.Text = text;
		}

		void Fill(nodegraph::Node &node, NodeKind kind, const Operation &operation) {
			switch (kind) {
			case NodeKind::Source:
			case NodeKind::Builtin:
			case NodeKind::Write:
				SetText(node, "name", operation.Text);
				break;
			case NodeKind::Fit:
				SetNumber(node, "size", operation.Number);
				break;
			case NodeKind::Scale:
				SetNumber(node, "x", operation.Amount.X);
				SetNumber(node, "y", operation.Amount.Y);
				SetNumber(node, "z", operation.Amount.Z);
				break;
			case NodeKind::Resize:
			case NodeKind::Rasterize:
				SetNumber(node, "width", operation.Width);
				SetNumber(node, "height", operation.Height);
				break;
			case NodeKind::Retime:
				SetNumber(node, "fps", operation.Number);
				break;
			case NodeKind::Decimate: {
				auto &value = node.Widgets["ratio"];
				value.Kind = nodegraph::WidgetKind::Slider;
				value.Number = operation.Number;
				break;
			}
			case NodeKind::Flipbook:
				SetNumber(node, "side", operation.Side);
				SetNumber(node, "frames", operation.Frames);
				SetNumber(node, "fps", operation.Number);
				break;
			default:
				break;
			}
		}

		Operation Describe(const nodegraph::Node &node, NodeKind kind) {
			Operation operation;
			switch (kind) {
			case NodeKind::Source:
				operation.Kind = OperationKind::AddSource;
				operation.Text = Read(node, "name").Text;
				break;
			case NodeKind::Builtin:
				operation.Kind = OperationKind::AddBuiltin;
				operation.Text = Read(node, "name").Text;
				break;
			case NodeKind::Write:
				operation.Kind = OperationKind::AddWrite;
				operation.Text = Read(node, "name").Text;
				break;
			case NodeKind::Fit:
				operation.Kind = OperationKind::AddFit;
				operation.Number = static_cast<float>(Read(node, "size").Number);
				break;
			case NodeKind::Scale:
				operation.Kind = OperationKind::AddScale;
				operation.Amount = {
					static_cast<float>(Read(node, "x").Number),
					static_cast<float>(Read(node, "y").Number),
					static_cast<float>(Read(node, "z").Number)
				};
				break;
			case NodeKind::Resize:
			case NodeKind::Rasterize:
				operation.Kind =
					kind == NodeKind::Resize ? OperationKind::AddResize : OperationKind::AddRasterize;
				operation.Width = Whole(node, "width");
				operation.Height = Whole(node, "height");
				break;
			case NodeKind::Retime:
				operation.Kind = OperationKind::AddRetime;
				operation.Number = static_cast<float>(Read(node, "fps").Number);
				break;
			case NodeKind::Decimate:
				operation.Kind = OperationKind::AddDecimate;
				operation.Number = static_cast<float>(Read(node, "ratio").Number);
				break;
			case NodeKind::Flipbook:
				operation.Kind = OperationKind::AddFlipbook;
				operation.Side = Whole(node, "side");
				operation.Frames = Whole(node, "frames");
				operation.Number = static_cast<float>(Read(node, "fps").Number);
				break;
			default:
				operation.Kind = OperationKind::AddNode;
				operation.Node = kind;
				break;
			}
			return operation;
		}

		// One column per step from a chain's input; a node shares its input's
		// row unless that slot in its column is taken.
		void Arrange(nodegraph::Graph &graph) {
			auto &nodes = graph.Nodes();
			std::unordered_map<nodegraph::NodeId, nodegraph::NodeId> input;
			for (const auto &link : graph.Links())
				input[link.To] = link.From;
			std::unordered_map<nodegraph::NodeId, int> depth;
			const auto depthOf = [&](nodegraph::NodeId id) {
				int steps = 0;
				for (auto at = input.find(id); at != input.end() && steps <= static_cast<int>(nodes.size());
					 at = input.find(at->second))
					++steps;
				return steps;
			};
			for (const auto &node : nodes)
				depth[node.Id] = depthOf(node.Id);
			std::vector<const nodegraph::Node *> order;
			for (const auto &node : nodes)
				order.push_back(&node);
			std::stable_sort(order.begin(), order.end(), [&](const auto *a, const auto *b) {
				return depth[a->Id] < depth[b->Id];
			});
			std::unordered_map<int, int> nextRow;
			std::unordered_map<nodegraph::NodeId, int> row;
			for (const auto *node : order) {
				const int column = depth[node->Id];
				int wanted = nextRow[column];
				if (const auto from = input.find(node->Id); from != input.end() && row.contains(from->second))
					wanted = std::max(wanted, row[from->second]);
				row[node->Id] = wanted;
				nextRow[column] = wanted + 1;
			}
			for (auto &node : nodes) {
				node.X = 40.0f + COLUMN * static_cast<float>(depth[node.Id]);
				node.Y = 40.0f + ROW * static_cast<float>(row[node.Id]);
			}
		}
	}

	void RegisterAssetPipelineNodeTypes() {
		if (nodegraph::NodeTypes::Find(TypeId(NodeKind::Source)) != nullptr) return;
		const struct {
			const char *Id;
			const char *Label;
		} data[] = {{"bake.bytes", "Bytes"}, {"bake.mesh", "Mesh"}, {"bake.texture", "Texture"}};
		for (const auto &item : data) {
			nodegraph::DataType type;
			type.Id = item.Id;
			type.Label = item.Label;
			type.Tint = {1, 1, 1, 1};
			nodegraph::DataTypes::Register(type);
		}
		for (int ordinal = 0; ordinal <= static_cast<int>(LAST_KIND); ++ordinal) {
			const auto kind = static_cast<NodeKind>(ordinal);
			const auto *schema = engine::bake::NodeSchemaFor(kind);
			if (schema == nullptr) continue;
			nodegraph::NodeType type;
			type.Id = PREFIX + schema->Id;
			type.Title = Title(kind);
			type.Category = ASSET_PIPELINE_CATEGORY;
			type.Accent = {1, 1, 1, 1};
			type.Width = 190.0f;
			for (const auto &port : schema->Ports) {
				// `any` in the bake schema is the canvas wildcard; `bake::Build`
				// is what refuses a mesh step fed a texture.
				const std::string portType = port.ValueType == "any" ? nodegraph::ANY_TYPE : port.ValueType;
				auto &ports =
					port.Direction == engine::graphcore::PortDirection::Input ? type.Inputs : type.Outputs;
				ports.push_back(nodegraph::Port(port.Id, portType));
			}
			type.Widgets = Widgets(kind);
			nodegraph::NodeTypes::Register(type);
		}
	}

	bool LoadAssetPipelineGraph(
		const engine::bake::Document &document, nodegraph::Graph &graph, std::string &problem
	) {
		RegisterAssetPipelineNodeTypes();
		graph = {};
		std::vector<nodegraph::NodeId> placed;
		bool clean = true;
		for (const Operation &operation : document.Operations()) {
			if (operation.Kind == OperationKind::Connect) continue;
			const auto kind = KindOf(operation);
			const std::string type = kind ? TypeId(*kind) : std::string{};
			const nodegraph::NodeId id = type.empty() ? nodegraph::NO_NODE : graph.Add(type, 0.0f, 0.0f);
			if (id == nodegraph::NO_NODE) {
				problem = "operation " + std::string(engine::bake::Describe(operation.Kind)) +
						  " has no canvas node";
				return false;
			}
			Fill(*graph.Find(id), *kind, operation);
			placed.push_back(id);
		}
		for (const Operation &operation : document.Operations()) {
			if (operation.Kind != OperationKind::Connect) continue;
			if (operation.From == 0 || operation.To == 0 || operation.From > placed.size() ||
				operation.To > placed.size()) {
				problem = "a wire names node " + std::to_string(std::max(operation.From, operation.To)) +
						  " of " + std::to_string(placed.size());
				return false;
			}
			const auto made =
				graph.Connect(placed[operation.From - 1], "output", placed[operation.To - 1], "input");
			if (made != nodegraph::LinkResult::Made && clean) {
				problem = "wire " + std::to_string(operation.From) + " to " + std::to_string(operation.To) +
						  " dropped: " + nodegraph::Describe(made);
				clean = false;
			}
		}
		Arrange(graph);
		return clean;
	}

	bool SaveAssetPipelineGraph(
		const nodegraph::Graph &graph, engine::bake::Document &document, std::string &problem
	) {
		document.Clear();
		std::unordered_map<nodegraph::NodeId, uint32_t> position;
		for (const auto &node : graph.Nodes()) {
			const auto kind = KindOf(node.Type);
			if (!kind) {
				problem = "'" + (node.Label.empty() ? node.Type : node.Label) +
						  "' is not an asset pipeline node; expand groups before saving";
				return false;
			}
			document.Record(Describe(node, *kind));
			position[node.Id] = static_cast<uint32_t>(document.NodeCount());
		}
		for (const auto &link : graph.Links()) {
			Operation wire;
			wire.Kind = OperationKind::Connect;
			wire.From = position[link.From];
			wire.To = position[link.To];
			document.Record(wire);
		}
		return true;
	}

	bool BakeAssetPipeline(
		const engine::bake::Document &document,
		const std::filesystem::path &rawRoot,
		const std::filesystem::path &bakedRoot,
		std::vector<engine::bake::BakedAsset> &baked,
		std::string &status
	) {
		baked.clear();
		std::vector<std::vector<std::byte>> held;
		const engine::bake::SourceResolver sources = [&](std::string_view name) {
			std::ifstream file(rawRoot / std::string(name), std::ios::binary);
			if (!file) return std::span<const std::byte>{};
			const std::vector<char> raw{std::istreambuf_iterator<char>(file), {}};
			auto &bytes = held.emplace_back(raw.size());
			std::memcpy(bytes.data(), raw.data(), raw.size());
			return std::span<const std::byte>(bytes);
		};
		engine::bake::Graph graph;
		std::string offender;
		const auto built = engine::bake::Build(document, graph, sources, offender);
		if (built != engine::bake::DocumentStatus::Ok) {
			status = std::string("bake: ") + engine::bake::Describe(built) + " at " + offender;
			return false;
		}
		std::string failure;
		if (!graph.Run(failure)) {
			status = "bake: " + failure;
			return false;
		}
		std::error_code ignored;
		std::filesystem::create_directories(bakedRoot, ignored);
		for (const auto &asset : graph.Baked()) {
			std::ofstream out(bakedRoot / asset.Name, std::ios::binary);
			out.write(
				reinterpret_cast<const char *>(asset.Bytes.data()),
				static_cast<std::streamsize>(asset.Bytes.size())
			);
			if (!out) {
				status = "bake: could not write " + asset.Name;
				return false;
			}
			baked.push_back(asset);
		}
		status = "baked " + std::to_string(baked.size()) + " asset(s)";
		return true;
	}
}
