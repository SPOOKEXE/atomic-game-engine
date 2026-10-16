#include "Encoding.hpp"
#include "Stages.hpp"

#include <engine/graphcore/Compile.hpp>
#include <engine/procedural/Region.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <new>
#include <utility>

namespace engine::procedural {

	namespace {
		using detail::Stage;

		struct Field {
			Stage Owner;
			const char *Name;
			uint64_t Minimum;
			uint64_t Maximum;
			uint64_t Default;
		};

		constexpr std::array<const char *, 6> KINDS{
			"region.seed",
			"village.sources",
			"village.entrances",
			"road.spline",
			"village.lots",
			"forest.scatter"
		};

		constexpr std::array<Field, 13> FIELDS{{
			{Stage::Forest, "density_ppm", 0, 1000000, 300000},
			{Stage::VillageLots, "density_ppm", 0, 1000000, 400000},
			{Stage::Forest, "min_distance_cells", 1, 8, 2},
			{Stage::VillageLots, "min_distance_cells", 1, 8, 2},
			{Stage::VillageLots, "radius_cells", 2, 16, 8},
			{Stage::Entrance, "clearance_cells", 1, 8, 3},
			{Stage::Road, "clearance_cells", 1, 8, 2},
			{Stage::Road, "max_reach_cells", 1, 32, 24},
			{Stage::Road, "bend_cells", 0, 8, 2},
			{Stage::Forest, "variants", 1, 64, 4},
			{Stage::Forest, "materials", 1, 64, 2},
			{Stage::VillageLots, "variants", 1, 64, 3},
			{Stage::VillageLots, "materials", 1, 64, 2},
		}};

		Stage KindOf(std::string_view kind) {
			for (size_t index = 0; index < KINDS.size(); ++index)
				if (kind == KINDS[index]) return static_cast<Stage>(index);
			return static_cast<Stage>(KINDS.size());
		}

		graphcore::NodeSchema Schema(const char *kind, std::initializer_list<graphcore::PortSchema> ports) {
			return {kind, std::vector<graphcore::PortSchema>(ports), {}};
		}

		std::array<graphcore::NodeSchema, 6> Schemas() {
			using graphcore::PortDirection;
			return {
				Schema(KINDS[0], {{"scope", "region", PortDirection::Output}}),
				Schema(
					KINDS[1],
					{{"scope", "region", PortDirection::Input},
					 {"villages", "villages", PortDirection::Output}}
				),
				Schema(
					KINDS[2],
					{{"scope", "region", PortDirection::Input},
					 {"villages", "villages", PortDirection::Input},
					 {"entrances", "entrances", PortDirection::Output}}
				),
				Schema(
					KINDS[3],
					{{"villages", "villages", PortDirection::Input},
					 {"entrances", "entrances", PortDirection::Input},
					 {"roads", "roads", PortDirection::Output}}
				),
				Schema(
					KINDS[4],
					{{"scope", "region", PortDirection::Input},
					 {"villages", "villages", PortDirection::Input},
					 {"entrances", "entrances", PortDirection::Input},
					 {"roads", "roads", PortDirection::Input},
					 {"lots", "lots", PortDirection::Output}}
				),
				Schema(
					KINDS[5],
					{{"scope", "region", PortDirection::Input},
					 {"villages", "villages", PortDirection::Input},
					 {"roads", "roads", PortDirection::Input},
					 {"lots", "lots", PortDirection::Input},
					 {"trees", "placements", PortDirection::Output}}
				)
			};
		}

		bool ReadSettings(const Recipe &recipe, ForestVillageSettings &settings, Diagnostic &diagnostic) {
			if (!recipe.Parameters.empty() || recipe.Nodes.size() != KINDS.size()) {
				detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.nodes");
				return false;
			}
			std::array<const OperationNode *, KINDS.size()> nodes{};
			for (const OperationNode &node : recipe.Nodes) {
				const Stage kind = KindOf(node.Kind);
				if (kind == static_cast<Stage>(KINDS.size()) || nodes[static_cast<size_t>(kind)] != nullptr) {
					detail::Refuse(diagnostic, Status::InvalidRecipe, node.Kind);
					return false;
				}
				nodes[static_cast<size_t>(kind)] = &node;
			}
			if (!nodes[static_cast<size_t>(Stage::Seed)]->Parameters.empty()) {
				detail::Refuse(diagnostic, Status::InvalidRecipe, KINDS[0]);
				return false;
			}
			std::array<uint64_t, FIELDS.size()> values{};
			for (size_t index = 0; index < FIELDS.size(); ++index) {
				const Field &field = FIELDS[index];
				const auto &parameters = nodes[static_cast<size_t>(field.Owner)]->Parameters;
				const auto found =
					std::find_if(parameters.begin(), parameters.end(), [&](const Parameter &parameter) {
						return parameter.Name == field.Name;
					});
				if (found == parameters.end() || !std::holds_alternative<uint64_t>(found->Value)) {
					detail::Refuse(diagnostic, Status::InvalidRecipe, field.Name);
					return false;
				}
				const uint64_t value = std::get<uint64_t>(found->Value);
				if (value < field.Minimum || value > field.Maximum) {
					detail::Refuse(diagnostic, Status::InvalidRecipe, field.Name);
					return false;
				}
				values[index] = value;
			}
			for (size_t stage = 0; stage < nodes.size(); ++stage) {
				size_t expected = 0;
				for (const Field &field : FIELDS)
					if (static_cast<size_t>(field.Owner) == stage) ++expected;
				if (nodes[stage]->Parameters.size() != expected) {
					detail::Refuse(diagnostic, Status::InvalidRecipe, nodes[stage]->Id);
					return false;
				}
			}
			settings.ForestDensityPpm = static_cast<uint32_t>(values[0]);
			settings.HouseDensityPpm = static_cast<uint32_t>(values[1]);
			settings.TreeMinimumDistanceCells = static_cast<uint16_t>(values[2]);
			settings.HouseMinimumDistanceCells = static_cast<uint16_t>(values[3]);
			settings.VillageRadiusCells = static_cast<uint16_t>(values[4]);
			settings.EntranceClearanceCells = static_cast<uint16_t>(values[5]);
			settings.RoadClearanceCells = static_cast<uint16_t>(values[6]);
			settings.MaximumRoadReachCells = static_cast<uint16_t>(values[7]);
			settings.RoadBendCells = static_cast<uint16_t>(values[8]);
			settings.TreeVariants = static_cast<uint16_t>(values[9]);
			settings.TreeMaterials = static_cast<uint16_t>(values[10]);
			settings.HouseVariants = static_cast<uint16_t>(values[11]);
			settings.HouseMaterials = static_cast<uint16_t>(values[12]);
			return true;
		}
	}

	std::span<const graphcore::NodeSchema> ForestVillageNodeSchemas() {
		static const auto schemas = Schemas();
		return schemas;
	}

	Recipe ForestVillageDefaults(std::string name, uint64_t rootSeed) {
		Recipe recipe;
		recipe.Name = std::move(name);
		recipe.Generator = "forest-village.v1";
		recipe.SchemaVersion = 1;
		recipe.RootSeed = rootSeed;
		constexpr std::array<const char *, 6> IDS{
			"region", "village_sources", "entrances", "roads", "lots", "forest"
		};
		for (size_t index = 0; index < KINDS.size(); ++index)
			recipe.Nodes.push_back({IDS[index], KINDS[index], {}});
		for (const Field &field : FIELDS)
			recipe.Nodes[static_cast<size_t>(field.Owner)].Parameters.push_back({field.Name, field.Default});
		recipe.Links = {
			{"region", "scope", "village_sources", "scope"},
			{"region", "scope", "entrances", "scope"},
			{"region", "scope", "lots", "scope"},
			{"region", "scope", "forest", "scope"},
			{"village_sources", "villages", "entrances", "villages"},
			{"village_sources", "villages", "roads", "villages"},
			{"village_sources", "villages", "lots", "villages"},
			{"village_sources", "villages", "forest", "villages"},
			{"entrances", "entrances", "roads", "entrances"},
			{"entrances", "entrances", "lots", "entrances"},
			{"roads", "roads", "lots", "roads"},
			{"roads", "roads", "forest", "roads"},
			{"lots", "lots", "forest", "lots"},
		};
		return recipe;
	}

	Status CompileForestVillage(const Recipe &recipe, CompiledRecipe &out, Diagnostic &diagnostic) {
		if (recipe.Generator != "forest-village.v1" || recipe.SchemaVersion != 1 || recipe.Links.size() != 13)
			return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.graph");
		try {
			CompiledRecipe candidate;
			const Status encoded = EncodeRecipe(recipe, candidate.CanonicalBytes_, diagnostic);
			if (encoded != Status::Ok) return encoded;
			if (!ReadSettings(recipe, candidate.Settings_, diagnostic)) return diagnostic.Code;

			std::vector<const OperationNode *> ordered;
			for (const OperationNode &node : recipe.Nodes)
				ordered.push_back(&node);
			std::sort(
				ordered.begin(), ordered.end(), [](const OperationNode *left, const OperationNode *right) {
					return StableNameLess(left->Id, right->Id);
				}
			);
			std::vector<graphcore::Node> nodes;
			for (const OperationNode *node : ordered)
				nodes.push_back({node->Id, node->Kind});
			std::vector<graphcore::Connection> links;
			for (const OperationLink &link : recipe.Links)
				links.push_back({link.FromNode, link.FromPort, link.ToNode, link.ToPort});
			const auto schemas = ForestVillageNodeSchemas();
			graphcore::Limits limits;
			limits.MaximumNodes = KINDS.size();
			limits.MaximumConnections = 13;
			limits.MaximumSchemas = KINDS.size();
			limits.MaximumPorts = 19;
			limits.MaximumIdentityBytes = 8192;
			graphcore::CompiledGraph graph;
			graphcore::Diagnostic graphDiagnostic;
			const auto graphStatus =
				graphcore::CompileGraph(nodes, links, schemas, limits, graph, graphDiagnostic);
			if (graphStatus != graphcore::CompileStatus::Ok)
				return detail::Refuse(
					diagnostic,
					graphStatus == graphcore::CompileStatus::AllocationFailed ? Status::AllocationFailed
																			  : Status::InvalidRecipe,
					graphDiagnostic.Node.empty() ? "recipe.graph" : graphDiagnostic.Node
				);
			for (size_t index = 0; index < candidate.Stages_.size(); ++index)
				candidate.Stages_[index] =
					static_cast<uint8_t>(KindOf(ordered[graph.Dependencies.Order[index]]->Kind));
			candidate.Definition_ = recipe;
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status RequiredRegionBounds(
		const CompiledRecipe &recipe, RegionScope scope, Bounds &owner, Bounds &query, Diagnostic &diagnostic
	) {
		if (recipe.CanonicalBytes_.empty() || scope.CellsPerSide == 0 || scope.CellsPerSide > 64)
			return detail::Refuse(diagnostic, Status::InvalidInput, "region.scope");
		const auto &settings = recipe.Settings_;
		const int64_t spacing =
			std::max(settings.TreeMinimumDistanceCells, settings.HouseMinimumDistanceCells);
		const int64_t competitorMargin = spacing + 2;
		// Every source influence must extend from the competitor band, not the owner edge.
		const int64_t halo =
			competitorMargin + std::max(
								   {static_cast<int64_t>(settings.VillageRadiusCells) + spacing,
									static_cast<int64_t>(settings.EntranceClearanceCells) + 1,
									static_cast<int64_t>(settings.MaximumRoadReachCells) * 2 +
										settings.RoadBendCells + settings.RoadClearanceCells + 2}
							   );
		const int64_t minimumX = static_cast<int64_t>(scope.X) * scope.CellsPerSide;
		const int64_t minimumY = static_cast<int64_t>(scope.Y) * scope.CellsPerSide;
		const int64_t maximumX = minimumX + scope.CellsPerSide;
		const int64_t maximumY = minimumY + scope.CellsPerSide;
		owner = {
			{minimumX * SUBCELLS_PER_CELL, minimumY * SUBCELLS_PER_CELL},
			{maximumX * SUBCELLS_PER_CELL, maximumY * SUBCELLS_PER_CELL}
		};
		query = {
			{(minimumX - halo) * SUBCELLS_PER_CELL, (minimumY - halo) * SUBCELLS_PER_CELL},
			{(maximumX + halo) * SUBCELLS_PER_CELL, (maximumY + halo) * SUBCELLS_PER_CELL}
		};
		diagnostic = {};
		return Status::Ok;
	}
}
