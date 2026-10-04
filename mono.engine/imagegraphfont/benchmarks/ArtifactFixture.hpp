#pragma once

#include <engine/imagegraphfont/GraphFontInputs.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace font_boundary_fixture {
	using engine::imagegraphfont::GraphFontConfiguration;
	inline constexpr uint64_t ArtifactOperationBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
	inline constexpr uint64_t CoverageFrameHash = 0xdeadbeef01234567;
	inline constexpr uint64_t TextureFrameHash = 0x123456789abcdef0;
	inline constexpr uint64_t DistanceFrameHash = 0x8877665544332211;

	namespace detail {
		using namespace engine::imagegraph;
		inline FontValue CoverageFont() {
			FontValue result;
			auto &font = result.Data.emplace();
			font.Raster = FontRasterProfile::NativeGlyphCoverage;
			font.Characters = FontCharacterProfile::UnicodeScalar;
			font.GlyphMapComplete = false;
			font.LineHeight = 10;
			font.SpaceAdvance = 4;
			font.Identity = "fixture-coverage";
			font.HasCharacterRange = true;
			font.FirstCharacter = 32;
			font.LastCharacter = 66;
			Image frame{5, 7, std::vector<uint8_t>(5 * 7 * 4), CoverageFrameHash};
			constexpr std::array<uint8_t, 7> rows{0x70, 0x88, 0x88, 0xf8, 0x88, 0x88, 0x88};
			for (size_t row = 0; row < rows.size(); ++row)
				for (size_t column = 0; column < 5; ++column) {
					const auto pixel = (row * 5 + column) * 4;
					frame.Pixels[pixel] = frame.Pixels[pixel + 1] = frame.Pixels[pixel + 2] = 255;
					frame.Pixels[pixel + 3] = rows[row] & (0x80 >> column) ? 255 : 0;
				}
			font.Frames.push_back(frame);
			font.SourceTexture = std::move(frame);
			font.SourceTexture->Hash = TextureFrameHash;
			font.Glyphs = {
				{32, true, {}, 4, 4, 10, {0, 8}, {}, 0},
				{65, true, 0, 8, 8, 10, {-1, 3}, Vector4{0, 0, 5, 7}, 0},
				{66, false, {}, 0, 0, 10, {0, 8}, {}, 0}
			};
			font.Measurements = {{"A", 20, 1, 8, 10}};
			return result;
		}
		// Accepted owned SDF serialization sample from GraphFontConfiguration tests.
		// Its one-pixel carrier is a codec fixture, not a decoder output golden.
		inline FontValue DistanceFont() {
			FontValue result;
			auto &font = result.Data.emplace();
			font.Raster = FontRasterProfile::NativeSignedDistance;
			font.Characters = FontCharacterProfile::UnicodeScalar;
			font.GlyphMapComplete = false;
			font.DistanceSpread = 4;
			font.LineHeight = 3;
			font.MissingAdvance = 1;
			font.SpaceAdvance = 2;
			font.Identity = "fixture-distance";
			font.HasCharacterRange = true;
			font.FirstCharacter = font.LastCharacter = 65;
			font.Frames.push_back({1, 1, {255, 255, 255, 44}, DistanceFrameHash});
			font.Glyphs = {{65, true, 0, 2.25, 1, 1, {-0.5, 0.25}, {}, 4}};
			font.Measurements = {{"A", 20, 1, 2.25, 3}};
			return result;
		}
		inline bool Refuse(std::string &failure, const char *message) {
			failure = message;
			return false;
		}
		enum class FontKind { Coverage, Distance, BitmapInput, BitmapReceipt };
		inline bool ImageOracle(const Image &image, bool distance, bool texture, std::string &failure) {
			if (image.Format != SurfaceFormat::RGBA8Unorm || image.Width != (distance ? 1u : 5u) ||
				image.Height != (distance ? 1u : 7u) || image.Pixels.size() != (distance ? 4u : 140u) ||
				image.Hash != (texture	  ? TextureFrameHash
							   : distance ? DistanceFrameHash
										  : CoverageFrameHash))
				return Refuse(failure, "image layout or stored hash differs from the literal fixture");
			if (distance)
				return image.Pixels == std::vector<uint8_t>{255, 255, 255, 44} ||
					   Refuse(failure, "distance sample must preserve white RGB and alpha 44");
			// Expanded independently from the BDF bitmap rows used by fixture construction.
			constexpr std::array<uint8_t, 35> alpha{0,	 255, 255, 255, 0,	 255, 0,   0,	0,	 255, 255, 0,
													0,	 0,	  255, 255, 255, 255, 255, 255, 255, 0,	  0,   0,
													255, 255, 0,   0,	0,	 255, 255, 0,	0,	 0,	  255};
			for (size_t pixel = 0; pixel < alpha.size(); ++pixel)
				if (image.Pixels[pixel * 4] != 255 || image.Pixels[pixel * 4 + 1] != 255 ||
					image.Pixels[pixel * 4 + 2] != 255 || image.Pixels[pixel * 4 + 3] != alpha[pixel])
					return Refuse(failure, "coverage bytes differ from the literal BDF bitmap");
			return true;
		}
		inline bool FontOracle(const FontValue &value, FontKind kind, std::string &failure) {
			if (!value.Data) return Refuse(failure, "font payload is absent");
			const auto &font = *value.Data;
			const bool distance = kind == FontKind::Distance;
			const bool bitmap = kind == FontKind::BitmapInput || kind == FontKind::BitmapReceipt;
			const bool texture = !distance && kind != FontKind::BitmapInput;
			if (font.Raster != (distance ? FontRasterProfile::NativeSignedDistance
								: bitmap ? FontRasterProfile::BitmapSurface
										 : FontRasterProfile::NativeGlyphCoverage) ||
				font.Characters !=
					(bitmap ? FontCharacterProfile::Utf16 : FontCharacterProfile::UnicodeScalar) ||
				font.GlyphMapComplete != bitmap || font.DistanceSpread != (distance ? 4 : 0) ||
				font.LineHeight != (distance ? 3 : 10) || font.MissingAdvance != (distance ? 1 : 0) ||
				font.SpaceAdvance != (distance ? 2 : 4) ||
				font.Identity != (distance ? "fixture-distance" : "fixture-coverage") ||
				!font.HasCharacterRange || font.FirstCharacter != (distance ? 65u : 32u) ||
				font.LastCharacter != (distance ? 65u : 66u) || font.Frames.size() != 1 ||
				font.Glyphs.size() != (distance ? 1u : 3u) || font.Measurements.size() != 1 ||
				font.SourceTexture.has_value() != texture)
				return Refuse(failure, "font profile, range, identity, metrics or owned counts differ");
			if (!ImageOracle(font.Frames[0], distance, false, failure) ||
				(texture && !ImageOracle(*font.SourceTexture, false, true, failure)))
				return false;
			const auto &measurement = font.Measurements[0];
			if (measurement.Text != "A" || measurement.MaximumLineWidth != 20 || measurement.LineGap != 1 ||
				measurement.Width != (distance ? 2.25 : 8) || measurement.Height != (distance ? 3 : 10))
				return Refuse(failure, "literal font measurement differs");
			for (size_t index = 0; index < font.Glyphs.size(); ++index) {
				const auto &glyph = font.Glyphs[index];
				const bool framed = distance || index == 1;
				const bool rect = !distance && index == 1 && kind != FontKind::BitmapInput;
				if (glyph.Character != (distance ? 65u : std::array<uint32_t, 3>{32, 65, 66}[index]) ||
					glyph.Present != (distance || index != 2) || glyph.Frame.has_value() != framed ||
					(framed && *glyph.Frame != 0) ||
					glyph.Advance != (distance	   ? 2.25
									  : index == 0 ? 4
									  : index == 1 ? 8
												   : 0) ||
					glyph.Width != (distance	 ? 1
									: index == 0 ? 4
									: index == 1 ? 8
												 : 0) ||
					glyph.Height != (distance ? 1 : 10) ||
					glyph.Offset != (distance	  ? Vector2{-0.5, 0.25}
									 : index == 1 ? Vector2{-1, 3}
												  : Vector2{0, 8}) ||
					glyph.DistancePaddingPixels != (distance ? 4 : 0) ||
					glyph.TextureRectangle.has_value() != rect ||
					(rect && *glyph.TextureRectangle != Vector4{0, 0, 5, 7}))
					return Refuse(failure, "literal glyph presence, spacing, frame, bearing or UV differs");
			}
			return true;
		}
		inline bool ContextOracle(const SourceFontContext &context, std::string &failure) {
			if (context.TextCaseProfile != FontTextCaseProfile::UnicodeDefault ||
				context.BitmapTextureProfile != FontBitmapTextureProfile::NativeFrameUv ||
				!context.AliasMapKnown || context.Aliases.size() != 1 ||
				context.Aliases[0].first != "primary" || context.Aliases[0].second != "/fixture/font.bdf" ||
				context.Directory != "/fixture/" || context.ApplicationLocation != "/fixture/app/" ||
				context.ProjectPath != "/fixture/project/" ||
				context.DefaultFontPath != "/fixture/default.bdf" || context.Playing != false ||
				!context.InitialFont || context.TextTransforms.size() != 1 ||
				context.TextTransforms[0].Original != "A" || context.TextTransforms[0].Transformed != "a" ||
				context.TextTransforms[0].ChangeCase != 1 || context.InitialTextFonts.size() != 1 ||
				context.InitialTextFonts[0].NodeId != "text" || !context.InitialTextFonts[0].Primary ||
				!context.InitialTextFonts[0].Fallback)
				return Refuse(failure, "literal namespace, profiles, transforms or seed identity differs");
			return FontOracle(*context.InitialFont, FontKind::Coverage, failure) &&
				   FontOracle(*context.InitialTextFonts[0].Primary, FontKind::Coverage, failure) &&
				   FontOracle(*context.InitialTextFonts[0].Fallback, FontKind::Distance, failure);
		}
	}

	inline GraphFontConfiguration MakeArtifactFixture() {
		using namespace engine::imagegraph;
		GraphFontConfiguration fixture;
		auto &context = fixture.Context;
		context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
		context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
		context.AliasMapKnown = true;
		context.Aliases = {{"primary", "/fixture/font.bdf"}};
		context.Directory = "/fixture/";
		context.ApplicationLocation = "/fixture/app/";
		context.ProjectPath = "/fixture/project/";
		context.DefaultFontPath = "/fixture/default.bdf";
		context.Playing = false;
		context.TextTransforms = {{"A", "a", 1}};
		context.InitialFont = detail::CoverageFont();
		context.InitialTextFonts = {{"text", detail::CoverageFont(), detail::DistanceFont()}};
		const auto observation = [&](const char *id, const char *role, const char *path, bool sdf) {
			SourceFontObservation result;
			auto &request = result.Request;
			request.Authored = {
				id,
				"pc.text",
				"",
				{3, 4},
				{{"text", std::string{"A"}},
				 {"font", std::string{path}},
				 {"size", int64_t{10}},
				 {"offset", Vector2{1, 2}},
				 {"use_sdf", sdf}}
			};
			request.Authored.SourceDisplayName = "Fixture Text";
			request.Authored.SourceInternalName = "Node_Text";
			request.Context = context;
			request.ProcessorRow = 2;
			request.Tick = 17;
			request.Subframe = 0.5;
			request.NegativeFrame = true;
			request.Role = role;
			request.ResolvedPath = path;
			request.PixelSize = 10;
			request.Antialias = false;
			request.SignedDistanceField = sdf;
			return result;
		};
		auto coverage = observation("text", "font", "/fixture/font.bdf", false);
		coverage.Request.Characters = {32, 65, 66};
		coverage.Request.Measurements = {{"A", 20, 1, 0, 0}};
		coverage.Font = detail::CoverageFont();
		fixture.Observations.push_back(std::move(coverage));
		auto distance = observation("sdf", "font", "/fixture/sdf.bdf", true);
		distance.Request.Characters = {65};
		distance.Font = detail::DistanceFont();
		fixture.Observations.push_back(std::move(distance));
		auto bitmap = observation("bitmap", "bitmap_texture", "", false);
		bitmap.Request.Characters = {32, 65, 66};
		bitmap.Request.FontInput = detail::CoverageFont();
		bitmap.Request.FontInput->Data->Raster = FontRasterProfile::BitmapSurface;
		bitmap.Request.FontInput->Data->Characters = FontCharacterProfile::Utf16;
		bitmap.Request.FontInput->Data->GlyphMapComplete = true;
		bitmap.Request.FontInput->Data->SourceTexture.reset();
		bitmap.Request.FontInput->Data->Glyphs[1].TextureRectangle.reset();
		bitmap.Font = *bitmap.Request.FontInput;
		bitmap.Font->Data->SourceTexture = detail::CoverageFont().Data->SourceTexture;
		bitmap.Font->Data->Glyphs[1].TextureRectangle = Vector4{0, 0, 5, 7};
		fixture.Observations.push_back(std::move(bitmap));
		auto absent = observation("absent", "fallback_font", "/fixture/missing.bdf", false);
		absent.Request.Characters = {65};
		absent.Presence = SourceFontPresence::AbsentFile;
		fixture.Observations.push_back(std::move(absent));
		fixture.ReadGrants = {
			{"text", "/fixture/font.bdf", false, "font"},
			{"sdf", "/fixture/sdf.bdf", false, "font"},
			{"absent", "/fixture/missing.bdf", false, "fallback_font"}
		};
		return fixture;
	}

	inline bool VerifyArtifactConfiguration(const GraphFontConfiguration &fixture, std::string &failure) {
		using namespace engine::imagegraph;
		if (!detail::ContextOracle(fixture.Context, failure)) return false;
		if (fixture.Observations.size() != 4 || fixture.ReadGrants.size() != 3)
			return detail::Refuse(failure, "literal observation or read grant count differs");
		constexpr std::array ids{"text", "sdf", "bitmap", "absent"};
		constexpr std::array roles{"font", "font", "bitmap_texture", "fallback_font"};
		constexpr std::array paths{"/fixture/font.bdf", "/fixture/sdf.bdf", "", "/fixture/missing.bdf"};
		for (size_t index = 0; index < fixture.Observations.size(); ++index) {
			const auto &observed = fixture.Observations[index];
			const auto &request = observed.Request;
			const auto &node = request.Authored;
			if (!detail::ContextOracle(request.Context, failure)) return false;
			if (request.ProcessorRow != 2 || request.Tick != 17 || request.Subframe != 0.5 ||
				!request.NegativeFrame || request.Role != roles[index] ||
				request.ResolvedPath != paths[index] || request.PixelSize != 10 || request.Antialias ||
				request.SignedDistanceField != (index == 1) ||
				request.FontInput.has_value() != (index == 2) ||
				request.Characters != (index == 0 || index == 2 ? std::vector<uint32_t>{32, 65, 66}
																: std::vector<uint32_t>{65}) ||
				request.Measurements.size() != (index == 0 ? 1u : 0u) ||
				observed.Presence !=
					(index == 3 ? SourceFontPresence::AbsentFile : SourceFontPresence::Present) ||
				observed.Font.has_value() != (index != 3))
				return detail::Refuse(
					failure, "literal request identity, controls or observation presence differs"
				);
			if (index == 0) {
				const auto &measurement = request.Measurements[0];
				if (measurement.Text != "A" || measurement.MaximumLineWidth != 20 ||
					measurement.LineGap != 1 || measurement.Width != 0 || measurement.Height != 0)
					return detail::Refuse(failure, "literal request measurement differs");
			}
			if (node.Id != ids[index] || node.Type != "pc.text" || !node.GroupId.empty() ||
				node.Position != Vector2{3, 4} || node.SourceDisplayName != "Fixture Text" ||
				node.SourceInternalName != "Node_Text" || node.Values.size() != 5 ||
				!node.DynamicInputs.empty() || !node.DynamicOutputs.empty() || !node.InstanceBase.empty() ||
				!node.InstanceOverrides.empty() || !node.SourceAnimatedInputs.empty() ||
				!node.SourceStaticInputs.empty() || !node.SourceInputExpressions.empty() ||
				!node.SourceProperties.empty() || node.SourceSeparatedVec2Animators ||
				!node.NativeSamplerBindings.empty())
				return detail::Refuse(failure, "literal native authored node metadata differs");
			const auto value = [&](size_t slot, const char *port, const auto &expected) {
				using T = std::decay_t<decltype(expected)>;
				const auto *actual = std::get_if<T>(&node.Values[slot].Data);
				return node.Values[slot].Port == port && actual && *actual == expected;
			};
			if (!value(0, "text", std::string{"A"}) || !value(1, "font", std::string{paths[index]}) ||
				!value(2, "size", int64_t{10}) || !value(3, "offset", Vector2{1, 2}) ||
				!value(4, "use_sdf", index == 1))
				return detail::Refuse(failure, "literal native authored values differ");
			if (index != 3 && !detail::FontOracle(
								  *observed.Font,
								  index == 1   ? detail::FontKind::Distance
								  : index == 2 ? detail::FontKind::BitmapReceipt
											   : detail::FontKind::Coverage,
								  failure
							  ))
				return false;
			if (index == 2 && !detail::FontOracle(*request.FontInput, detail::FontKind::BitmapInput, failure))
				return false;
		}
		constexpr std::array<size_t, 3> grantIndices{0, 1, 3};
		for (size_t slot = 0; slot < grantIndices.size(); ++slot) {
			const auto index = grantIndices[slot];
			const auto &grant = fixture.ReadGrants[slot];
			if (grant.NodeId != ids[index] || grant.File != paths[index] || grant.Write ||
				grant.Resource != roles[index])
				return detail::Refuse(failure, "literal read capability differs");
		}
		failure.clear();
		return true;
	}

	inline bool VerifyEncodedArtifact(std::string_view bytes, std::string &failure) {
		constexpr std::array tokens{
			R"("schema":"atomic.font_inputs.v1")",
			R"("textCaseProfile":"unicode_default")",
			R"("bitmapTextureProfile":"native_frame_uv")",
			R"("raster":"native_glyph_coverage")",
			R"("raster":"native_signed_distance")",
			R"("raster":"bitmap_surface")",
			R"("characters":"unicode_scalar")",
			R"("characters":"utf16")",
			R"("format":"rgba8_unorm")",
			R"("presence":"absent_file")",
			R"("pixelsHex":"ffffff2c")",
			R"("authored":"imagegraph 9\nnode \"text\" \"pc.text\")"
		};
		for (const auto *token : tokens)
			if (bytes.find(token) == std::string_view::npos)
				return detail::Refuse(
					failure, "literal schema, profile, sample or native singleton token is absent"
				);
		for (uint64_t hash : {CoverageFrameHash, TextureFrameHash, DistanceFrameHash})
			if (bytes.find("\"storedHash\":" + std::to_string(hash)) == std::string_view::npos)
				return detail::Refuse(failure, "independent literal stored hash is absent");
		failure.clear();
		return true;
	}
}
