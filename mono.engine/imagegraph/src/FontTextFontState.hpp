#pragma once

#include "SourceFontObservationContext.hpp"

#include <array>

namespace engine::imagegraph::detail {
	struct FontTextFontState {
		AllocationReservation PrimaryCharge, FallbackCharge;
		std::optional<FontValue> Primary, Fallback;
	};
	struct FontTextFontSelection {
		AllocationReservation FontCharge;
		std::array<SourceFontObservationLease, 3> Observations;
		FontValue Font;
	};
	struct FontTextLayout;
	bool ObserveBitmapTextTexture(
		NodeContext &, const FontTextLayout &, uint32_t size, bool antialias, FontTextFontSelection &
	);
	bool CloneTextFont(
		NodeContext &,
		const FontValue &,
		std::optional<FontValue> &,
		AllocationReservation &,
		std::string_view port
	);
	bool SelectTextFont(
		NodeContext &,
		std::string_view text,
		uint8_t changeCase,
		std::optional<std::string_view> observedCase,
		bool monospaced,
		bool wordWrap,
		uint32_t size,
		bool antialias,
		bool sdf,
		std::span<const FontMeasurement> measurements,
		FontTextFontState &,
		FontTextFontSelection &
	);
}
