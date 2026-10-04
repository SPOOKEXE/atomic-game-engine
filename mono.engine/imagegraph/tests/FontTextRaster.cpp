#include "FontTextRaster.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.font_text_raster")
using namespace engine::imagegraph;
namespace {
	FontData RasterFace() {
		FontData font;
		font.LineHeight = 2;
		font.Frames = {{1, 2, {255, 0, 0, 128, 0, 255, 0, 255}}};
		FontGlyph glyph;
		glyph.Character = 'A';
		glyph.Frame = 0;
		glyph.Advance = 1;
		glyph.Width = 1;
		glyph.Height = 2;
		font.Glyphs = {glyph};
		return font;
	}
}
TEST_CASE(
	"text glyph native pixel centers implement source alpha multiply and alpha add separately",
	"[source_font]"
) {
	const auto font = RasterFace();
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok);
	CHECK(footprint.Work == 6);
	Image output{1, 2, std::vector<uint8_t>(8)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 0, 0, 128, 0, 255, 0, 255});
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{192, 0, 0, 255, 0, 255, 0, 255});
	options.Blend = detail::FontTextBlend::AlphaAdd;
	output.Pixels.assign(8, 0);
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 128, 0, 255, 0, 255});
}
TEST_CASE(
	"text glyph clockwise coordinate convention rotates counterclockwise source degrees", "[source_font]"
) {
	const auto font = RasterFace();
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	placement.Position = {2, 2};
	placement.Rotation = 90;
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 4, 3, footprint, failure) == Status::Ok);
	Image output{4, 3, std::vector<uint8_t>(48)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(detail::ReadPixel(output, 2, 1) == detail::Rgba{128 / 255.0, 0, 0, 128 / 255.0});
	CHECK(detail::ReadPixel(output, 3, 1) == detail::Rgba{0, 1, 0, 1});
	CHECK(detail::ReadPixel(output, 2, 2) == detail::Rgba{});
}
TEST_CASE(
	"textured glyphs require real atlas observations and use shader local normalized coordinates",
	"[source_font]"
) {
	auto font = RasterFace();
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	Image texture{1, 2, {0, 0, 255, 255, 255, 255, 255, 255}};
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	options.Texture = &texture;
	detail::FontGlyphRasterFootprint footprint;
	footprint.Work = 41;
	std::string failure;
	CHECK(
		detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) ==
		Status::UnsupportedExecution
	);
	CHECK(footprint.Work == 41);
	font.SourceTexture = font.Frames[0];
	font.Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 2};
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok);
	Image output{1, 2, std::vector<uint8_t>(8)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 128, 0, 255, 0, 255});
}
TEST_CASE("text glyph malformed admission cannot mutate the candidate image", "[source_font]") {
	const auto font = RasterFace();
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok);
	Image output{1, 2, std::vector<uint8_t>(8, 17)};
	const auto before = output;
	++footprint.Work;
	CHECK(
		detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) ==
		Status::InvalidValue
	);
	CHECK(output == before);
	options.Sampler.Interpolation = 6;
	CHECK(
		detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) ==
		Status::UnsupportedExecution
	);
}

TEST_CASE(
	"native file glyph texture modulation uses real local coverage without fake atlas coordinates",
	"[source_font]"
) {
	auto font = RasterFace();
	font.Raster = FontRasterProfile::NativeGlyphCoverage;
	font.Characters = FontCharacterProfile::UnicodeScalar;
	font.GlyphMapComplete = false;
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	Image texture{1, 2, {255, 255, 255, 128, 255, 255, 255, 255}};
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	options.Texture = &texture;
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok);
	Image output{1, 2, std::vector<uint8_t>(8)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 0, 0, 64, 0, 255, 0, 255});
	CHECK_FALSE(font.SourceTexture);
	CHECK_FALSE(font.Glyphs[0].TextureRectangle);
}
TEST_CASE(
	"text debug texture output requires owned atlas and rectangles before pixel publication", "[source_font]"
) {
	auto font = RasterFace();
	font.Raster = FontRasterProfile::NativeGlyphCoverage;
	font.Characters = FontCharacterProfile::UnicodeScalar;
	font.GlyphMapComplete = false;
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	options.DebugTexture = true;
	Image texture{1, 1, {255, 255, 255, 255}};
	options.Texture = &texture;
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	CHECK(
		detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) ==
		Status::UnsupportedExecution
	);
	font.SourceTexture = font.Frames[0];
	font.Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 2};
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok);
	Image output{1, 2, std::vector<uint8_t>(8)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 0, 255, 128, 255, 0, 255});
}
TEST_CASE(
	"bitmap UTF16 astral characters render both owned surrogate frames at their advances", "[source_font]"
) {
	FontData font;
	font.LineHeight = 1;
	font.Frames = {{1, 1, {255, 0, 0, 255}}, {1, 1, {0, 255, 0, 255}}};
	FontGlyph first;
	first.Character = 0xD83D;
	first.Frame = 0;
	first.Advance = 1;
	first.Width = 1;
	first.Height = 1;
	FontGlyph second = first;
	second.Character = 0xDE00;
	second.Frame = 1;
	font.Glyphs = {first, second};
	detail::FontGlyphPlacement placement;
	placement.Character = 0x1F600;
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 2, 1, footprint, failure) == Status::Ok);
	Image output{2, 1, std::vector<uint8_t>(8)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255});
}

TEST_CASE(
	"native distance raster uses real contour bytes and a one-pixel scaled coverage profile",
	"[source_font][font_native_sdf]"
) {
	FontData font;
	font.Raster = FontRasterProfile::NativeSignedDistance;
	font.Characters = FontCharacterProfile::UnicodeScalar;
	font.GlyphMapComplete = false;
	font.DistanceSpread = 8;
	font.LineHeight = 1;
	font.Frames = {{5, 1, {255, 255, 255, 112, 255, 255, 255, 124, 255, 255,
						   255, 128, 255, 255, 255, 132, 255, 255, 255, 144}}};
	FontGlyph glyph;
	glyph.Character = 'A';
	glyph.Frame = 0;
	glyph.Advance = 5;
	glyph.Width = 5;
	glyph.Height = 1;
	glyph.DistancePaddingPixels = 8;
	font.Glyphs = {glyph};
	detail::FontGlyphPlacement placement;
	placement.Character = 'A';
	detail::FontTextRasterOptions options;
	options.Sampler = {1, 3};
	options.DistanceAntialias = true;
	detail::FontGlyphRasterFootprint footprint;
	std::string failure;
	REQUIRE(detail::MeasureFontGlyphRaster(font, placement, options, 5, 1, footprint, failure) == Status::Ok);
	Image output{5, 1, std::vector<uint8_t>(20)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	// Distances -1,-.25,0,.25,1 intersect a unit-width pixel at coverage0,.25,.5,.75,1.
	CHECK(output.Pixels == std::vector<uint8_t>{0,	 0,	  0,   0,	64,	 64,  64,  64,	128, 128,
												128, 128, 191, 191, 191, 191, 255, 255, 255, 255});
	options.DistanceAntialias = false;
	output.Pixels.assign(20, 0);
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0,	 0,	  0,   0,	0,	 0,	  0,   0,	255, 255,
												255, 255, 255, 255, 255, 255, 255, 255, 255, 255});
	placement.Scale = {2, 2};
	options.DistanceAntialias = true;
	REQUIRE(
		detail::MeasureFontGlyphRaster(font, placement, options, 10, 2, footprint, failure) == Status::Ok
	);
	output = {10, 2, std::vector<uint8_t>(80)};
	REQUIRE(detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) == Status::Ok);
	for (uint32_t y = 0; y < 2; ++y) {
		CHECK(detail::ReadPixel(output, 0, y) == detail::Rgba{});
		CHECK(detail::ReadPixel(output, 2, y) == detail::Rgba{});
		CHECK(
			detail::ReadPixel(output, 4, y) == detail::Rgba{128 / 255., 128 / 255., 128 / 255., 128 / 255.}
		);
		CHECK(detail::ReadPixel(output, 6, y) == detail::Rgba{1, 1, 1, 1});
		CHECK(detail::ReadPixel(output, 8, y) == detail::Rgba{1, 1, 1, 1});
	}
}

TEST_CASE(
	"glyph raster footprint quotes actual sampler texel reads for local and observed atlas streams",
	"[source_font][font_sampling_work]"
) {
	struct SamplingCase {
		int64_t Interpolation;
		uint64_t GlyphWork, TexturedWork;
	};
	// Two pixels: one/ four/ four/ thirty-six source texel reads, plus target read and write.
	for (const auto sampling :
		 std::array<SamplingCase, 4>{{{1, 6, 8}, {2, 12, 20}, {3, 12, 20}, {4, 76, 148}}}) {
		auto font = RasterFace();
		font.Raster = FontRasterProfile::NativeGlyphCoverage;
		detail::FontGlyphPlacement placement;
		placement.Character = 'A';
		detail::FontTextRasterOptions options;
		options.Sampler = {sampling.Interpolation, 3};
		detail::FontGlyphRasterFootprint footprint;
		std::string failure;
		REQUIRE(
			detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok
		);
		CHECK(footprint.Work == sampling.GlyphWork);
		Image texture{1, 2, {255, 255, 255, 255, 255, 255, 255, 255}};
		options.Texture = &texture;
		REQUIRE(
			detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok
		);
		CHECK(footprint.Work == sampling.TexturedWork);
		font.SourceTexture = font.Frames[0];
		font.Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 2};
		REQUIRE(
			detail::MeasureFontGlyphRaster(font, placement, options, 1, 2, footprint, failure) == Status::Ok
		);
		CHECK(footprint.Work == sampling.TexturedWork);
		if (sampling.Interpolation == 3) {
			Image output{1, 2, std::vector<uint8_t>(8)};
			REQUIRE(
				detail::DrawFontGlyphRaster(font, placement, options, footprint, output, failure) ==
				Status::Ok
			);
			CHECK(output.Pixels == std::vector<uint8_t>{128, 0, 0, 128, 0, 255, 0, 255});
		}
	}
}
