#include "AssetPipelineGraph.hpp"

#include <engine/bake/GraphDocument.hpp>
#include <engine/bakegraph/Schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

namespace studio {
	namespace {
		using engine::bake::NodeKind;
		using engine::bake::Operation;
		using engine::bake::OperationKind;
		namespace ang = engine::ang;

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

		ang::ParamDef Text(const char *name, const char *label, const char *fallback) {
			return {
				.Name = name,
				.Type = "string",
				.Default = ang::Value(fallback),
				.Label = label,
				.Widget = "text"
			};
		}

		ang::ParamDef Number(const char *name, const char *label, double fallback) {
			return {
				.Name = name,
				.Type = "number",
				.Default = ang::Value(fallback),
				.Label = label,
				.Widget = "number"
			};
		}

		std::vector<ang::ParamDef> Params(NodeKind kind) {
			switch (kind) {
			case NodeKind::Source:
				return {Text("name", "Raw file", "")};
			case NodeKind::Builtin:
				return {Text("name", "Built-in", "cube")};
			case NodeKind::Fit:
				return {Number("size", "Size (m)", 1.0)};
			case NodeKind::Scale:
				return {Number("x", "X", 1.0), Number("y", "Y", 1.0), Number("z", "Z", 1.0)};
			case NodeKind::Resize:
			case NodeKind::Rasterize:
				return {Number("width", "Width", 256), Number("height", "Height", 256)};
			case NodeKind::Retime:
				return {Number("fps", "Frames/s", 30)};
			case NodeKind::Decimate: {
				ang::ParamDef keep = Number("ratio", "Keep", 0.5);
				keep.Widget = "slider";
				keep.Minimum = 0.01;
				keep.Maximum = 1.0;
				return {keep};
			}
			case NodeKind::Flipbook:
				return {
					Number("side", "Grid side", 4),
					Number("frames", "Frames", 16),
					Number("fps", "Frames/s", 12)
				};
			case NodeKind::Write:
				return {Text("name", "Asset name", "")};
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

		std::string TextOf(const ang::NodeInstance &node, const char *key) {
			const ang::Value value = AssetPipelineParam(node, key);
			return value.Get<std::string>() != nullptr ? *value.Get<std::string>() : std::string();
		}

		double NumberOf(const ang::NodeInstance &node, const char *key) {
			return AssetPipelineParam(node, key).AsNumber();
		}

		uint32_t Whole(const ang::NodeInstance &node, const char *key) {
			const double value = NumberOf(node, key);
			return value <= 0.0 ? 0u : static_cast<uint32_t>(std::lround(value));
		}

		void Fill(ang::NodeInstance &node, NodeKind kind, const Operation &operation) {
			const auto number = [&](const char *key, double value) { node.Params[key] = ang::Value(value); };
			switch (kind) {
			case NodeKind::Source:
			case NodeKind::Builtin:
			case NodeKind::Write:
				node.Params["name"] = ang::Value(operation.Text);
				break;
			case NodeKind::Fit:
			case NodeKind::Retime:
				number(kind == NodeKind::Fit ? "size" : "fps", operation.Number);
				break;
			case NodeKind::Scale:
				number("x", operation.Amount.X);
				number("y", operation.Amount.Y);
				number("z", operation.Amount.Z);
				break;
			case NodeKind::Resize:
			case NodeKind::Rasterize:
				number("width", operation.Width);
				number("height", operation.Height);
				break;
			case NodeKind::Decimate:
				number("ratio", operation.Number);
				break;
			case NodeKind::Flipbook:
				number("side", operation.Side);
				number("frames", operation.Frames);
				number("fps", operation.Number);
				break;
			default:
				break;
			}
		}

		Operation Describe(const ang::NodeInstance &node, NodeKind kind) {
			Operation operation;
			switch (kind) {
			case NodeKind::Source:
				operation.Kind = OperationKind::AddSource;
				operation.Text = TextOf(node, "name");
				break;
			case NodeKind::Builtin:
				operation.Kind = OperationKind::AddBuiltin;
				operation.Text = TextOf(node, "name");
				break;
			case NodeKind::Write:
				operation.Kind = OperationKind::AddWrite;
				operation.Text = TextOf(node, "name");
				break;
			case NodeKind::Fit:
				operation.Kind = OperationKind::AddFit;
				operation.Number = static_cast<float>(NumberOf(node, "size"));
				break;
			case NodeKind::Scale:
				operation.Kind = OperationKind::AddScale;
				operation.Amount = {
					static_cast<float>(NumberOf(node, "x")),
					static_cast<float>(NumberOf(node, "y")),
					static_cast<float>(NumberOf(node, "z"))
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
				operation.Number = static_cast<float>(NumberOf(node, "fps"));
				break;
			case NodeKind::Decimate:
				operation.Kind = OperationKind::AddDecimate;
				operation.Number = static_cast<float>(NumberOf(node, "ratio"));
				break;
			case NodeKind::Flipbook:
				operation.Kind = OperationKind::AddFlipbook;
				operation.Side = Whole(node, "side");
				operation.Frames = Whole(node, "frames");
				operation.Number = static_cast<float>(NumberOf(node, "fps"));
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
		void Arrange(std::vector<ang::NodeInstance> &nodes, const std::vector<ang::Link> &links) {
			std::unordered_map<std::string, std::string> input;
			for (const auto &link : links)
				input[link.ToNode] = link.FromNode;
			std::unordered_map<std::string, int> depth;
			const auto depthOf = [&](const std::string &id) {
				int steps = 0;
				for (auto at = input.find(id); at != input.end() && steps <= static_cast<int>(nodes.size());
					 at = input.find(at->second))
					++steps;
				return steps;
			};
			for (const auto &node : nodes)
				depth[node.Id] = depthOf(node.Id);
			std::vector<const ang::NodeInstance *> order;
			for (const auto &node : nodes)
				order.push_back(&node);
			std::stable_sort(order.begin(), order.end(), [&](const auto *a, const auto *b) {
				return depth[a->Id] < depth[b->Id];
			});
			std::unordered_map<int, int> nextRow;
			std::unordered_map<std::string, int> row;
			for (const auto *node : order) {
				const int column = depth[node->Id];
				int wanted = nextRow[column];
				if (const auto from = input.find(node->Id); from != input.end() && row.contains(from->second))
					wanted = std::max(wanted, row[from->second]);
				row[node->Id] = wanted;
				nextRow[column] = wanted + 1;
			}
			for (auto &node : nodes) {
				node.Position = {
					40.0f + COLUMN * static_cast<float>(depth[node.Id]),
					40.0f + ROW * static_cast<float>(row[node.Id])
				};
			}
		}

		ang::Registry BuildRegistry() {
			ang::Registry types;
			for (const auto &[id, label] :
				 {std::pair{"bake.bytes", "Bytes"}, {"bake.mesh", "Mesh"}, {"bake.texture", "Texture"}})
				types.DefineType({.Id = id, .Label = label});
			for (int ordinal = 0; ordinal <= static_cast<int>(LAST_KIND); ++ordinal) {
				const auto kind = static_cast<NodeKind>(ordinal);
				const auto *schema = engine::bake::NodeSchemaFor(kind);
				if (schema == nullptr) continue;
				ang::NodeDef def{
					.Id = PREFIX + schema->Id,
					.Category = ASSET_PIPELINE_CATEGORY,
					.Label = Title(kind),
				};
				for (const auto &port : schema->Ports) {
					// `any` in the bake schema is the ang wildcard; `bake::Build`
					// is what refuses a mesh step fed a texture.
					const bool in = port.Direction == engine::graphcore::PortDirection::Input;
					(in ? def.Inputs : def.Outputs)
						.push_back(
							{.Name = port.Id,
							 .Type = port.ValueType,
							 .Dir = in ? ang::Direction::Input : ang::Direction::Output}
						);
				}
				def.Params = Params(kind);
				types.DefineNode(std::move(def));
			}
			return types;
		}
	}

	const ang::Registry &AssetPipelineTypes() {
		static const ang::Registry types = BuildRegistry();
		return types;
	}

	ang::Value AssetPipelineParam(const ang::NodeInstance &node, std::string_view key) {
		if (const auto held = node.Params.find(std::string(key)); held != node.Params.end())
			return held->second;
		if (const auto def = AssetPipelineTypes().FindNode(node.Def))
			if (const ang::ParamDef *param = def->FindParam(key)) return param->Default;
		return {};
	}

	bool
	LoadAssetPipelineGraph(const engine::bake::Document &document, ang::Graph &graph, std::string &problem) {
		const ang::Registry &types = AssetPipelineTypes();
		graph = {};
		std::vector<ang::NodeInstance> nodes;
		ang::Graph probe;
		for (const Operation &operation : document.Operations()) {
			if (operation.Kind == OperationKind::Connect) continue;
			const auto kind = KindOf(operation);
			const std::string type = kind ? TypeId(*kind) : std::string{};
			if (type.empty() || types.FindNode(type) == nullptr) {
				problem = "operation " + std::string(engine::bake::Describe(operation.Kind)) +
						  " has no canvas node";
				return false;
			}
			ang::NodeInstance node{.Id = probe.NewNodeId(type), .Def = type};
			Fill(node, *kind, operation);
			probe.Apply(ang::edit::AddNode{node});
			nodes.push_back(std::move(node));
		}
		bool clean = true;
		std::vector<ang::Link> links;
		for (const Operation &operation : document.Operations()) {
			if (operation.Kind != OperationKind::Connect) continue;
			if (operation.From == 0 || operation.To == 0 || operation.From > nodes.size() ||
				operation.To > nodes.size()) {
				problem = "a wire names node " + std::to_string(std::max(operation.From, operation.To)) +
						  " of " + std::to_string(nodes.size());
				return false;
			}
			const ang::Link link{nodes[operation.From - 1].Id, "output", nodes[operation.To - 1].Id, "input"};
			const ang::LinkVerdict verdict = ang::CheckNewLink(probe, types, link);
			if (!verdict.Ok) {
				if (clean) {
					problem = "wire " + std::to_string(operation.From) + " to " +
							  std::to_string(operation.To) + " dropped: " + verdict.Reason;
				}
				clean = false;
				continue;
			}
			probe.Apply(ang::edit::AddLink{link});
			links.push_back(link);
		}
		Arrange(nodes, links);
		std::vector<ang::Edit> edits;
		for (ang::NodeInstance &node : nodes)
			edits.push_back(ang::edit::AddNode{std::move(node)});
		for (const ang::Link &link : links)
			edits.push_back(ang::edit::AddLink{link});
		const ang::EditResult made = graph.Apply(std::span<const ang::Edit>(edits));
		if (!made.Ok) {
			problem = made.Error;
			graph = {};
			return false;
		}
		return clean;
	}

	bool
	SaveAssetPipelineGraph(const ang::Graph &graph, engine::bake::Document &document, std::string &problem) {
		document.Clear();
		std::unordered_map<std::string, uint32_t> position;
		for (const auto &node : graph.Nodes()) {
			const auto kind = KindOf(node.Def);
			if (!kind) {
				problem = "'" + node.Id + "' is not an asset pipeline node";
				return false;
			}
			document.Record(Describe(node, *kind));
			position[node.Id] = static_cast<uint32_t>(document.NodeCount());
		}
		for (const auto &link : graph.Links()) {
			Operation wire;
			wire.Kind = OperationKind::Connect;
			wire.From = position[link.FromNode];
			wire.To = position[link.ToNode];
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
