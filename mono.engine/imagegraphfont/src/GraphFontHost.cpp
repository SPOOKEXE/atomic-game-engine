#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/gui/FontGlyphs.hpp>
#include <engine/imagegraphfont/GraphFontHost.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

namespace engine::imagegraphfont {
	GraphFontHost::GraphFontHost(std::span<const GraphFontFileGrant> grants, assets::ContentPolicy policy)
		: Policy(policy) {
		using imagegraph::Limits;
		if (grants.size() > Limits::MaximumNodes) throw std::length_error("font grants exceed node limit");
		uint64_t bytes = sizeof(*this) + grants.size() * sizeof(FontGrant);
		for (const auto &grant : grants) {
			if (grant.NodeId.empty() || grant.NodeId.size() > Limits::MaximumTextBytes ||
				grant.Resource.size() > Limits::MaximumTextBytes ||
				grant.File.native().size() > Limits::MaximumTextBytes)
				throw std::length_error("font grant exceeds text limit");
			const uint64_t text =
				grant.NodeId.size() + grant.Resource.size() + grant.File.native().size() * 4 + 3 * 32;
			if (text > Limits::MaximumEvaluationBytes - bytes)
				throw std::length_error("font grants exceed retained byte limit");
			bytes += text;
		}
		Grants.reserve(grants.size());
		for (const auto &grant : grants)
			Grants.push_back({grant.NodeId, grant.File.string(), grant.Resource, grant.Write});
		GrantBytes = sizeof(*this) + Grants.capacity() * sizeof(FontGrant);
		for (const auto &grant : Grants)
			GrantBytes += grant.NodeId.capacity() + grant.Resource.capacity() + grant.Path.capacity() + 3;
		if (GrantBytes > Limits::MaximumEvaluationBytes)
			throw std::length_error("font grant capacities exceed byte limit");
	}
	bool GraphFontHost::Observe(
		const imagegraph::SourceFontRequest &request,
		uint64_t maximumBytes,
		imagegraph::SourceFontObservation &output,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraphfont.font_glyphs");
		using namespace imagegraph;
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		maximumBytes = std::min(maximumBytes, uint64_t{gui::MAXIMUM_FONT_GLYPH_OPERATION_BYTES});
		const auto requestBytes = SourceFontRequestRetainedBytes(request);
		uint64_t priorBytes = 0;
		if (!output.Request.Authored.Id.empty()) {
			const auto prior = SourceFontObservationRetainedBytes(output);
			if (!prior) return fail("prior font observation is malformed");
			priorBytes = *prior;
		}
		if (!requestBytes || GrantBytes > maximumBytes || priorBytes > maximumBytes - GrantBytes ||
			*requestBytes > (maximumBytes - GrantBytes - priorBytes) / 2)
			return fail("font observation request and replacement exceed operation budget");
		if (request.Role == "bitmap_texture" || request.FontInput)
			return fail("bitmap atlas observations never use the file font provider");

		if (!request.Measurements.empty())
			return fail("source whole-string measurements require owned source observations");
		const FontGrant *selected = nullptr;
		for (const auto &grant : Grants)
			if (grant.NodeId == request.Authored.Id && grant.Resource == request.Role &&
				grant.Path == request.ResolvedPath) {
				if (selected) return fail("font resource has duplicate exact grants");
				selected = &grant;
			}
		if (!selected || selected->Write || selected->Path != request.ResolvedPath ||
			!Policy.AllowsName(request.ResolvedPath) || request.ResolvedPath.find('\0') != std::string::npos)
			return fail("font path or operation differs from its exact content grant");
		const uint64_t fixedBytes = GrantBytes + priorBytes + 2 * *requestBytes;
		std::error_code error;
		const auto status = std::filesystem::status(selected->Path, error);
		if ((!error && !std::filesystem::exists(status)) || error == std::errc::no_such_file_or_directory) {
			SourceFontObservation candidate;
			candidate.Request = request;
			candidate.Presence = SourceFontPresence::AbsentFile;
			const auto retained = SourceFontObservationRetainedBytes(candidate);
			if (!retained || *retained > maximumBytes - GrantBytes - priorBytes)
				return fail("absent font observation capacities exceed operation budget");
			output = std::move(candidate);
			failure.clear();
			return true;
		}
		if (error || !std::filesystem::is_regular_file(status))
			return fail("font grant is not a readable regular file");
		const uint64_t fileBytes = std::filesystem::file_size(selected->Path, error);
		if (error || !fileBytes || fileBytes > gui::MAXIMUM_FONT_GLYPH_FILE_BYTES ||
			fileBytes > maximumBytes - fixedBytes)
			return fail("font file exceeds byte budget or cannot be inspected");
		std::vector<std::byte> bytes(static_cast<size_t>(fileBytes));
		if (bytes.capacity() > maximumBytes - fixedBytes)
			return fail("font file capacity exceeds operation budget");
		std::ifstream stream(selected->Path, std::ios::binary);
		stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!stream || static_cast<size_t>(stream.gcount()) != bytes.size() ||
			stream.peek() != std::char_traits<char>::eof())
			return fail("font file changed or could not be read completely");
		// The decoder charges provided bytes itself. Partition remaining memory equally between its
		// coverage/vendor workspace and the simultaneous RGBA font/receipt candidate.
		const uint64_t remaining = maximumBytes - fixedBytes;
		if (remaining <= bytes.capacity() || (remaining - bytes.capacity()) / 2 < fileBytes)
			return fail("font decode and owned conversion exceed operation budget");
		const uint64_t decoderCap = (remaining - bytes.capacity()) / 2;
		core::Metrics::Count("imagegraphfont.font.input_bytes", fileBytes);
		gui::FontGlyphBatch decoded;
		gui::FontGlyphRequest glyphRequest{
			bytes,
			request.Characters,
			static_cast<uint16_t>(request.PixelSize),
			request.Antialias,
			static_cast<size_t>(decoderCap)
		};
		glyphRequest.Raster = request.SignedDistanceField ? gui::FontGlyphRaster::SignedDistance
														  : gui::FontGlyphRaster::Coverage;
		glyphRequest.DistanceSpread = 8;
		if (gui::DecodeFontGlyphs(glyphRequest, decoded) != gui::FontGlyphStatus::Ok)
			return fail("font glyph decoder refused exact granted bytes or operation budget");
		uint64_t fontBytes = sizeof(FontData) + request.ResolvedPath.size() + 32 +
							 decoded.Glyphs.size() * (sizeof(FontGlyph) + sizeof(Image));
		for (const auto &glyph : decoded.Glyphs) {
			const uint64_t pixels = uint64_t{glyph.Width} * glyph.Height;
			if (pixels > Limits::MaximumArrayBytes / 4 || fontBytes > Limits::MaximumArrayBytes - pixels * 4)
				return fail("decoded font exceeds owned glyph pixel limit");
			fontBytes += pixels * 4;
		}
		if (fontBytes > Limits::MaximumArrayBytes || fontBytes > decoderCap)
			return fail("decoded font conversion exceeds owned byte budget");
		SourceFontObservation candidate;
		candidate.Request = request;
		candidate.Font.emplace();
		auto &font = candidate.Font->Data.emplace();
		font.Raster = request.SignedDistanceField ? FontRasterProfile::NativeSignedDistance
												  : FontRasterProfile::NativeGlyphCoverage;
		font.DistanceSpread = decoded.DistanceSpread;
		font.Characters = FontCharacterProfile::UnicodeScalar;
		font.GlyphMapComplete = false;
		font.Identity = request.ResolvedPath;
		font.LineHeight = decoded.LineHeightPixels;
		font.Glyphs.reserve(decoded.Glyphs.size());
		font.Frames.reserve(decoded.Glyphs.size());
		for (const auto &glyph : decoded.Glyphs) {
			FontGlyph owned;
			owned.DistancePaddingPixels = glyph.DistancePaddingPixels;
			owned.Character = glyph.Character;
			owned.Present = glyph.Present;
			owned.Advance = glyph.AdvanceXPixels;
			owned.Width = std::max(0.0, glyph.AdvanceXPixels);
			owned.Height = decoded.LineHeightPixels;
			owned.Offset = {double(glyph.OffsetXPixels), decoded.AscentPixels + glyph.OffsetYPixels};
			if (glyph.Character == 32) font.SpaceAdvance = glyph.AdvanceXPixels;
			if (glyph.Present && glyph.Width && glyph.Height) {
				owned.Frame = static_cast<uint32_t>(font.Frames.size());
				Image image{glyph.Width, glyph.Height, std::vector<uint8_t>(glyph.Coverage.size() * 4)};
				core::Metrics::Count("imagegraphfont.font.converted_payload_bytes", image.Pixels.size());
				core::Metrics::Count("imagegraphfont.font.converted_glyphs", 1);
				for (size_t i = 0; i < glyph.Coverage.size(); ++i) {
					image.Pixels[i * 4] = image.Pixels[i * 4 + 1] = image.Pixels[i * 4 + 2] = 255;
					image.Pixels[i * 4 + 3] = glyph.Coverage[i];
				}
				font.Frames.push_back(std::move(image));
			}
			font.Glyphs.push_back(std::move(owned));
		}
		const auto retained = SourceFontObservationRetainedBytes(candidate);
		if (!retained ||
			*retained > maximumBytes - GrantBytes - priorBytes - bytes.capacity() - decoded.RetainedBytes)
			return fail("font observation retained capacities exceed operation budget");
		output = std::move(candidate);
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "font observation allocation failed";
		return false;
	} catch (const std::length_error &) {
		failure = "font observation allocation length exceeds limits";
		return false;
	}
}
