#pragma once

#include <engine/procedural/Region.hpp>

#include <vector>

namespace engine::procedural::detail {

	struct QueryView {
		const SourceQuery *Villages = nullptr;
		const SourceQuery *Entrances = nullptr;
		std::vector<const SpatialLeaf *> VillageLeaves{};
		std::vector<const SpatialLeaf *> EntranceLeaves{};
		std::vector<const SplineFact *> SplineLeaves{};
		std::vector<const OverrideFact *> OverrideLeaves{};
	};

	bool Contains(Bounds bounds, Point point);
	Status
	ValidateQueries(const RegionRequest &request, Bounds required, QueryView &out, Diagnostic &diagnostic);
	Status ValidateAuthoredInputs(
		const RegionRequest &,
		Bounds owner,
		Bounds required,
		const ForestVillageSettings &,
		QueryView &,
		Diagnostic &
	);
	bool Intersects(Bounds left, Bounds right);
	bool OverrideTarget(std::string_view, PlacementKind &, Point &cell);
}
