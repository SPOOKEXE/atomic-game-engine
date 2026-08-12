#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ui/Fonts.hpp>

#include <array>
#include <filesystem>

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

		size_t IndexOf(Typeface face, TextSize size) {
			return static_cast<size_t>(face) * SizeCount + static_cast<size_t>(size);
		}
	}

	bool LoadFonts(float scale) {
		Loaded.fill(nullptr);

		ImGuiIO &io = ImGui::GetIO();
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
			// than one that opens in imgui's default — but it is worth saying
			// once, because "the text looks wrong" otherwise has no explanation
			// anywhere.
			ENGINE_WARN("no fonts found under {} — falling back to imgui's built-in face", root.string());
			return false;
		}

		// The interface face at body size is what everything gets unless it
		// asks otherwise.
		if (ImFont *body = Font(Typeface::Interface, TextSize::Body); body != nullptr) {
			io.FontDefault = body;
		}

		return true;
	}

	ImFont *Font(Typeface face, TextSize size) {
		if (face >= Typeface::Count || size >= TextSize::Count) {
			return nullptr;
		}

		ImFont *found = Loaded[IndexOf(face, size)];
		if (found != nullptr) {
			return found;
		}

		// A family that would not load falls back to the interface one at the
		// same size rather than to nothing, so a missing file costs the shapes
		// and not the layout.
		return Loaded[IndexOf(Typeface::Interface, size)];
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
		// carries `InterfaceSettings::Scale` — so a zoom multiplies the size
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
