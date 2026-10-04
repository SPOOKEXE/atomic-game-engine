#include "FontTextFontState.hpp"

#include "FontPath.hpp"
#include "FontPayload.hpp"
#include "FontTextLayout.hpp"
#include "FontUnicode.hpp"

#include <algorithm>

namespace engine::imagegraph::detail {
	bool CloneTextFont(
		NodeContext &c,
		const FontValue &font,
		std::optional<FontValue> &output,
		AllocationReservation &outputCharge,
		std::string_view port
	) {
		if (!ValidFontPayload(font))
			return c.Fail(Status::InvalidValue, "text font payload is malformed", port);
		auto charge = c.ReserveWorkspace(FontStorageBytes(font, true) + sizeof(FontValue), port);
		if (!charge) return false;
		std::optional<FontValue> candidate{font};
		output.swap(candidate);
		candidate.reset();
		outputCharge = std::move(*charge);
		return true;
	}
	namespace {
		bool KnownScalar(const FontData &font, uint32_t point) {
			if (font.Characters == FontCharacterProfile::UnicodeScalar || point <= 0xffff)
				return font.GlyphMapComplete || FontGlyphForUnit(font, point);
			point -= 0x10000;
			return font.GlyphMapComplete || (FontGlyphForUnit(font, 0xd800 + (point >> 10)) &&
											 FontGlyphForUnit(font, 0xdc00 + (point & 1023)));
		}
		bool PresentScalar(const FontData &font, uint32_t point) {
			const auto present = [&](uint32_t unit) {
				const auto *glyph = FontGlyphForUnit(font, unit);
				return glyph && glyph->Present;
			};
			if (font.Characters == FontCharacterProfile::UnicodeScalar || point <= 0xffff)
				return present(point);
			point -= 0x10000;
			return present(0xd800 + (point >> 10)) && present(0xdc00 + (point & 1023));
		}
		bool GenerateTextFont(
			NodeContext &c,
			std::string_view path,
			std::string_view role,
			uint32_t size,
			bool antialias,
			bool sdf,
			std::span<const uint32_t> characters,
			std::optional<FontValue> &output,
			AllocationReservation &outputCharge,
			FontTextFontSelection &selection,
			size_t slot
		) {
			const uint64_t workspace = 3 * (Limits::MaximumTextBytes + std::string{}.capacity());
			auto pathCharge = c.ReserveWorkspace(workspace, role);
			if (!pathCharge) return false;
			std::string resolved, failure;
			const auto status =
				ResolveSourceFontPath(path, c.Request.SourceFonts, workspace, resolved, failure);
			if (status != Status::Ok) return c.Fail(status, std::move(failure), role);
			if (resolved.empty()) return true;
			if (!c.Request.SourceFonts || !c.Request.SourceFonts->Playing)
				return c.Fail(
					Status::UnsupportedExecution, "text file font requires an observed playback state", role
				);
			if (*c.Request.SourceFonts->Playing) return true;
			const auto nodeBytes = SourceFontAuthoredRetainedBytes(c.Authored);
			const auto contextBytes = c.Request.SourceFonts
										  ? SourceFontContextRetainedBytes(*c.Request.SourceFonts)
										  : std::optional<uint64_t>{0};
			if (!contextBytes) return c.Fail(Status::InvalidValue, "font source context is malformed", role);
			const uint64_t requestBytes = *contextBytes + sizeof(SourceFontRequest) +
										  characters.size() * sizeof(uint32_t) +
										  std::max(resolved.size(), std::string{}.capacity()) +
										  std::max(role.size(), std::string{}.capacity());
			if (!nodeBytes || *nodeBytes > c.AvailableBytes() ||
				requestBytes > c.AvailableBytes() - *nodeBytes)
				return c.Fail(Status::LimitExceeded, "font request clone exceeds live byte budget", role);
			auto charge = c.ReserveWorkspace(*nodeBytes + requestBytes, role);
			if (!charge) return false;
			SourceFontRequest request;
			request.Authored = c.Authored;
			if (c.Request.SourceFonts) request.Context = *c.Request.SourceFonts;
			request.ProcessorRow = c.ProcessorRow;
			request.Tick = c.Request.Tick;
			request.Subframe = c.Request.Subframe;
			request.NegativeFrame = c.Request.NegativeFrame;
			request.Role = role;
			request.ResolvedPath = std::move(resolved);
			request.PixelSize = size;
			request.Antialias = antialias;
			request.SignedDistanceField = sdf;
			request.Characters.assign(characters.begin(), characters.end());
			if (!ObserveSourceFont(c, request, selection.Observations[slot])) return false;
			const auto &observed = *selection.Observations[slot].Record;
			if (observed.Presence == SourceFontPresence::AbsentFile) return true;
			if (!observed.Font)
				return c.Fail(Status::InvalidValue, "present font observation omits its font", role);
			return CloneTextFont(c, *observed.Font, output, outputCharge, role);
		}

	}
	bool ObserveBitmapTextTexture(
		NodeContext &c,
		const FontTextLayout &layout,
		uint32_t size,
		bool antialias,
		FontTextFontSelection &selection
	) {
		if (!c.Input("texture") || selection.Font.Data->Raster != FontRasterProfile::BitmapSurface)
			return true;

		if (c.Request.SourceFonts &&
			c.Request.SourceFonts->BitmapTextureProfile == FontBitmapTextureProfile::NativeFrameUv &&
			!c.Boolean("attribute_debug_texture")) {
			const bool recorded = std::any_of(
				c.Request.FontObservations.begin(),
				c.Request.FontObservations.end(),
				[&](const SourceFontObservation &value) {
					const auto &key = value.Request;
					return key.Authored.Id == c.Authored.Id && key.ProcessorRow == c.ProcessorRow &&
						   key.Tick == c.Request.Tick && key.Subframe == c.Request.Subframe &&
						   key.NegativeFrame == c.Request.NegativeFrame && key.Role == "bitmap_texture";
				}
			);
			if (!recorded) return true;
		}
		const auto &font = *selection.Font.Data;
		auto charactersCharge = c.ReserveWorkspace(MaximumFontGlyphs * sizeof(uint32_t), "bitmap_texture");
		if (!charactersCharge) return false;
		std::vector<uint32_t> characters;
		characters.reserve(MaximumFontGlyphs);
		for (const auto &line : layout.Lines) {
			FontScalarCursor cursor{line.Text};
			uint32_t character = 0;
			while (cursor.Next(character)) {
				if (characters.size() == MaximumFontGlyphs)
					return c.Fail(
						Status::LimitExceeded,
						"bitmap texture characters exceed staging bound",
						"bitmap_texture"
					);
				characters.push_back(character);
			}
			if (cursor.Invalid)
				return c.Fail(Status::InvalidValue, "bitmap texture text is malformed", "bitmap_texture");
		}
		std::sort(characters.begin(), characters.end());
		characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
		bool renderable = false;

		const auto hasRectangle = [&](uint32_t unit) {
			const auto *glyph = FontGlyphForUnit(font, unit);
			if (!glyph || !glyph->Present || !glyph->Frame) return true;
			renderable = true;
			return glyph->TextureRectangle.has_value();
		};
		bool complete = true;
		for (uint32_t point : characters) {
			if (font.Characters == FontCharacterProfile::Utf16 && point > 0xffff) {
				point -= 0x10000;
				const bool first = hasRectangle(0xd800 + (point >> 10));
				const bool second = hasRectangle(0xdc00 + (point & 1023));
				complete = complete && first && second;
			} else {
				const bool present = hasRectangle(point);
				complete = complete && present;
			}
		}
		if (!renderable || (complete && font.SourceTexture)) return true;
		const auto nodeBytes = SourceFontAuthoredRetainedBytes(c.Authored);
		const auto contextBytes = c.Request.SourceFonts
									  ? SourceFontContextRetainedBytes(*c.Request.SourceFonts)
									  : std::optional<uint64_t>{sizeof(SourceFontContext)};
		if (!nodeBytes || !contextBytes)
			return c.Fail(
				Status::InvalidValue, "bitmap texture request context is malformed", "bitmap_texture"
			);
		const uint64_t bytes = *nodeBytes + *contextBytes + sizeof(SourceFontRequest) +
							   FontStorageBytes(selection.Font, true) + characters.size() * sizeof(uint32_t) +
							   64;
		auto charge = c.ReserveWorkspace(bytes, "bitmap_texture");
		if (!charge) return false;
		SourceFontRequest request;
		request.Authored = c.Authored;
		if (c.Request.SourceFonts) request.Context = *c.Request.SourceFonts;
		request.ProcessorRow = c.ProcessorRow;
		request.Tick = c.Request.Tick;
		request.Subframe = c.Request.Subframe;
		request.NegativeFrame = c.Request.NegativeFrame;
		request.Role = "bitmap_texture";
		request.PixelSize = size;
		request.Antialias = antialias;
		request.FontInput = selection.Font;
		request.Characters.assign(characters.begin(), characters.end());
		if (!ObserveSourceFont(c, request, selection.Observations[2])) return false;
		const auto &observed = *selection.Observations[2].Record;
		if (!observed.Font)
			return c.Fail(
				Status::InvalidValue, "bitmap texture observation omitted its font", "bitmap_texture"
			);
		std::optional<FontValue> candidate;
		if (!CloneTextFont(c, *observed.Font, candidate, selection.FontCharge, "bitmap_texture"))
			return false;
		selection.Font = std::move(*candidate);
		return true;
	}
	bool SelectTextFont(
		NodeContext &c,
		std::string_view text,
		uint8_t changeCase,
		std::optional<std::string_view> observedCase,
		bool monospaced,
		bool wordWrap,
		uint32_t size,
		bool antialias,
		bool sdf,
		FontTextFontState &state,
		FontTextFontSelection &selection
	) {
		if (size == 0 || size > 512 || changeCase > 3 || text.size() > Limits::MaximumTextBytes)
			return c.Fail(Status::InvalidValue, "text font controls exceed represented bounds", "size");
		auto charactersCharge = c.ReserveWorkspace((MaximumFontGlyphs * 2 + 2) * sizeof(uint32_t), "text");
		if (!charactersCharge) return false;
		std::vector<uint32_t> characters;
		characters.reserve(MaximumFontGlyphs * 2 + 2);
		FontScalarCursor cursor{text};
		uint32_t point = 0;
		bool titleStart = true;
		size_t count = 0;
		while (cursor.Next(point)) {
			if (++count > MaximumFontGlyphs)
				return c.Fail(Status::LimitExceeded, "text glyph requests exceed character limit", "text");
			if (changeCase && point > 127 && !observedCase)
				return c.Fail(
					Status::UnsupportedExecution,
					"non-ASCII text casing needs recorded Unicode transformation",
					"change_case"
				);
			characters.push_back(point);
			const uint32_t original = point;
			if (!observedCase && changeCase == 1 && point >= 'A' && point <= 'Z') point += 'a' - 'A';
			if (!observedCase && (changeCase == 2 || (changeCase == 3 && titleStart)) && point >= 'a' &&
				point <= 'z')
				point -= 'a' - 'A';
			titleStart = original == 32;
			if (point != original) characters.push_back(point);
		}
		if (cursor.Invalid)
			return c.Fail(Status::UnsupportedExecution, "text glyph request is not valid Unicode", "text");
		if (observedCase) {
			cursor = FontScalarCursor{*observedCase};
			while (cursor.Next(point)) {
				if (characters.size() == MaximumFontGlyphs * 2)
					return c.Fail(
						Status::LimitExceeded, "cased font glyph requests exceed staging capacity", "text"
					);
				characters.push_back(point);
			}
			if (cursor.Invalid)
				return c.Fail(Status::InvalidValue, "observed font casing is not valid Unicode", "text");
		}

		if (monospaced) characters.push_back('W');
		if (wordWrap) characters.push_back(' ');
		std::sort(characters.begin(), characters.end());
		characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
		if (characters.size() > MaximumFontGlyphs)
			return c.Fail(
				Status::LimitExceeded, "distinct text glyph requests exceed observed font limit", "text"
			);
		const Value *primary = c.Find("font");
		const FontValue *selected = primary ? std::get_if<FontValue>(primary) : nullptr;
		std::optional<std::string_view> path;
		if (primary) {
			if (const auto *textPath = std::get_if<std::string>(primary)) path = *textPath;
		} else if (c.Request.SourceFonts && c.Request.SourceFonts->DefaultFontPath)
			path = *c.Request.SourceFonts->DefaultFontPath;
		else if (state.Primary && c.Request.SourceFonts && c.Request.SourceFonts->Playing &&
				 *c.Request.SourceFonts->Playing)
			selected = &*state.Primary;
		else
			return c.Fail(Status::UnsupportedExecution, "source default font path is unobserved", "font");
		if (!selected && !path)
			return c.Fail(Status::UnsupportedExecution, "text font requires a path or owned font", "font");
		if (path) {
			if (!GenerateTextFont(
					c,
					*path,
					"font",
					size,
					antialias,
					sdf,
					characters,
					state.Primary,
					state.PrimaryCharge,
					selection,
					0
				))
				return false;
			selected = state.Primary ? &*state.Primary : nullptr;
		}
		const auto *fallback = c.Find("fallback_font");
		if (const auto *fallbackPath = fallback ? std::get_if<std::string>(fallback) : nullptr)
			if (!GenerateTextFont(
					c,
					*fallbackPath,
					"fallback_font",
					size,
					antialias,
					sdf,
					characters,
					state.Fallback,
					state.FallbackCharge,
					selection,
					1
				))
				return false;
		// Source generateFont ignores raw non-string fallback handles and preserves its old fallback.
		if (!text.empty() && state.Fallback) {
			if (!state.Primary)
				return c.Fail(
					Status::UnsupportedExecution,
					"source fallback check needs its retained primary font",
					"fallback_font"
				);
			cursor = FontScalarCursor{text};
			bool allPresent = true;
			while (cursor.Next(point)) {
				if (!KnownScalar(*state.Primary->Data, point))
					return c.Fail(
						Status::UnsupportedExecution,
						"source fallback membership is not observed for a glyph",
						"font"
					);
				allPresent = allPresent && PresentScalar(*state.Primary->Data, point);
			}
			if (!allPresent) selected = &*state.Fallback;
		}
		if (!selected) {
			if (text.empty()) return true;
			return c.Fail(
				Status::UnsupportedExecution, "text has no observed initial or retained font", "font"
			);
		}
		std::optional<FontValue> owned;
		if (!CloneTextFont(c, *selected, owned, selection.FontCharge, "font")) return false;
		selection.Font = std::move(*owned);
		return true;
	}
}
