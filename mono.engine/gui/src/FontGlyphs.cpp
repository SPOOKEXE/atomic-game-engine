#include "FontGlyphBitmap.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/gui/FontGlyphs.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H
#include FT_MODULE_H

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace engine::gui {
	namespace {
		struct GlyphOperationBudget {
			size_t Limit = 0;
			size_t Live = 0;
			size_t Peak = 0;
			bool LimitHit = false;
			bool AllocationFailed = false;

			bool Charge(size_t bytes) {
				if (bytes > Limit - Live) {
					LimitHit = true;
					return false;
				}
				Live += bytes;
				Peak = std::max(Peak, Live);
				return true;
			}
		};
		struct alignas(std::max_align_t) GlyphAllocationHeader {
			size_t Bytes = 0;
		};
		void *AllocateGlyphMemory(FT_Memory memory, long bytes) {
			if (bytes <= 0) return nullptr;
			auto &budget = *static_cast<GlyphOperationBudget *>(memory->user);
			const size_t payload = static_cast<size_t>(bytes);
			if (payload > std::numeric_limits<size_t>::max() - sizeof(GlyphAllocationHeader) ||
				!budget.Charge(payload + sizeof(GlyphAllocationHeader)))
				return nullptr;
			// FreeType's C allocation hooks require malloc-compatible aligned blocks.
			auto *allocation =
				static_cast<GlyphAllocationHeader *>(std::malloc(payload + sizeof(GlyphAllocationHeader)));
			if (!allocation) {
				budget.Live -= payload + sizeof(GlyphAllocationHeader);
				budget.AllocationFailed = true;
				return nullptr;
			}
			allocation->Bytes = payload;
			return allocation + 1;
		}
		void FreeGlyphMemory(FT_Memory memory, void *block) {
			if (!block) return;
			auto *allocation = static_cast<GlyphAllocationHeader *>(block) - 1;
			static_cast<GlyphOperationBudget *>(memory->user)->Live -=
				allocation->Bytes + sizeof(GlyphAllocationHeader);
			std::free(allocation);
		}
		void *ReallocateGlyphMemory(FT_Memory memory, long, long bytes, void *block) {
			if (!block) return AllocateGlyphMemory(memory, bytes);
			if (bytes <= 0) {
				FreeGlyphMemory(memory, block);
				return nullptr;
			}
			const auto *allocation = static_cast<const GlyphAllocationHeader *>(block) - 1;
			if (static_cast<size_t>(bytes) <= allocation->Bytes) return block;
			// Charge both live blocks during growth, rather than assuming an in-place realloc.
			void *replacement = AllocateGlyphMemory(memory, bytes);
			if (!replacement) return nullptr;
			std::memcpy(replacement, block, allocation->Bytes);
			FreeGlyphMemory(memory, block);
			return replacement;
		}
		struct GlyphLibrary {
			FT_Library Handle = nullptr;
			~GlyphLibrary() {
				if (Handle) FT_Done_Library(Handle);
			}
		};
		struct GlyphFace {
			FT_Face Handle = nullptr;
			~GlyphFace() {
				if (Handle) FT_Done_Face(Handle);
			}
		};
		FontGlyphStatus GlyphFailure(const GlyphOperationBudget &budget, FontGlyphStatus ordinary) {
			if (budget.LimitHit) return FontGlyphStatus::LimitExceeded;
			if (budget.AllocationFailed) return FontGlyphStatus::AllocationFailed;
			return ordinary;
		}
		bool UnicodeScalar(uint32_t character) {
			return character <= 0x10ffff && (character < 0xd800 || character > 0xdfff);
		}
		FontGlyphStatus LoadGlyphCoverage(FT_Face face, uint32_t index, bool antialias) {
			FT_Int32 flags = FT_LOAD_NO_AUTOHINT | (antialias ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO);
			// PNG/color bitmap codecs can allocate outside FreeType's memory callbacks.
			if (FT_IS_SCALABLE(face)) flags |= FT_LOAD_NO_BITMAP;
			if (FT_Load_Glyph(face, index, flags) != 0) return FontGlyphStatus::DecodeFailed;
			if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
				FT_Render_Glyph(face->glyph, antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO) != 0)
				return FontGlyphStatus::DecodeFailed;
			const FT_Bitmap &bitmap = face->glyph->bitmap;
			if (face->glyph->bitmap_top == std::numeric_limits<int>::min())
				return FontGlyphStatus::UnsupportedCoverage;
			if (!bitmap.width || !bitmap.rows) return FontGlyphStatus::Ok;
			if (bitmap.pixel_mode != FT_PIXEL_MODE_MONO && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
				return FontGlyphStatus::UnsupportedCoverage;
			const uint64_t pitch =
				bitmap.pitch < 0 ? uint64_t(-int64_t(bitmap.pitch)) : uint64_t(bitmap.pitch);
			const uint64_t rowBytes =
				bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? (uint64_t(bitmap.width) + 7) / 8 : bitmap.width;
			if (!bitmap.buffer || pitch < rowBytes ||
				pitch * bitmap.rows > MAXIMUM_FONT_GLYPH_OPERATION_BYTES ||
				uint64_t(bitmap.width) * bitmap.rows > MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES ||
				(bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && (bitmap.num_grays < 2 || bitmap.num_grays > 256)))
				return FontGlyphStatus::UnsupportedCoverage;
			return FontGlyphStatus::Ok;
		}
		FontGlyphStatus DecodeGlyphBatch(const FontGlyphRequest &request, FontGlyphBatch &output) {
			if (request.FontBytes.empty() || request.PixelSize == 0 ||
				request.PixelSize > MAXIMUM_FONT_GLYPH_PIXEL_SIZE || request.MaximumOperationBytes == 0 ||
				request.MaximumOperationBytes > MAXIMUM_FONT_GLYPH_OPERATION_BYTES)
				return FontGlyphStatus::InvalidRequest;
			if (request.FontBytes.size() > MAXIMUM_FONT_GLYPH_FILE_BYTES ||
				request.Characters.size() > MAXIMUM_FONT_GLYPH_CHARACTERS)
				return FontGlyphStatus::LimitExceeded;
			// The pinned WOFF2 Brotli decoder bypasses FT_Memory; refuse before opening a face.
			constexpr std::array<std::byte, 4> woff2{
				std::byte{'w'}, std::byte{'O'}, std::byte{'F'}, std::byte{'2'}
			};
			if (request.FontBytes.size() >= woff2.size() &&
				std::equal(woff2.begin(), woff2.end(), request.FontBytes.begin()))
				return FontGlyphStatus::UnsupportedFont;
			std::array<uint32_t, MAXIMUM_FONT_GLYPH_CHARACTERS> characters{};
			for (size_t i = 0; i < request.Characters.size(); ++i) {
				if (!UnicodeScalar(request.Characters[i])) return FontGlyphStatus::InvalidRequest;
				characters[i] = request.Characters[i];
			}
			std::sort(characters.begin(), characters.begin() + request.Characters.size());
			if (std::adjacent_find(characters.begin(), characters.begin() + request.Characters.size()) !=
				characters.begin() + request.Characters.size())
				return FontGlyphStatus::InvalidRequest;
			GlyphOperationBudget budget{request.MaximumOperationBytes};
			if (!budget.Charge(request.FontBytes.size()) || !budget.Charge(request.Characters.size_bytes()) ||
				!budget.Charge(sizeof(characters)))
				return FontGlyphStatus::LimitExceeded;
			FT_MemoryRec_ memory{&budget, AllocateGlyphMemory, FreeGlyphMemory, ReallocateGlyphMemory};
			GlyphLibrary library;
			if (FT_New_Library(&memory, &library.Handle) != 0)
				return GlyphFailure(budget, FontGlyphStatus::DecodeFailed);
			FT_Add_Default_Modules(library.Handle);
			if (budget.LimitHit || budget.AllocationFailed)
				return GlyphFailure(budget, FontGlyphStatus::DecodeFailed);
			GlyphFace face;
			if (FT_New_Memory_Face(
					library.Handle,
					reinterpret_cast<const FT_Byte *>(request.FontBytes.data()),
					static_cast<FT_Long>(request.FontBytes.size()),
					0,
					&face.Handle
				) != 0)
				return GlyphFailure(budget, FontGlyphStatus::InvalidFont);
			if (!FT_IS_SCALABLE(face.Handle)) {
				const char *format = FT_Get_Font_Format(face.Handle);
				if (!format || (std::string_view(format) != "BDF" && std::string_view(format) != "PCF"))
					return FontGlyphStatus::UnsupportedFont;
			}
			if (FT_Select_Charmap(face.Handle, FT_ENCODING_UNICODE) != 0)
				return FontGlyphStatus::UnsupportedFont;
			if (FT_Set_Pixel_Sizes(face.Handle, 0, request.PixelSize) != 0)
				return GlyphFailure(budget, FontGlyphStatus::DecodeFailed);
			// Count every requested bitmap before allocating any copied glyph coverage.
			size_t retained = request.Characters.size() * sizeof(FontGlyphCoverage);
			if (retained > MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES) return FontGlyphStatus::LimitExceeded;
			for (uint32_t character : request.Characters) {
				const FT_UInt index = FT_Get_Char_Index(face.Handle, character);
				if (!index) continue;
				const auto status = LoadGlyphCoverage(face.Handle, index, request.Antialias);
				if (status != FontGlyphStatus::Ok || budget.LimitHit || budget.AllocationFailed)
					return GlyphFailure(budget, status);
				const size_t pixels =
					size_t(face.Handle->glyph->bitmap.width) * face.Handle->glyph->bitmap.rows;
				if (pixels > MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES - retained)
					return FontGlyphStatus::LimitExceeded;
				retained += pixels;
			}
			if (!budget.Charge(retained)) return FontGlyphStatus::LimitExceeded;
			size_t remainingPixels = retained - request.Characters.size() * sizeof(FontGlyphCoverage);
			FontGlyphBatch candidate;
			candidate.Glyphs.reserve(request.Characters.size());
			const size_t metadataExcess =
				(candidate.Glyphs.capacity() - request.Characters.size()) * sizeof(FontGlyphCoverage);
			if (metadataExcess > MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES - retained ||
				!budget.Charge(metadataExcess))
				return FontGlyphStatus::LimitExceeded;
			retained += metadataExcess;
			size_t actualRetained = candidate.Glyphs.capacity() * sizeof(FontGlyphCoverage);
			candidate.AscentPixels = double(face.Handle->size->metrics.ascender) / 64;
			candidate.DescentPixels = -double(face.Handle->size->metrics.descender) / 64;
			candidate.LineHeightPixels = double(face.Handle->size->metrics.height) / 64;
			for (uint32_t character : request.Characters) {
				FontGlyphCoverage glyph;
				glyph.Character = character;
				glyph.GlyphIndex = FT_Get_Char_Index(face.Handle, character);
				glyph.Present = glyph.GlyphIndex != 0;
				if (glyph.Present) {
					const auto status = LoadGlyphCoverage(face.Handle, glyph.GlyphIndex, request.Antialias);
					if (status != FontGlyphStatus::Ok || budget.LimitHit || budget.AllocationFailed)
						return GlyphFailure(budget, status);
					const auto &slot = *face.Handle->glyph;
					glyph.AdvanceXPixels = double(slot.advance.x) / 64;
					glyph.AdvanceYPixels = -double(slot.advance.y) / 64;
					glyph.OffsetXPixels = slot.bitmap_left;
					glyph.OffsetYPixels = -slot.bitmap_top;
					glyph.Width = slot.bitmap.width;
					glyph.Height = slot.bitmap.rows;
					const size_t pixels = size_t(glyph.Width) * glyph.Height;
					if (pixels > remainingPixels) return FontGlyphStatus::LimitExceeded;
					remainingPixels -= pixels;
					glyph.Coverage.reserve(pixels);
					const size_t excess = glyph.Coverage.capacity() - pixels;
					if (excess > MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES - retained || !budget.Charge(excess))
						return FontGlyphStatus::LimitExceeded;
					retained += excess;
					actualRetained += glyph.Coverage.capacity();
					glyph.Coverage.resize(pixels);
					const size_t pitch = slot.bitmap.pitch < 0 ? size_t(-int64_t(slot.bitmap.pitch))
															   : size_t(slot.bitmap.pitch);
					if (pixels && !detail::CopyFontGlyphBitmap(
									  {{slot.bitmap.buffer, pitch * glyph.Height},
									   glyph.Width,
									   glyph.Height,
									   slot.bitmap.pitch,
									   slot.bitmap.num_grays,
									   slot.bitmap.pixel_mode == FT_PIXEL_MODE_MONO},
									  glyph.Coverage
								  ))
						return FontGlyphStatus::UnsupportedCoverage;
				}
				candidate.Glyphs.push_back(std::move(glyph));
			}
			if (budget.LimitHit || budget.AllocationFailed)
				return GlyphFailure(budget, FontGlyphStatus::DecodeFailed);
			candidate.RetainedBytes = actualRetained;
			candidate.PeakOperationBytes = budget.Peak;
			output = std::move(candidate);
			return FontGlyphStatus::Ok;
		}
	}
	FontGlyphStatus DecodeFontGlyphs(const FontGlyphRequest &request, FontGlyphBatch &output) {
		ENGINE_PROFILE("gui.font_glyphs.decode");
		try {
			FontGlyphBatch candidate;
			const auto status = DecodeGlyphBatch(request, candidate);
			// Release FreeType's borrowed input before replacement, even when input aliases old output.
			if (status == FontGlyphStatus::Ok) output = std::move(candidate);
			return status;
		} catch (const std::bad_alloc &) {
			// Allocation failure must preserve the caller's last complete glyph batch.
			return FontGlyphStatus::AllocationFailed;
		}
	}
}
