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
	GraphFontHost::GraphFontHost(
		std::span<const GraphFontFileGrant> grants, assets::ContentPolicy policy, uint64_t maximumBytes
	)
		: Policy(policy) {
		using imagegraph::Limits;
		if (grants.size() > Limits::MaximumNodes) throw std::length_error("font grants exceed node limit");
		const uint64_t maximum = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		uint64_t bytes =
			sizeof(*this) + grants.size() * sizeof(FontGrant) + MaximumCachedFonts * sizeof(CachedFont);
		if (bytes > maximum) throw std::length_error("font cache metadata exceeds retained byte limit");
		for (const auto &grant : grants) {
			if (grant.NodeId.empty() || grant.NodeId.size() > Limits::MaximumTextBytes ||
				grant.Resource.size() > Limits::MaximumTextBytes ||
				grant.File.native().size() > Limits::MaximumTextBytes)
				throw std::length_error("font grant exceeds text limit");
			const uint64_t text =
				grant.NodeId.size() + grant.Resource.size() + grant.File.native().size() * 4 + 3 * 32;
			if (text > maximum - bytes) throw std::length_error("font grants exceed retained byte limit");
			bytes += text;
		}
		Cache.reserve(MaximumCachedFonts);
		Grants.reserve(grants.size());
		for (const auto &grant : grants)
			Grants.push_back({grant.NodeId, grant.File.string(), grant.Resource, grant.Write});
		GrantBytes =
			sizeof(*this) + Grants.capacity() * sizeof(FontGrant) + Cache.capacity() * sizeof(CachedFont);
		for (const auto &grant : Grants)
			GrantBytes += grant.NodeId.capacity() + grant.Resource.capacity() + grant.Path.capacity() + 3;
		if (GrantBytes > maximum) throw std::length_error("font grant capacities exceed byte limit");
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
		const uint64_t residentBytes = RetainedBytes();
		const auto requestBytes = SourceFontRequestRetainedBytes(request);
		uint64_t priorBytes = 0;
		if (!output.Request.Authored.Id.empty()) {
			const auto prior = SourceFontObservationRetainedBytes(output);
			if (!prior) return fail("prior font observation is malformed");
			priorBytes = *prior;
		}
		if (!requestBytes || residentBytes > maximumBytes || priorBytes > maximumBytes - residentBytes ||
			*requestBytes > (maximumBytes - residentBytes - priorBytes) / 2)
			return fail("font observation request and replacement exceed operation budget");
		if (request.Role == "bitmap_texture" || request.FontInput)
			return fail("bitmap atlas observations never use the file font provider");

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
		const uint64_t fixedBytes = residentBytes + priorBytes + 2 * *requestBytes;
		std::error_code error;
		const auto status = std::filesystem::status(selected->Path, error);
		if ((!error && !std::filesystem::exists(status)) || error == std::errc::no_such_file_or_directory) {
			SourceFontObservation candidate;
			candidate.Request = request;
			candidate.Presence = SourceFontPresence::AbsentFile;
			const auto retained = SourceFontObservationRetainedBytes(candidate);
			if (!retained || *retained > maximumBytes - residentBytes - priorBytes)
				return fail("absent font observation capacities exceed operation budget");
			output = std::move(candidate);
			failure.clear();
			return true;
		}
		if (error || !std::filesystem::is_regular_file(status))
			return fail("font grant is not a readable regular file");
		const CachedFont *cached = nullptr;
		for (const auto &entry : Cache)
			if (entry.Path == request.ResolvedPath && entry.PixelSize == request.PixelSize &&
				entry.Antialias == request.Antialias &&
				entry.SignedDistanceField == request.SignedDistanceField) {
				cached = &entry;
				break;
			}
		CachedFont pending;
		uint64_t pendingBytes = 0;
		if (!cached) {
			if (Cache.size() == MaximumCachedFonts) return fail("font lifetime cache entry limit reached");
			const uint64_t fileBytes = std::filesystem::file_size(selected->Path, error);
			const uint64_t keyQuote = request.ResolvedPath.size() + 32;
			if (error || !fileBytes || fileBytes > gui::MAXIMUM_FONT_GLYPH_FILE_BYTES ||
				keyQuote > MaximumCacheBytes - CacheBytes ||
				fileBytes > MaximumCacheBytes - CacheBytes - keyQuote ||
				keyQuote > maximumBytes - fixedBytes || fileBytes > maximumBytes - fixedBytes - keyQuote)
				return fail("font file or lifetime cache exceeds byte budget");
			pending.Path = request.ResolvedPath;
			pending.PixelSize = request.PixelSize;
			pending.Antialias = request.Antialias;
			pending.SignedDistanceField = request.SignedDistanceField;
			pending.Bytes.resize(static_cast<size_t>(fileBytes));
			pendingBytes = pending.Path.capacity() + 1 + pending.Bytes.capacity();
			if (pendingBytes > MaximumCacheBytes - CacheBytes || pendingBytes > maximumBytes - fixedBytes)
				return fail("font cache capacities exceed operation budget");
			std::ifstream stream(selected->Path, std::ios::binary);
			stream.read(
				reinterpret_cast<char *>(pending.Bytes.data()),
				static_cast<std::streamsize>(pending.Bytes.size())
			);
			if (!stream || static_cast<size_t>(stream.gcount()) != pending.Bytes.size() ||
				stream.peek() != std::char_traits<char>::eof())
				return fail("font file changed or could not be read completely");
			core::Metrics::Count("imagegraphfont.font.input_bytes", fileBytes);
		} else
			core::Metrics::Count("imagegraphfont.font.cache_hits", 1);
		const std::span<const std::byte> bytes =
			cached ? std::span<const std::byte>{cached->Bytes} : std::span<const std::byte>{pending.Bytes};
		const uint64_t fileBytes = bytes.size();
		// The decoder charges provided bytes itself. Partition remaining memory equally between its
		// coverage/vendor workspace and the simultaneous RGBA font/receipt candidate.
		const uint64_t remaining = maximumBytes - fixedBytes;
		if (remaining <= pendingBytes || (remaining - pendingBytes) / 2 < fileBytes)
			return fail("font decode and owned conversion exceed operation budget");
		const uint64_t decoderCap = (remaining - pendingBytes) / 2;
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
		if (!request.Measurements.empty()) {
			const auto heldCandidate = SourceFontObservationRetainedBytes(candidate);
			uint64_t held = residentBytes + priorBytes + *requestBytes;
			if (!heldCandidate || pendingBytes > maximumBytes - held ||
				decoded.RetainedBytes > maximumBytes - held - pendingBytes ||
				*heldCandidate > maximumBytes - held - pendingBytes - decoded.RetainedBytes)
				return fail("native font measurement coexistence exceeds operation budget");
			held += pendingBytes + decoded.RetainedBytes + *heldCandidate;
			// The child operation also counts its borrowed font/request backing. Keeping
			// that conservative reservation prevents measurement growth using decode storage.
			const auto measured = MeasureNativeSourceFont(
				*candidate.Font,
				request.Measurements,
				maximumBytes - held,
				16u * 1024u * 1024u,
				font.Measurements,
				failure
			);
			if (measured != Status::Ok) return false;
		}
		const auto retained = SourceFontObservationRetainedBytes(candidate);
		if (!retained ||
			*retained > maximumBytes - residentBytes - priorBytes - pendingBytes - decoded.RetainedBytes)
			return fail("font observation retained capacities exceed operation budget");
		// grug keep successful bytes for this host revision. no eviction changes a live font silently.
		if (!cached) {
			Cache.push_back(std::move(pending));
			CacheBytes += pendingBytes;
		}
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
