#pragma once

// Bounded RGB palette mapping for image nodes.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace engine::imagegraph::detail {
	inline constexpr size_t MAXIMUM_PALETTE_ENTRIES = Limits::MaximumPaletteEntries;

	struct PaletteValue {
		std::array<Colour, MAXIMUM_PALETTE_ENTRIES> Colors{};
		size_t Count = 0;
	};

	enum class PaletteStatus : uint8_t { Ok, InvalidControl, InvalidImage };

	inline bool MakePalette(std::span<const Colour> colors, PaletteValue &out) {
		if (colors.empty() || colors.size() > MAXIMUM_PALETTE_ENTRIES) return false;
		PaletteValue parsed;
		std::copy(colors.begin(), colors.end(), parsed.Colors.begin());
		parsed.Count = colors.size();
		out = parsed;
		return true;
	}

	inline bool MakePalette(const ArrayValue &array, PaletteValue &out) {
		if (array.ElementType != ValueType::Colour || array.Elements.empty() ||
			array.Elements.size() > MAXIMUM_PALETTE_ENTRIES) {
			return false;
		}
		PaletteValue parsed;
		for (size_t index = 0; index < array.Elements.size(); index++) {
			const Colour *color = std::get_if<Colour>(&array.Elements[index]);
			if (color == nullptr) return false;
			parsed.Colors[index] = *color;
		}
		parsed.Count = array.Elements.size();
		out = parsed;
		return true;
	}

	inline const Colour &NearestPaletteColour(
		uint8_t red,
		uint8_t green,
		uint8_t blue,
		uint8_t alpha,
		bool premultiplied,
		const PaletteValue &palette
	) {
		uint64_t bestDistance = UINT64_MAX;
		const Colour *best = &palette.Colors[0];
		for (size_t index = 0; index < palette.Count; index++) {
			const Colour &candidate = palette.Colors[index];
			const int64_t redDelta = premultiplied ? int64_t(red) * alpha - int64_t(candidate.Red) * 255
												   : int64_t(red) - candidate.Red;
			const int64_t greenDelta = premultiplied ? int64_t(green) * alpha - int64_t(candidate.Green) * 255
													 : int64_t(green) - candidate.Green;
			const int64_t blueDelta = premultiplied ? int64_t(blue) * alpha - int64_t(candidate.Blue) * 255
													: int64_t(blue) - candidate.Blue;
			const uint64_t distance =
				static_cast<uint64_t>(redDelta * redDelta + greenDelta * greenDelta + blueDelta * blueDelta);
			if (distance < bestDistance) {
				bestDistance = distance;
				best = &candidate;
			}
		}
		return *best;
	}

	inline const Colour &
	NearestPaletteColour(uint8_t red, uint8_t green, uint8_t blue, const PaletteValue &palette) {
		return NearestPaletteColour(red, green, blue, 255, false, palette);
	}

	inline PaletteStatus PosterizeWithPalette(
		const Image &source, Image &output, const PaletteValue &palette, bool posterizeAlpha = false
	) {
		if (palette.Count == 0 || palette.Count > MAXIMUM_PALETTE_ENTRIES)
			return PaletteStatus::InvalidControl;
		if (source.Width == 0 || source.Height == 0 || source.Width > Limits::MaximumDimension ||
			source.Height > Limits::MaximumDimension || source.Width != output.Width ||
			source.Height != output.Height) {
			return PaletteStatus::InvalidImage;
		}
		const uint64_t bytes = uint64_t(source.Width) * source.Height * 4;
		if (bytes > Limits::MaximumOutputBytes || bytes != source.Pixels.size() ||
			bytes != output.Pixels.size())
			return PaletteStatus::InvalidImage;

		for (size_t index = 0; index < source.Pixels.size(); index += 4) {
			const Colour &color = NearestPaletteColour(
				source.Pixels[index],
				source.Pixels[index + 1],
				source.Pixels[index + 2],
				source.Pixels[index + 3],
				posterizeAlpha,
				palette
			);
			output.Pixels[index] = color.Red;
			output.Pixels[index + 1] = color.Green;
			output.Pixels[index + 2] = color.Blue;
			output.Pixels[index + 3] = posterizeAlpha ? color.Alpha : source.Pixels[index + 3];
		}
		return PaletteStatus::Ok;
	}
}
