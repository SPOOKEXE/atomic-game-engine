#include <engine/render/ShapedGlyphAtlas.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>

namespace engine::render {

	namespace {
		struct Library {
			FT_Library Value = nullptr;
			Library() {
				FT_Init_FreeType(&Value);
			}
			~Library() {
				if (Value != nullptr) FT_Done_FreeType(Value);
			}
		};
		FT_Library FreeType() {
			static Library library;
			return library.Value;
		}
	}

	ShapedGlyphAtlas::ShapedGlyphAtlas(float pixelSize) : PixelSize(pixelSize) {}

	size_t ShapedGlyphAtlas::KeyHash::operator()(const Key &key) const {
		return (static_cast<size_t>(key.Face.Id()) * 1315423911u + key.Index) * 257u + key.PixelSize;
	}

	ShapedGlyphAtlas::Page *ShapedGlyphAtlas::AllocatePage(uint64_t use) {
		if (Pages.size() < MAXIMUM_PAGES) {
			Pages.push_back(
				Page{
					.Pixels = std::vector<uint8_t>(static_cast<size_t>(PAGE_EXTENT) * PAGE_EXTENT), .Keys = {}
				}
			);
			Pages.back().LastUse = use;
			PackingPage = static_cast<uint16_t>(Pages.size() - 1);
			return &Pages.back();
		}
		auto victim =
			std::min_element(Pages.begin(), Pages.end(), [use](const Page &left, const Page &right) {
				const uint64_t leftAge = left.LastUse == use ? UINT64_MAX : left.LastUse;
				const uint64_t rightAge = right.LastUse == use ? UINT64_MAX : right.LastUse;
				return leftAge < rightAge;
			});
		// A page referenced by this draw list cannot be replaced until its
		// vertices have been consumed. Drop excess glyphs locally at the cap.
		if (victim->LastUse == use) return nullptr;
		for (const Key &key : victim->Keys)
			Entries.erase(key);
		*victim = Page{
			.Pixels = std::vector<uint8_t>(static_cast<size_t>(PAGE_EXTENT) * PAGE_EXTENT),
			.LastUse = use,
			.Keys = {}
		};
		PackingPage = static_cast<uint16_t>(victim - Pages.begin());
		return &*victim;
	}

	std::vector<ShapedAtlasGlyph> ShapedGlyphAtlas::Resolve(
		const gui::FontPackage &package,
		std::span<const gui::ShapedGlyph> glyphs,
		uint64_t use,
		float pixelSize
	) {
		std::vector<ShapedAtlasGlyph> resolved;
		resolved.reserve(glyphs.size());
		const float requested = pixelSize > 0.0f ? pixelSize : PixelSize;
		if (!std::isfinite(requested) || requested <= 0.0f || requested > gui::MAXIMUM_SHAPED_PIXEL_SIZE) {
			return resolved;
		}
		for (const gui::ShapedGlyph &shaped : glyphs) {
			const float glyphSize = shaped.PixelSize > 0.0f ? shaped.PixelSize : requested;
			if (!std::isfinite(glyphSize) || glyphSize > gui::MAXIMUM_SHAPED_PIXEL_SIZE) {
				resolved.push_back({});
				continue;
			}
			const uint16_t rasterSize = static_cast<uint16_t>(std::lround(glyphSize));
			if (rasterSize == 0) {
				resolved.push_back({});
				continue;
			}
			const Key key{shaped.Face, shaped.Index, rasterSize};
			if (const auto found = Entries.find(key); found != Entries.end()) {
				Pages[found->second.Page].LastUse = use;
				resolved.push_back(found->second.Glyph);
				continue;
			}
			const auto face = std::find_if(
				package.Faces().begin(),
				package.Faces().end(),
				[&shaped](const gui::FontPackageFace &candidate) { return candidate.Name == shaped.Face; }
			);
			if (face == package.Faces().end() || FreeType() == nullptr) {
				resolved.push_back({});
				continue;
			}
			FT_Face opened = nullptr;
			if (FT_New_Memory_Face(
					FreeType(),
					reinterpret_cast<const FT_Byte *>(face->Bytes.data()),
					static_cast<FT_Long>(face->Bytes.size()),
					0,
					&opened
				) != 0 ||
				FT_Set_Pixel_Sizes(opened, 0, rasterSize) != 0 ||
				FT_Load_Glyph(opened, shaped.Index, FT_LOAD_RENDER) != 0) {
				if (opened != nullptr) FT_Done_Face(opened);
				resolved.push_back({});
				continue;
			}
			const FT_Bitmap &bitmap = opened->glyph->bitmap;
			if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.buffer == nullptr ||
				bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.width > PAGE_EXTENT - 2 ||
				bitmap.rows > PAGE_EXTENT - 2 || std::abs(bitmap.pitch) < static_cast<int>(bitmap.width)) {
				FT_Done_Face(opened);
				resolved.push_back({});
				continue;
			}
			const uint16_t width = static_cast<uint16_t>(bitmap.width);
			const uint16_t height = static_cast<uint16_t>(bitmap.rows);
			Page *page = PackingPage < Pages.size() ? &Pages[PackingPage] : AllocatePage(use);
			if (page == nullptr) {
				FT_Done_Face(opened);
				resolved.push_back({});
				continue;
			}
			if (page->NextX + width + 1 > PAGE_EXTENT) {
				page->NextX = 1;
				page->NextY = static_cast<uint16_t>(page->NextY + page->RowHeight + 1);
				page->RowHeight = 0;
			}
			if (page->NextY + height + 1 > PAGE_EXTENT) page = AllocatePage(use);
			if (page == nullptr) {
				FT_Done_Face(opened);
				resolved.push_back({});
				continue;
			}
			const uint16_t pageIndex = static_cast<uint16_t>(page - Pages.data());
			for (uint16_t row = 0; row < height; row++) {
				const size_t sourceRow = bitmap.pitch >= 0 ? row : height - 1 - row;
				std::copy_n(
					bitmap.buffer + sourceRow * static_cast<size_t>(std::abs(bitmap.pitch)),
					width,
					page->Pixels.data() + static_cast<size_t>(page->NextY + row) * PAGE_EXTENT + page->NextX
				);
			}
			ShapedAtlasGlyph glyph{
				pageIndex,
				page->NextX,
				page->NextY,
				width,
				height,
				static_cast<float>(opened->glyph->bitmap_left),
				static_cast<float>(-opened->glyph->bitmap_top),
				true
			};
			page->NextX = static_cast<uint16_t>(page->NextX + width + 1);
			page->RowHeight = std::max(page->RowHeight, height);
			page->LastUse = use;
			page->Keys.push_back(key);
			Entries.emplace(key, Entry{glyph, pageIndex});
			resolved.push_back(glyph);
			FT_Done_Face(opened);
		}
		return resolved;
	}

	uint16_t ShapedGlyphAtlas::PageCount() const {
		return static_cast<uint16_t>(Pages.size());
	}
	const std::vector<uint8_t> &ShapedGlyphAtlas::Coverage(uint16_t page) const {
		static const std::vector<uint8_t> empty;
		return page < Pages.size() ? Pages[page].Pixels : empty;
	}

	void ShapedGlyphAtlas::Clear() {
		Pages.clear();
		Entries.clear();
		PackingPage = UINT16_MAX;
	}
}
