#include <engine/assets/AssetKind.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace engine::assets {
	namespace {
		// The one extension table in the engine.
		//
		// Deliberately a table rather than a chain of comparisons: adding a
		// format is a row, and a row is what a reviewer can check against the
		// importer that will read it.
		//
		// `.agame` and `.aworld` are here because the studio already writes
		// them and a game file is content a client fetches like any other.
		constexpr std::array<std::pair<std::string_view, AssetKind>, 27> EXTENSIONS{{
			{"mesh", AssetKind::Mesh},	   {"glb", AssetKind::Mesh},		 {"gltf", AssetKind::Mesh},
			{"obj", AssetKind::Mesh},	   {"fbx", AssetKind::Mesh},

			{"png", AssetKind::Texture},   {"jpg", AssetKind::Texture},		 {"jpeg", AssetKind::Texture},
			{"tga", AssetKind::Texture},   {"ktx2", AssetKind::Texture},	 {"dds", AssetKind::Texture},
			{"basis", AssetKind::Texture},

			{"wav", AssetKind::Audio},	   {"ogg", AssetKind::Audio},		 {"flac", AssetKind::Audio},
			{"mp3", AssetKind::Audio},

			{"mat", AssetKind::Material},  {"surface", AssetKind::Material},

			{"ttf", AssetKind::Font},	   {"otf", AssetKind::Font},

			{"luau", AssetKind::Script},   {"lua", AssetKind::Script},		 {"ts", AssetKind::Script},
			{"js", AssetKind::Script},

			{"mp4", AssetKind::Video},

			{"agame", AssetKind::Data},	   {"aworld", AssetKind::Data},
		}};
	}

	const char *Describe(AssetKind kind) {
		switch (kind) {
		case AssetKind::Mesh:
			return "mesh";
		case AssetKind::Texture:
			return "texture";
		case AssetKind::Audio:
			return "audio";
		case AssetKind::Material:
			return "material";
		case AssetKind::Font:
			return "font";
		case AssetKind::Script:
			return "script";
		case AssetKind::Video:
			return "video";
		case AssetKind::Data:
			return "data";
		case AssetKind::Unknown:
			break;
		}
		return "unknown";
	}

	AssetKind KindFromName(std::string_view text) {
		for (uint8_t value = 1; value <= static_cast<uint8_t>(AssetKind::Data); ++value) {
			const auto kind = static_cast<AssetKind>(value);
			if (text == Describe(kind)) {
				return kind;
			}
		}
		return AssetKind::Unknown;
	}

	AssetKind KindOfName(std::string_view name) {
		const size_t dot = name.find_last_of('.');
		if (dot == std::string_view::npos || dot + 1 >= name.size()) {
			return AssetKind::Unknown;
		}
		// A dot in a directory component is not an extension: `v1.2/rock` has
		// no extension at all, and reading `2/rock` as one would put an asset
		// in whatever kind that happened to match.
		if (name.find('/', dot) != std::string_view::npos) {
			return AssetKind::Unknown;
		}

		std::string extension(name.substr(dot + 1));
		std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
			return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
		});

		for (const auto &[suffix, kind] : EXTENSIONS) {
			if (extension == suffix) {
				return kind;
			}
		}
		return AssetKind::Unknown;
	}
}
