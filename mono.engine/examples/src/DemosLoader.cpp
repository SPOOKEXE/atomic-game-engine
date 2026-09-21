#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>

#include <algorithm>
#include <iterator>
#include <system_error>
#include <utility>

namespace engine::examples {

	namespace {
		constexpr std::string_view DEMOS_DIRECTORY = "examples";
		constexpr std::string_view SCRIPTS_DIRECTORY = "scripts";
		constexpr std::string_view WORLDS_DIRECTORY = "worlds";

		bool IsListed(DemoKind kind, const std::filesystem::path &path) {
			return kind == DemoKind::Script ? path.extension() == ".luau" : path.extension() == ".aworld";
		}

		bool IsSupported(DemoKind kind, const std::filesystem::path &path) {
			if (kind == DemoKind::World) {
				return path.extension() == ".aworld";
			}
			return path.extension() == ".luau" || path.extension() == ".js";
		}

		bool IsLeaf(std::string_view name) {
			if (name.empty()) {
				return false;
			}
			const std::filesystem::path path(name);
			return !path.is_absolute() && path.filename() == path && path != "." && path != "..";
		}
	}

	DemosLoader::DemosLoader(std::filesystem::path root)
		: Root_(root.empty() ? DefaultRoot() : std::move(root)) {}

	const std::filesystem::path &DemosLoader::Root() const {
		return Root_;
	}

	std::filesystem::path DemosLoader::Directory(DemoKind kind) const {
		return Root_ / (kind == DemoKind::Script ? SCRIPTS_DIRECTORY : WORLDS_DIRECTORY);
	}

	std::filesystem::path DemosLoader::Resolve(DemoKind kind, std::string_view name) const {
		if (!IsLeaf(name) || !IsSupported(kind, std::filesystem::path(name))) {
			return {};
		}

		const std::filesystem::path path = Directory(kind) / std::filesystem::path(name);
		std::error_code failure;
		return std::filesystem::is_regular_file(path, failure) ? path : std::filesystem::path{};
	}

	std::vector<DemoEntry> DemosLoader::List(DemoKind kind) const {
		std::vector<DemoEntry> found;
		std::error_code failure;
		for (std::filesystem::directory_iterator entries(Directory(kind), failure), end;
			 entries != end && !failure;
			 entries.increment(failure)) {
			if (!entries->is_regular_file(failure) || !IsListed(kind, entries->path())) {
				continue;
			}
			found.push_back(
				DemoEntry{
					.Name = entries->path().filename().string(),
					.Kind = kind,
					.Path = entries->path(),
				}
			);
		}

		std::sort(found.begin(), found.end(), [](const DemoEntry &left, const DemoEntry &right) {
			return left.Name < right.Name;
		});
		return found;
	}

	std::vector<DemoEntry> DemosLoader::List() const {
		std::vector<DemoEntry> found = List(DemoKind::Script);
		std::vector<DemoEntry> worlds = List(DemoKind::World);
		found.insert(
			found.end(), std::make_move_iterator(worlds.begin()), std::make_move_iterator(worlds.end())
		);
		return found;
	}

	std::optional<DemoEntry> DemosLoader::Find(DemoKind kind, std::string_view name) const {
		const std::filesystem::path path = Resolve(kind, name);
		if (path.empty() || !IsListed(kind, path)) {
			return std::nullopt;
		}
		return DemoEntry{.Name = std::string(name), .Kind = kind, .Path = path};
	}

	std::filesystem::path DemosLoader::DefaultRoot() {
		const std::filesystem::path preferred = core::Paths::Assets() / DEMOS_DIRECTORY;
		std::error_code failure;
		if (std::filesystem::is_directory(preferred, failure)) {
			return preferred;
		}

		const std::filesystem::path sibling = core::Paths::Base().parent_path() / "assets" / DEMOS_DIRECTORY;
		failure.clear();
		return std::filesystem::is_directory(sibling, failure) ? sibling : preferred;
	}
}
