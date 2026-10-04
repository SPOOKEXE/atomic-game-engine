#include "FontPayload.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		bool AddFontBytes(uint64_t &bytes, uint64_t count, uint64_t unit = 1) {
			if (count > (std::numeric_limits<uint64_t>::max() - bytes) / unit) return false;
			bytes += count * unit;
			return true;
		}
		bool FontImageValid(const Image &image) {
			return ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) &&
				   FiniteSurfaceSamples(image);
		}
	}
	uint64_t FontStorageBytes(const FontValue &value, bool retained) {
		if (!value.Data) return 0;
		const auto &data = *value.Data;
		uint64_t bytes = sizeof(FontData);
		if (!AddFontBytes(bytes, retained ? data.Frames.capacity() : data.Frames.size(), sizeof(Image)) ||
			!AddFontBytes(bytes, retained ? data.Glyphs.capacity() : data.Glyphs.size(), sizeof(FontGlyph)) ||
			!AddFontBytes(
				bytes,
				retained ? data.Measurements.capacity() : data.Measurements.size(),
				sizeof(FontMeasurement)
			) ||
			!AddFontBytes(bytes, retained ? data.Identity.capacity() : data.Identity.size()))
			return std::numeric_limits<uint64_t>::max();
		for (const auto &frame : data.Frames)
			if (!AddFontBytes(bytes, retained ? frame.Pixels.capacity() : frame.Pixels.size()))
				return std::numeric_limits<uint64_t>::max();
		for (const auto &measurement : data.Measurements)
			if (!AddFontBytes(bytes, retained ? measurement.Text.capacity() : measurement.Text.size()))
				return std::numeric_limits<uint64_t>::max();
		if (data.SourceTexture &&
			!AddFontBytes(
				bytes, retained ? data.SourceTexture->Pixels.capacity() : data.SourceTexture->Pixels.size()
			))
			return std::numeric_limits<uint64_t>::max();
		return bytes;
	}
	bool ValidFontPayload(const FontValue &value) {
		if (!value.Data) return false;
		const auto &data = *value.Data;
		if (data.Raster > FontRasterProfile::NativeSignedDistance ||
			data.Characters > FontCharacterProfile::UnicodeScalar || data.Frames.size() > MaximumFontGlyphs ||
			data.Glyphs.size() > MaximumFontGlyphs || data.Measurements.size() > MaximumFontGlyphs ||
			data.Identity.size() > Limits::MaximumTextBytes || !std::isfinite(data.LineHeight) ||
			data.LineHeight < 0 || !std::isfinite(data.MissingAdvance) || !std::isfinite(data.SpaceAdvance) ||
			FontStorageBytes(value, true) > Limits::MaximumArrayBytes)
			return false;
		if (data.Raster == FontRasterProfile::NativeSignedDistance) {
			if (data.DistanceSpread < 2 || data.DistanceSpread > 32 || data.GlyphMapComplete ||
				data.Characters != FontCharacterProfile::UnicodeScalar)
				return false;
		} else if (data.DistanceSpread)
			return false;
		const uint32_t maximum = data.Characters == FontCharacterProfile::Utf16 ? 0xffff : 0x10ffff;
		if (data.HasCharacterRange &&
			(data.FirstCharacter > data.LastCharacter || data.LastCharacter > maximum))
			return false;
		if (data.Raster == FontRasterProfile::BitmapSurface &&
			(data.Frames.empty() || !data.GlyphMapComplete))
			return false;
		if (data.Raster == FontRasterProfile::NativeGlyphCoverage && data.GlyphMapComplete) return false;
		for (const auto &frame : data.Frames) {
			if (!FontImageValid(frame)) return false;
			if (data.Raster == FontRasterProfile::NativeSignedDistance) {
				if (frame.Format != SurfaceFormat::RGBA8Unorm) return false;
				for (size_t pixel = 0; pixel < frame.Pixels.size(); pixel += 4)
					if (frame.Pixels[pixel] != 255 || frame.Pixels[pixel + 1] != 255 ||
						frame.Pixels[pixel + 2] != 255)
						return false;
			}
		}
		if (data.SourceTexture && !FontImageValid(*data.SourceTexture)) return false;
		for (size_t index = 0; index < data.Glyphs.size(); ++index) {
			const auto &glyph = data.Glyphs[index];
			if (glyph.Character > maximum ||
				(data.Characters == FontCharacterProfile::UnicodeScalar && glyph.Character >= 0xd800 &&
				 glyph.Character <= 0xdfff) ||
				(index && data.Glyphs[index - 1].Character >= glyph.Character) ||
				(glyph.Frame && (!glyph.Present || *glyph.Frame >= data.Frames.size())) ||
				!std::isfinite(glyph.Advance) || !std::isfinite(glyph.Width) || glyph.Width < 0 ||
				!std::isfinite(glyph.Height) || glyph.Height < 0 || !std::isfinite(glyph.Offset.X) ||
				!std::isfinite(glyph.Offset.Y))
				return false;
			if (data.Raster == FontRasterProfile::NativeSignedDistance) {
				if (glyph.DistancePaddingPixels != (glyph.Frame ? data.DistanceSpread : 0)) return false;
			} else if (glyph.DistancePaddingPixels)
				return false;
			if (glyph.TextureRectangle) {
				const auto &rectangle = *glyph.TextureRectangle;
				if (!std::isfinite(rectangle.X) || !std::isfinite(rectangle.Y) ||
					!std::isfinite(rectangle.Z) || rectangle.Z < 0 || !std::isfinite(rectangle.W) ||
					rectangle.W < 0)
					return false;
				if (data.SourceTexture &&
					(rectangle.X < 0 || rectangle.Y < 0 || rectangle.X > data.SourceTexture->Width ||
					 rectangle.Y > data.SourceTexture->Height ||
					 rectangle.Z > data.SourceTexture->Width - rectangle.X ||
					 rectangle.W > data.SourceTexture->Height - rectangle.Y))
					return false;
			}
		}
		for (const auto &measurement : data.Measurements)
			if (measurement.Text.size() > Limits::MaximumTextBytes ||
				!std::isfinite(measurement.MaximumLineWidth) || measurement.MaximumLineWidth < 0 ||
				!std::isfinite(measurement.LineGap) || !std::isfinite(measurement.Width) ||
				measurement.Width < 0 || !std::isfinite(measurement.Height) || measurement.Height < 0)
				return false;
		return true;
	}
	bool ContainsFontLiteral(const Value &value) {
		size_t visits = 0;
		const auto contains = [&](const auto &self, const auto &leaf, size_t depth) -> bool {
			using T = std::decay_t<decltype(leaf)>;
			if (++visits > Limits::MaximumArrayBytes || depth > Limits::MaximumArrayDepth) return true;
			if constexpr (std::is_same_v<T, FontValue>)
				return true;
			else if constexpr (std::is_same_v<T, ArrayValue>) {
				if (leaf.ElementType == ValueType::Font) return true;
				for (const auto &entry : leaf.Elements)
					if (std::visit([&](const auto &child) { return self(self, child, depth + 1); }, entry))
						return true;
				for (const auto &row : leaf.Nested)
					for (const auto &entry : row)
						if (std::visit(
								[&](const auto &child) { return self(self, child, depth + 2); }, entry
							))
							return true;
				for (const auto &entry : leaf.Items)
					if (self(self, entry, depth + 1)) return true;
			} else if constexpr (std::is_same_v<T, SourceArrayItem>) {
				return std::visit(
					[&](const auto &child) -> bool {
						using C = std::decay_t<decltype(child)>;
						if constexpr (std::is_same_v<C, ElementValue>)
							return std::visit(
								[&](const auto &entry) { return self(self, entry, depth); }, child
							);
						else if constexpr (std::is_same_v<C, std::vector<SourceArrayItem>>) {
							for (const auto &entry : child)
								if (self(self, entry, depth + 1)) return true;
						}
						return false;
					},
					leaf.Data
				);
			} else if constexpr (std::is_same_v<T, StructValue>) {
				if (leaf.Data)
					for (const auto &[key, child] : leaf.Data->Fields) {
						(void)key;
						if (std::visit(
								[&](const auto &entry) { return self(self, entry, depth + 1); }, child
							))
							return true;
					}
			} else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
				if (leaf.Data) {
					for (const auto &instruction : leaf.Data->Instructions)
						if (std::visit(
								[&](const auto &entry) { return self(self, entry, depth + 1); },
								instruction.Literal
							))
							return true;
					for (const auto &[key, child] : leaf.Data->Bindings) {
						(void)key;
						if (std::visit(
								[&](const auto &entry) { return self(self, entry, depth + 1); }, child
							))
							return true;
					}
				}
			} else if constexpr (std::is_same_v<T, ArraySelectorValue>) {
				return leaf.Data && self(self, leaf.Data->Values, depth + 1);
			}
			return false;
		};
		return std::visit([&](const auto &leaf) { return contains(contains, leaf, 0); }, value);
	}

}
