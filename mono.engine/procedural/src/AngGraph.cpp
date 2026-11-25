#include <engine/procedural/AngGraph.hpp>
#include <engine/procedural/Region.hpp>

#include <limits>
#include <string>
#include <type_traits>

namespace engine::procedural {

	namespace recipe_graph_detail {
		constexpr const char *RECIPE_KIND = "procedural.recipe";

		ang::Value ParamValue(const ParameterData &data) {
			return std::visit(
				[](const auto &held) -> ang::Value {
					using Held = std::decay_t<decltype(held)>;
					if constexpr (std::is_same_v<Held, uint64_t>) {
						// A seed past the signed range is kept exactly, as text.
						if (held > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							return ang::Value(std::to_string(held));
						}
						return ang::Value(static_cast<int64_t>(held));
					} else if constexpr (std::is_same_v<Held, StableText>) {
						return ang::Value(held.Value);
					} else if constexpr (std::is_same_v<Held, ByteBuffer>) {
						return ang::Value(std::to_string(held.Value.size()) + " bytes");
					} else if constexpr (std::is_same_v<Held, Vector2>) {
						return ang::Value(
							core::Vector2{
								static_cast<float>(held.Values[0]), static_cast<float>(held.Values[1])
							}
						);
					} else if constexpr (std::is_same_v<Held, Vector3>) {
						return ang::Value(
							core::Vector3{
								static_cast<float>(held.Values[0]),
								static_cast<float>(held.Values[1]),
								static_cast<float>(held.Values[2])
							}
						);
					} else if constexpr (std::is_same_v<Held, Vector4>) {
						return ang::Value(
							ang::Vector4{
								static_cast<float>(held.Values[0]),
								static_cast<float>(held.Values[1]),
								static_cast<float>(held.Values[2]),
								static_cast<float>(held.Values[3])
							}
						);
					} else {
						return ang::Value(held);
					}
				},
				data
			);
		}

		ang::Registry BuildTypes() {
			ang::Registry types = ang::Registry::WithScalars();
			types.DefineNode({.Id = RECIPE_KIND, .Category = "Procedural", .Label = "Recipe"});
			for (const graphcore::NodeSchema &schema : ForestVillageNodeSchemas()) {
				ang::NodeDef def{.Id = schema.Id, .Category = "Procedural"};
				for (const graphcore::PortSchema &port : schema.Ports) {
					if (types.FindType(port.ValueType) == nullptr) {
						types.DefineType({.Id = port.ValueType});
					}
					const bool input = port.Direction == graphcore::PortDirection::Input;
					(input ? def.Inputs : def.Outputs)
						.push_back(
							{.Name = port.Id,
							 .Type = port.ValueType,
							 .Dir = input ? ang::Direction::Input : ang::Direction::Output,
							 .Optional = port.Optional}
						);
				}
				types.DefineNode(std::move(def));
			}
			return types;
		}
	}

	const ang::Registry &RecipeGraphTypes() {
		static const ang::Registry TYPES = recipe_graph_detail::BuildTypes();
		return TYPES;
	}

	bool ToAngGraph(const Recipe &recipe, ang::Graph &out) {
		using namespace recipe_graph_detail;
		constexpr float COLUMN = 280.0f;
		std::vector<ang::NodeInstance> nodes;
		ang::NodeInstance owner{.Id = "recipe", .Def = RECIPE_KIND, .Position = {0.0f, -200.0f}};
		for (const Parameter &parameter : recipe.Parameters) {
			owner.Params[parameter.Name] = ParamValue(parameter.Value);
		}
		owner.Params["name"] = ang::Value(recipe.Name);
		owner.Params["generator"] = ang::Value(recipe.Generator);
		owner.Params["root_seed"] = ParamValue(recipe.RootSeed);
		nodes.push_back(std::move(owner));
		for (const OperationNode &node : recipe.Nodes) {
			ang::NodeInstance placed{
				.Id = node.Id,
				.Def = node.Kind,
				.Position = {COLUMN * static_cast<float>(nodes.size() - 1), 0.0f}
			};
			for (const Parameter &parameter : node.Parameters) {
				placed.Params[parameter.Name] = ParamValue(parameter.Value);
			}
			nodes.push_back(std::move(placed));
		}
		std::vector<ang::Link> links;
		for (const OperationLink &link : recipe.Links) {
			links.push_back({link.FromNode, link.FromPort, link.ToNode, link.ToPort});
		}
		return out.Load(std::move(nodes), std::move(links)).Ok;
	}
}
