#pragma once

// Discovers baked assets staged with the engine examples.
//
// @tier L10 · shared

#include <filesystem>
#include <string>
#include <vector>

namespace engine::examples {

	// One engine-owned baked asset a program may make resident without a CDN.
	struct PackagedAsset {
		// The staged file to read.
		std::filesystem::path Path;

		// Its stable name inside the examples tree.
		std::string Name;
	};

	// Lists every baked asset staged below the examples root, in name order.
	//
	// Scripts and worlds use `DemosLoader`; this is their sibling for assets
	// programs register directly with their own runtime intake.
	std::vector<PackagedAsset> PackagedAssets(std::filesystem::path root = {});
}
