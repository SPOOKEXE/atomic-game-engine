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
		//
		// **`.amesh` and `.atex` are the baked forms and the rest are sources.**
		// Both sides are in one table on purpose: a publisher pointed at a
		// source tree and one pointed at a baked tree must classify the same
		// way, or the kind-filtered request that fetches "every mesh" would
		// return a different set depending on which was published. What is
		// *inside* an entry is the importer's problem — `bake::ReadModel` is the
		// one thing that reads a `.glb` — and this table has never claimed
		// otherwise.
		constexpr std::array<std::pair<std::string_view, AssetKind>, 39> EXTENSIONS{{
			{"amesh", AssetKind::Mesh},
			{"mesh", AssetKind::Mesh},
			{"glb", AssetKind::Mesh},
			{"gltf", AssetKind::Mesh},
			{"obj", AssetKind::Mesh},
			{"fbx", AssetKind::Mesh},
			{"pmx", AssetKind::Mesh},

			{"atex", AssetKind::Texture},
			{"png", AssetKind::Texture},
			{"jpg", AssetKind::Texture},
			{"jpeg", AssetKind::Texture},
			{"bmp", AssetKind::Texture},
			{"tga", AssetKind::Texture},
			{"ktx2", AssetKind::Texture},
			{"dds", AssetKind::Texture},
			{"basis", AssetKind::Texture},

			// **A texture, because that is what one becomes.** `bake` lays a
			// GIF's frames out as a grid in a single image and records the side,
			// the count and the rate — see `assets::TextureData::FlipbookSide` —
			// so everything downstream of the decoder handles it as the one image
			// it now is. Naming a separate kind here would need a second route
			// through the client's content pump to arrive at the same table.
			//
			// It was in `IsRuntimeReadable`'s source list and missing from this
			// one, which is the half that decides what a publish records: a `.gif`
			// went to the CDN as `Unknown` and `client::Client` never looked at
			// it, so the decoder that already existed could not be reached.
			{"gif", AssetKind::Texture},

			// **A texture, for the same reason `.gif` is one**: `bake`
			// rasterises the drawing at a size the pipeline names and what
			// arrives is an ordinary `.atex`. Nothing downstream knows it was
			// ever a vector, which is the point — a runtime holds no rasteriser
			// any more than it holds a PNG decoder.
			{"svg", AssetKind::Texture},

			{"wav", AssetKind::Audio},
			{"ogg", AssetKind::Audio},
			{"flac", AssetKind::Audio},
			{"mp3", AssetKind::Audio},

			{"amat", AssetKind::Material},
			{"mat", AssetKind::Material},
			{"surface", AssetKind::Material},

			{"ttf", AssetKind::Font},
			{"otf", AssetKind::Font},

			{"luau", AssetKind::Script},
			{"lua", AssetKind::Script},
			{"ts", AssetKind::Script},
			{"js", AssetKind::Script},

			{"mp4", AssetKind::Video},

			{"agame", AssetKind::Data},
			{"aworld", AssetKind::Data},

			// **Source and compiled both route here.** What somebody publishes
			// is what they wrote; `bake` turns one into the other, the way it
			// does for a mesh. `spv` is what a renderer is eventually handed.
			{"spv", AssetKind::Shader},
			{"frag", AssetKind::Shader},
			{"vert", AssetKind::Shader},
			{"comp", AssetKind::Shader},
			{"glsl", AssetKind::Shader},
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
		case AssetKind::Shader:
			return "shader";
		case AssetKind::Unknown:
			break;
		}
		return "unknown";
	}

	bool IsRuntimeReadable(std::string_view name) {
		// **The source forms, listed rather than the baked ones.** There are
		// three baked extensions today and fourteen sources, but the list that
		// must not go stale is this one: a format added to the extension table
		// without a row here would be offered as loadable and would not load,
		// which is the failure this function exists to end. A baked form added
		// without a row here is simply offered, which is correct.
		static constexpr std::string_view SOURCES[] = {
			// Models, all of which bake to `.amesh`.
			"glb",
			"gltf",
			"obj",
			"fbx",
			"pmx",

			// Images, all of which bake to `.atex`. `.gif` is here and its
			// baked form is an ordinary flipbook sheet — `bake/Gif.cpp`.
			// `.svg` is here and its baked form is the rasterisation `bake`
			// made at the size the pipeline asked for: a runtime handed the
			// markup would need an XML parser and a rasteriser to draw an icon,
			// which is exactly the work a bake does once.
			"png",
			"jpg",
			"jpeg",
			"bmp",
			"gif",
			"svg",
			"tga",

			// Material descriptions, which bake to `.amat`.
			"mat",
			"surface",

			// Shader sources, which bake to `.spv`. A renderer is handed a
			// compiled module and holds no compiler — see
			// `Renderer::AddShader` — so GLSL reaching a runtime is a mistake
			// that has to be caught here rather than at the draw.
			"frag",
			"vert",
			"comp",
			"glsl",
		};

		const size_t dot = name.find_last_of('.');
		if (dot == std::string_view::npos || dot + 1 >= name.size()) {
			// No extension at all. Not a source this baker knows, so it is
			// whatever it is — the same answer `KindOfName` gives it.
			return true;
		}

		std::string extension(name.substr(dot + 1));
		std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
			return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
		});

		for (const std::string_view source : SOURCES) {
			if (extension == source) {
				return false;
			}
		}
		return true;
	}

	AssetKind KindFromName(std::string_view text) {
		for (uint8_t value = 1; value <= static_cast<uint8_t>(AssetKind::Shader); ++value) {
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
