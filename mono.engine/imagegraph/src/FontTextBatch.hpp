#pragma once

#include "FontTextFontState.hpp"
#include "FontTextLayout.hpp"
#include "FontTextRaster.hpp"

namespace engine::imagegraph::detail {
	struct FontTextAtlasDraw {
		uint32_t Width = 1, Height = 1;
		SurfaceLayout Surface{};
		FontGlyphPlacement Placement;
		FontGlyphRasterFootprint Footprint;
	};
	struct FontTextRow {
		AllocationReservation LayoutCharge, DrawCharge, InputsCharge, RetainedAtlasCharge;
		FontTextFontSelection SelectedFont;
		FontTextLayout Layout;
		std::vector<FontGlyphPlacement> Draws;
		std::vector<FontGlyphRasterFootprint> Footprints;
		std::vector<FontTextAtlasDraw> AtlasDraws;
		std::optional<Image> Texture, Background;
		ArrayValue Atlas;
		SurfaceLayout Surface{};
		uint32_t Width = 1, Height = 1;
		SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
		FontTextRasterOptions Raster;
		Colour BackgroundColour{0, 0, 0, 255};
		bool Empty = false, GenerateAtlas = false, ClearBackground = false;
		uint64_t OutputBytes = 0, TemporaryBytes = 0, Work = 0;
	};
	struct FontTextBatch {
		// Reservations outlive the storage they admit.
		AllocationReservation TableCharge, OutputCharge, StateCharge, TemporaryCharge;
		FontTextFontState Fonts;
		std::vector<FontTextRow> Rows;
		DataReplayEntry State;
		const ArrayValue *PreviousRows = nullptr;
		size_t ExpectedRows = 0, PreparedRows = 0;
		uint64_t Work = 0, CaseWork = 0;
		bool Admitted = false;
	};
	bool BeginFontTextBatch(NodeContext &, size_t rows, FontTextBatch &);
	bool PrepareFontTextRow(NodeContext &, FontTextBatch &);
	bool AdmitFontTextBatch(NodeContext &, FontTextBatch &);
	bool RenderFontTextRow(NodeContext &, FontTextBatch &);
}
