#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/PackagedAssets.hpp>

#include <algorithm>
#include <system_error>

namespace engine::examples {

	std::vector<PackagedAsset> PackagedAssets(std::filesystem::path root) {
		if (root.empty()) root = DemosLoader().Root();

		std::vector<PackagedAsset> found;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator entries(root, error), end;
			 entries != end && !error;
			 entries.increment(error)) {
			if (!entries->is_regular_file(error)) continue;
			const std::filesystem::path relative = entries->path().lexically_relative(root);
			if (relative.extension() != ".atex" && relative.extension() != ".amesh") continue;
			found.push_back({.Path = entries->path(), .Name = relative.generic_string()});
		}
		std::sort(found.begin(), found.end(), [](const PackagedAsset &left, const PackagedAsset &right) {
			return left.Name < right.Name;
		});
		return found;
	}
}
