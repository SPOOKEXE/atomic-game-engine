#pragma once

#include <engine/procedural/Region.hpp>

#include <vector>

namespace engine::procedural::detail {

	struct QueryView {
		const SourceQuery *Villages = nullptr;
		const SourceQuery *Entrances = nullptr;
		std::vector<const SpatialLeaf *> VillageLeaves{};
		std::vector<const SpatialLeaf *> EntranceLeaves{};
	};

	bool Contains(Bounds bounds, Point point);
	Status
	ValidateQueries(const RegionRequest &request, Bounds required, QueryView &out, Diagnostic &diagnostic);
}
