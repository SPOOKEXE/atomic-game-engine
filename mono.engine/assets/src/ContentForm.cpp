#include <engine/assets/ContentForm.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace engine::assets {
	namespace {
		// The one extension table in the engine.
		//
		// Deliberately a table rather than a chain of comparisons: adding a
		// format is a row, and a row is what a reviewer can check against the
		// importer that will read it.
		//
		// **Three columns because there were two lists.** `Kind` is what a
		// manifest records and what a client routes on; `Source` is whether a
		// baker still has to convert it, which used to be a second list beside
		// this one with a comment saying it was the one that goes stale.
		//
		// `.agame` and `.aworld` are here because the studio already writes them
		// and a game file is content a client fetches like any other.
		struct Row {
			ContentForm Form;
			std::string_view Name;
			AssetKind Kind;
			bool Source;
		};

		// The canonical name first for each form, so `Describe` can take the
		// first row it finds - `.jpg` and `.jpeg` are one form and a setting is
		// written with one of them.
		//
		// **The size is deduced and never written down.** A declared count that
		// is one too large leaves a value-initialised row on the end whose form
		// is `Unknown` and whose name is a null pointer, so `Describe(Unknown)`
		// finds it and hands `strlen` a null - which is how this table
		// segfaulted the first time it was written.
		constexpr Row ROWS[] = {
			{ContentForm::AMesh, "amesh", AssetKind::Mesh, false},
			{ContentForm::Mesh, "mesh", AssetKind::Mesh, false},
			{ContentForm::Glb, "glb", AssetKind::Mesh, true},
			{ContentForm::Gltf, "gltf", AssetKind::Mesh, true},
			{ContentForm::Obj, "obj", AssetKind::Mesh, true},
			{ContentForm::Fbx, "fbx", AssetKind::Mesh, true},
			{ContentForm::Pmx, "pmx", AssetKind::Mesh, true},

			{ContentForm::ATex, "atex", AssetKind::Texture, false},
			{ContentForm::Png, "png", AssetKind::Texture, true},
			{ContentForm::Jpeg, "jpeg", AssetKind::Texture, true},
			{ContentForm::Jpeg, "jpg", AssetKind::Texture, true},
			{ContentForm::Bmp, "bmp", AssetKind::Texture, true},
			{ContentForm::Tga, "tga", AssetKind::Texture, true},

			// Already compressed for a GPU, and not this engine's to bake.
			{ContentForm::Ktx2, "ktx2", AssetKind::Texture, false},
			{ContentForm::Dds, "dds", AssetKind::Texture, false},
			{ContentForm::Basis, "basis", AssetKind::Texture, false},

			// **A texture, because that is what one becomes.** `bake` lays a
			// GIF's frames out as a grid in a single image and records the side,
			// the count and the rate - see `assets::TextureData::FlipbookSide` -
			// so everything downstream of the decoder handles it as the one
			// image it now is. Naming a separate kind would need a second route
			// through the client's content pump to arrive at the same table.
			{ContentForm::Gif, "gif", AssetKind::Texture, true},

			// **A texture, for the same reason `.gif` is one**: `bake`
			// rasterises the drawing at a size the pipeline names and what
			// arrives is an ordinary `.atex`. Nothing downstream knows it was
			// ever a vector, which is the point - a runtime holds no rasteriser
			// any more than it holds a PNG decoder.
			{ContentForm::Svg, "svg", AssetKind::Texture, true},

			{ContentForm::Wav, "wav", AssetKind::Audio, false},
			{ContentForm::Ogg, "ogg", AssetKind::Audio, false},
			{ContentForm::Flac, "flac", AssetKind::Audio, false},
			{ContentForm::Mp3, "mp3", AssetKind::Audio, false},

			{ContentForm::AMat, "amat", AssetKind::Material, false},
			{ContentForm::Mat, "mat", AssetKind::Material, true},
			{ContentForm::Surface, "surface", AssetKind::Material, true},

			{ContentForm::Ttf, "ttf", AssetKind::Font, false},
			{ContentForm::Otf, "otf", AssetKind::Font, false},

			{ContentForm::Luau, "luau", AssetKind::Script, false},
			{ContentForm::Lua, "lua", AssetKind::Script, false},
			{ContentForm::TypeScript, "ts", AssetKind::Script, false},
			{ContentForm::JavaScript, "js", AssetKind::Script, false},

			{ContentForm::Mp4, "mp4", AssetKind::Video, false},

			{ContentForm::AGame, "agame", AssetKind::Data, false},
			{ContentForm::AWorld, "aworld", AssetKind::Data, false},

			// **Source and compiled both route to `Shader`.** What somebody
			// publishes is what they wrote; `bake` turns one into the other, the
			// way it does for a mesh. A renderer is handed a compiled module and
			// holds no compiler - see `Renderer::AddShader` - so GLSL reaching a
			// runtime is a mistake that has to be caught by the source column
			// rather than at the draw.
			{ContentForm::Spv, "spv", AssetKind::Shader, false},
			{ContentForm::Frag, "frag", AssetKind::Shader, true},
			{ContentForm::Vert, "vert", AssetKind::Shader, true},
			{ContentForm::Comp, "comp", AssetKind::Shader, true},
			{ContentForm::Glsl, "glsl", AssetKind::Shader, true},
		};

		const Row *RowOf(ContentForm form) {
			for (const Row &row : ROWS) {
				if (row.Form == form) {
					return &row;
				}
			}
			return nullptr;
		}

		// The extension, lowercased, or empty when the name has none.
		//
		// A dot in a directory component is not an extension: `v1.2/rock` has no
		// extension at all, and reading `2/rock` as one would put an asset in
		// whatever form that happened to match.
		std::string ExtensionOf(std::string_view name) {
			const size_t dot = name.find_last_of('.');
			if (dot == std::string_view::npos || dot + 1 >= name.size()) {
				return {};
			}
			if (name.find('/', dot) != std::string_view::npos) {
				return {};
			}

			std::string extension(name.substr(dot + 1));
			std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
				return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
			});
			return extension;
		}
	}

	const char *Describe(ContentForm form) {
		if (const Row *row = RowOf(form); row != nullptr) {
			// Every name in the table is a string literal, so this is a pointer
			// into static storage rather than into the view.
			return row->Name.data();
		}
		return "unknown";
	}

	ContentForm FormFromName(std::string_view text) {
		for (const Row &row : ROWS) {
			if (row.Name == text) {
				return row.Form;
			}
		}
		return ContentForm::Unknown;
	}

	ContentForm FormOfName(std::string_view name) {
		const std::string extension = ExtensionOf(name);
		if (extension.empty()) {
			return ContentForm::Unknown;
		}
		for (const Row &row : ROWS) {
			if (row.Name == extension) {
				return row.Form;
			}
		}
		return ContentForm::Unknown;
	}

	AssetKind KindOfForm(ContentForm form) {
		const Row *row = RowOf(form);
		return row == nullptr ? AssetKind::Unknown : row->Kind;
	}

	bool IsSourceForm(ContentForm form) {
		const Row *row = RowOf(form);
		return row != nullptr && row->Source;
	}

	std::span<const ContentForm> AllForms() {
		// Built once from the table, so a form added above appears here, in the
		// policy and in `--flags` with no other edit. Deduplicated, because two
		// extensions may name one form.
		static const std::vector<ContentForm> forms = [] {
			std::vector<ContentForm> distinct;
			for (const Row &row : ROWS) {
				if (std::find(distinct.begin(), distinct.end(), row.Form) == distinct.end()) {
					distinct.push_back(row.Form);
				}
			}
			return distinct;
		}();
		return forms;
	}
}
