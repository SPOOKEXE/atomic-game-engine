#include "../FontPayload.hpp"
#include "../FontUnicode.hpp"
#include "Families.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t BitmapFontWork = 16 * 1024 * 1024;
		struct BitmapFontMap {
			uint32_t Character = 0, Frame = 0;
		};
		bool BitmapFont(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.bitmap_font");
			const auto *rawMap = c.Find("string_map");
			const auto *map = rawMap ? std::get_if<std::string>(rawMap) : nullptr;
			if (!map || map->size() > Limits::MaximumTextBytes)
				return c.Fail(
					Status::UnsupportedExecution,
					"bitmap font requires a bounded source string map",
					"string_map"
				);
			const auto *rawSeparation = c.Find("separation");
			const auto separation =
				rawSeparation ? SourceChoiceNumber(*rawSeparation) : std::optional<double>{2};
			if (!separation || *separation < std::numeric_limits<int32_t>::min() ||
				*separation > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution,
					"bitmap font separation exceeds represented source int32 profile",
					"separation"
				);
			// Source surface-created frames have full extents in both proportional branches.
			(void)c.Boolean("proportional", true);
			if (c.FailureCode != Status::Ok) return false;
			const int32_t sep = static_cast<int32_t>(*separation);
			size_t maps = 0;
			FontUtf16Cursor cursor{{*map}};
			uint32_t character = 0;
			while (cursor.Next(character))
				if (++maps > MaximumFontGlyphs)
					return c.Fail(
						Status::LimitExceeded, "bitmap string map exceeds glyph bound", "string_map"
					);
			if (cursor.Source.Invalid)
				return c.Fail(
					Status::UnsupportedExecution, "bitmap string map is not valid Unicode", "string_map"
				);
			const ImageArray *array = nullptr;
			for (const auto &[port, data] : c.ImageArrays)
				if (port == "font_surfaces") array = data;
			const Image *single = c.Input("font_surfaces");
			if (!single)
				if (const auto *raw = c.Find("font_surfaces"))
					if (const auto *surface = std::get_if<SurfaceValue>(raw)) single = &surface->Data;
			size_t frames = 0;
			const Image *first = nullptr;
			uint64_t scan = 0;
			const auto visit = [&](const auto &body) -> bool {
				if (array) {
					if (array->Images.size() > MaximumFontGlyphs || array->Items.size() > MaximumFontGlyphs)
						return c.Fail(
							Status::LimitExceeded, "bitmap source frame list exceeds bounds", "font_surfaces"
						);
					if (array->Items.empty()) {
						for (const auto &image : array->Images)
							if (!body(image)) return false;
					} else
						for (const auto &item : array->Items) {
							// Nested arrays are not source surface handles and are skipped by surface_exists.
							if (const auto *index = std::get_if<size_t>(&item.Data)) {
								if (*index >= array->Images.size())
									return c.Fail(
										Status::InvalidValue,
										"bitmap frame index is outside owned images",
										"font_surfaces"
									);
								if (!body(array->Images[*index])) return false;
							}
						}
				} else if (single && !body(*single))
					return false;
				return true;
			};
			if (!visit([&](const Image &image) {
					if (!ValidSurfaceLayout(
							image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes
						) ||
						!FiniteSurfaceSamples(image))
						return c.Fail(
							Status::InvalidValue, "bitmap source frame has invalid pixels", "font_surfaces"
						);
					if (image.Pixels.size() > BitmapFontWork - scan)
						return c.Fail(
							Status::LimitExceeded,
							"bitmap source frame scan exceeds work bound",
							"font_surfaces"
						);
					scan += image.Pixels.size();
					if (!first) first = &image;
					++frames;
					return true;
				}))
				return false;
			if (!first)
				return c.Fail(
					Status::UnsupportedExecution,
					"empty bitmap input leaves a freed source font handle without an owned observation",
					"font_surfaces"
				);
			const auto layout = CheckedSurfaceLayout(
				first->Width, first->Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumArrayBytes
			);
			if (!layout || frames > MaximumFontGlyphs ||
				frames > BitmapFontWork / std::max(uint64_t{1}, layout->Bytes))
				return c.Fail(
					Status::LimitExceeded, "bitmap frame normalization exceeds work bound", "font_surfaces"
				);
			const uint64_t pixels = frames * layout->Bytes;
			const uint64_t bytes = sizeof(FontData) + frames * sizeof(Image) + pixels +
								   maps * sizeof(FontGlyph) + std::string{}.capacity() + sizeof(Value);
			if (bytes > Limits::MaximumArrayBytes || !c.ReserveOutput(bytes, "font"))
				return c.FailureCode == Status::Ok
						   ? c.Fail(Status::LimitExceeded, "bitmap font exceeds owned byte bound", "font")
						   : false;
			auto mapCharge = c.ReserveWorkspace(maps * sizeof(BitmapFontMap), "string_map");
			if (!mapCharge) return false;
			std::vector<BitmapFontMap> mapping(maps);
			cursor = FontUtf16Cursor{{*map}};
			for (size_t index = 0; index < maps; ++index) {
				if (!cursor.Next(character))
					return c.Fail(
						Status::InvalidValue, "bitmap map changed during immutable evaluation", "string_map"
					);
				mapping[index] = {character, static_cast<uint32_t>(index)};
			}
			std::sort(mapping.begin(), mapping.end(), [](const auto &a, const auto &b) {
				return a.Character != b.Character ? a.Character < b.Character : a.Frame < b.Frame;
			});
			FontValue result;
			auto &font = result.Data.emplace();
			font.Frames.reserve(frames);
			font.Glyphs.reserve(maps);
			font.LineHeight = first->Height;
			font.MissingAdvance = sep;
			font.SpaceAdvance = double(first->Width) + sep;
			font.HasCharacterRange = !mapping.empty();
			if (!mapping.empty()) {
				font.FirstCharacter = mapping.front().Character;
				font.LastCharacter = mapping.back().Character;
			}
			if (!visit([&](const Image &image) {
					Image copied{first->Width, first->Height, std::vector<uint8_t>(layout->Bytes)};
					core::Metrics::Count(
						"imagegraph.font.bitmap_allocated_payload_bytes", copied.Pixels.size()
					);
					core::Metrics::Count("imagegraph.font.bitmap_allocations", 1);
					// Explicit CPU pixel-center nearest profile for heterogeneous runtime sprite frames.
					for (uint32_t y = 0; y < copied.Height; ++y)
						for (uint32_t x = 0; x < copied.Width; ++x) {
							SurfacePixel pixel{};
							const uint32_t sx = static_cast<uint32_t>(
								(uint64_t(x) * 2 + 1) * image.Width / (uint64_t(copied.Width) * 2)
							);
							const uint32_t sy = static_cast<uint32_t>(
								(uint64_t(y) * 2 + 1) * image.Height / (uint64_t(copied.Height) * 2)
							);
							if (!LoadSurfacePixel(image, sx, sy, pixel))
								return c.Fail(
									Status::InvalidValue, "bitmap frame cannot be sampled", "font_surfaces"
								);
							if (const auto info = DescribeSurfaceFormat(image.Format);
								info && info->Channels == 1)
								pixel = {pixel[0], pixel[0], pixel[0], 1};
							if (!StoreSurfacePixel(copied, x, y, pixel))
								return c.Fail(
									Status::UnsupportedExecution,
									"bitmap frame has no finite RGBA8 sprite profile",
									"font_surfaces"
								);
						}
					copied.Hash = SurfaceHash(copied);
					font.Frames.push_back(std::move(copied));
					return true;
				}))
				return false;
			for (size_t index = 0; index < mapping.size();) {
				size_t end = index + 1;
				while (end < mapping.size() && mapping[end].Character == mapping[index].Character)
					++end;
				const auto &selected = mapping[end - 1];
				FontGlyph glyph;
				glyph.Character = selected.Character;
				glyph.Present = selected.Frame < frames;
				glyph.Advance = sep;
				if (glyph.Present) {
					glyph.Frame = selected.Frame;
					glyph.Advance += first->Width;
					glyph.Width = first->Width;
					glyph.Height = first->Height;
				}
				font.Glyphs.push_back(glyph);
				index = end;
			}
			if (!ValidFontPayload(result))
				return c.Fail(Status::InvalidValue, "bitmap font payload failed native validation", "font");
			c.SetValue("font", std::move(result));
			if (!c.SetOutputDomain("font", {ValueType::Font, std::nullopt, SourceSocketKind::Font}))
				return false;
			return c.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceBitmapFontExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.font_bitmap", BitmapFont, true}};
		return entries;
	}
}
