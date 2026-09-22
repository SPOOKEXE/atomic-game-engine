#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ui/Fonts.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace engine::ui {

	namespace {
		// The one table that maps a role to a family.
		//
		// Swapping a family is a line here. A file name appearing anywhere else
		// is the thing this table exists to prevent.
		struct Family {
			Typeface Face;
			const char *File;
		};

		constexpr std::array<Family, static_cast<size_t>(Typeface::Count)> FAMILIES{{
			{Typeface::Interface, "Inter.ttf"},
			{Typeface::Monospace, "JetBrainsMono.ttf"},
			{Typeface::Display, "Roboto.ttf"},
			{Typeface::Fallback, "NotoSans.ttf"},
		}};

		// The point sizes behind `TextSize`.
		//
		// **13 for body, matching what imgui's default was**, so switching to a
		// real face changes the shapes and not the layout. The other two are one
		// step either side; a scale with more steps than this is a scale nobody
		// can keep to.
		constexpr std::array<float, static_cast<size_t>(TextSize::Count)> SIZES{{11.0f, 13.0f, 16.0f}};

		constexpr size_t FaceCount = static_cast<size_t>(Typeface::Count);
		constexpr size_t SizeCount = static_cast<size_t>(TextSize::Count);

		// Loaded faces, by role and size. Null for anything that would not load.
		std::array<ImFont *, FaceCount * SizeCount> Loaded{};
		ImFontAtlas *LoadedAtlas = nullptr;
		ImGuiContext *LoadedContext = nullptr;

		// This is deliberately one fixed allocation, made before the first frame.
		// The studio painter fills subrectangles in it for the canonical shaper;
		// asking ImGui for one rectangle per glyph while panels are drawing can
		// replace the atlas texture and invalidate vertices already recorded.
		constexpr int SHAPED_GLYPH_ATLAS_EXTENT = 1024;
		std::unordered_map<ImGuiContext *, ImFontAtlasRectId> ShapedGlyphRects;

		size_t IndexOf(Typeface face, TextSize size) {
			return static_cast<size_t>(face) * SizeCount + static_cast<size_t>(size);
		}
	}

	bool LoadFonts(float scale) {
		Loaded.fill(nullptr);

		ImGuiIO &io = ImGui::GetIO();
		LoadedAtlas = io.Fonts;
		LoadedContext = ImGui::GetCurrentContext();
		ImFontAtlasRectId shapedRect =
			io.Fonts->AddCustomRect(SHAPED_GLYPH_ATLAS_EXTENT, SHAPED_GLYPH_ATLAS_EXTENT);
		if (shapedRect == ImFontAtlasRectId_Invalid) {
			ENGINE_WARN("could not reserve the shaped glyph atlas rectangle");
		} else {
			ShapedGlyphRects[ImGui::GetCurrentContext()] = shapedRect;
		}
		const float factor = scale > 0.0f ? scale : 1.0f;

		const std::filesystem::path root = core::Paths::Assets() / "fonts";

		// The coverage face, read once and merged into every other. Merging
		// rather than switching is what stops a name with one non-Latin
		// character changing font mid-word.
		const std::filesystem::path fallback = root / "NotoSans.ttf";
		const bool haveFallback = std::filesystem::exists(fallback);

		bool any = false;

		for (const Family &family : FAMILIES) {
			const std::filesystem::path path = root / family.File;
			if (!std::filesystem::exists(path)) {
				continue;
			}

			for (size_t index = 0; index < SizeCount; index++) {
				const float pixels = SIZES[index] * factor;

				ImFont *font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), pixels);
				if (font == nullptr) {
					continue;
				}

				if (haveFallback && family.Face != Typeface::Fallback) {
					// **Merged, so the glyph is found rather than the box.** A
					// second face added with `MergeMode` fills in every code
					// point the first one does not have, which is the whole
					// reason a coverage font is vendored at all.
					ImFontConfig merge;
					merge.MergeMode = true;
					io.Fonts->AddFontFromFileTTF(fallback.string().c_str(), pixels, &merge);
				}

				Loaded[IndexOf(family.Face, static_cast<TextSize>(index))] = font;
				any = true;
			}
		}

		if (!any) {
			// Not fatal. An editor that refused to open over a font is worse
			// than one that opens in imgui's default - but it is worth saying
			// once, because "the text looks wrong" otherwise has no explanation
			// anywhere.
			ENGINE_WARN("no fonts found under {} - falling back to imgui's built-in face", root.string());
			return false;
		}

		// The interface face at body size is what everything gets unless it
		// asks otherwise.
		if (ImFont *body = Font(Typeface::Interface, TextSize::Body); body != nullptr) {
			io.FontDefault = body;
		}

		return true;
	}

	bool ShapedGlyphAtlasRect(ImFontAtlasRect *out) {
		if (out == nullptr) {
			return false;
		}
		ImGuiIO &io = ImGui::GetIO();
		const auto found = ShapedGlyphRects.find(ImGui::GetCurrentContext());
		return found != ShapedGlyphRects.end() && io.Fonts != nullptr &&
			   io.Fonts->GetCustomRect(found->second, out);
	}

	ImFont *Font(Typeface face, TextSize size) {
		if (face >= Typeface::Count || size >= TextSize::Count ||
			LoadedContext != ImGui::GetCurrentContext() || LoadedAtlas != ImGui::GetIO().Fonts) {
			return nullptr;
		}

		const auto live = [](ImFont *candidate) {
			if (candidate == nullptr) return false;
			for (ImFont *font : ImGui::GetIO().Fonts->Fonts) {
				if (font == candidate) return true;
			}
			return false;
		};

		ImFont *found = Loaded[IndexOf(face, size)];
		if (live(found)) return found;

		// A family that would not load falls back to the interface one at the
		// same size rather than to nothing, so a missing file costs the shapes
		// and not the layout.
		ImFont *fallback = Loaded[IndexOf(Typeface::Interface, size)];
		return live(fallback) ? fallback : nullptr;
	}

	const gui::FontPackage &GuiFontPackage() {
		static const gui::FontPackage package = [] {
			gui::FontPackage loaded;
			const std::filesystem::path root = core::Paths::Assets() / "fonts";
			constexpr std::array packageFaces{
				std::pair{"Inter.ttf", gui::FontFace::Regular},
				std::pair{"Roboto.ttf", gui::FontFace::Bold},
				std::pair{"NotoSans.ttf", gui::FontFace::Italic},
				std::pair{"JetBrainsMono.ttf", gui::FontFace::Code},
			};
			size_t total = 0;
			for (const auto &[name, role] : packageFaces) {
				std::ifstream file(root / name, std::ios::binary | std::ios::ate);
				if (!file) continue;
				const std::streamsize size = file.tellg();
				if (size <= 0 || static_cast<size_t>(size) > gui::MAXIMUM_FONT_PACKAGE_BYTES ||
					total + static_cast<size_t>(size) > gui::MAXIMUM_FONT_PACKAGE_TOTAL_BYTES)
					continue;
				std::vector<std::byte> bytes(static_cast<size_t>(size));
				file.seekg(0);
				if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) continue;
				if (loaded.Add(core::Name(std::string("fonts/") + name), role, bytes)) total += bytes.size();
			}
			return loaded;
		}();
		return package;
	}

	ScopedFont::ScopedFont(Typeface face, TextSize size, float scale) {
		const float factor = scale > 0.0f ? scale : 1.0f;

		ImFont *font = Font(face, size);
		if (font == nullptr && factor == 1.0f) {
			// Nothing loaded and nothing to scale. Leaving imgui's default
			// alone is cheaper than pushing it back over itself.
			return;
		}

		// `LegacySize` is what the face was rasterised at, which already
		// carries `InterfaceSettings::Scale` - so a zoom multiplies the size
		// somebody chose rather than replacing it.
		//
		// A null face means the family would not load; `PushFont` reads that as
		// "keep the current one", which is what a zoom wants when the shapes it
		// asked for are missing.
		const float base = font != nullptr ? font->LegacySize : ImGui::GetStyle().FontSizeBase;

		ImGui::PushFont(font, base * factor);
		Pushed = true;
	}

	ScopedFont::~ScopedFont() {
		if (Pushed) {
			ImGui::PopFont();
		}
	}
}
