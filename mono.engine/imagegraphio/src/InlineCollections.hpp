#pragma once

#include "ImportBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <nlohmann/json.hpp>

namespace engine::imagegraphio::detail {
	// Source inline collections retain ordinary wires and identify members separately from node.group.
	bool ProjectInlineCollections(
		const nlohmann::json &root, imagegraph::Document &document, ImportBudget &budget, std::string &failure
	);
}
