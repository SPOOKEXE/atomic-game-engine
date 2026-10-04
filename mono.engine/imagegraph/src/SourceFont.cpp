#include "FontPayload.hpp"
#include "FontUnicode.hpp"
#include "SourceSeparatedVec2.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceFont.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::imagegraph {
	namespace {
		struct FontBytes {
			uint64_t Bytes = 0;
			bool Add(uint64_t count, uint64_t unit = 1) {
				if (!unit || Bytes > Limits::MaximumEvaluationBytes ||
					count > (Limits::MaximumEvaluationBytes - Bytes) / unit)
					return false;
				Bytes += count * unit;
				return true;
			}
			bool Text(const std::string &text) {
				return text.size() <= Limits::MaximumTextBytes && Add(text.capacity());
			}
		};
		bool FontAuthoredBytes(const Node &node, FontBytes &bytes) {
			const auto cloned = NodeClonePayloadBytes(node);
			if (!cloned || !bytes.Add(*cloned)) return false;
			const auto separated = detail::SeparatedVec2Bytes(node, false);
			const auto retainedSeparated = detail::SeparatedVec2Bytes(node, true);
			if (!separated || !retainedSeparated ||
				(*retainedSeparated > *separated && !bytes.Add(*retainedSeparated - *separated)))
				return false;

			const auto extra = [&](const auto &items) {
				return bytes.Add(
					items.capacity() - items.size(),
					sizeof(typename std::decay_t<decltype(items)>::value_type)
				);
			};
			const auto textSlack = [&](const std::string &text) {
				const auto copied = std::max(text.size(), std::string{}.capacity());
				return bytes.Add(text.capacity() > copied ? text.capacity() - copied : 0);
			};
			if (!extra(node.Values) || !extra(node.SourceProperties) || !extra(node.DynamicInputs) ||
				!extra(node.DynamicOutputs) || !extra(node.InstanceOverrides) ||
				!extra(node.SourceAnimatedInputs) || !extra(node.SourceStaticInputs) ||
				!extra(node.SourceInputExpressions) || !extra(node.NativeSamplerBindings))
				return false;
			for (const auto *text :
				 {&node.Id,
				  &node.Type,
				  &node.GroupId,
				  &node.InstanceBase,
				  &node.SourceDisplayName,
				  &node.SourceInternalName})
				if (!textSlack(*text)) return false;
			for (const auto *values : {&node.Values, &node.SourceProperties})
				for (const auto &value : *values)
					if (!textSlack(value.Port)) return false;
			for (const auto &input : node.DynamicInputs)
				if (!textSlack(input.Id) || !textSlack(input.SourceInputId) ||
					!textSlack(input.SourceLayerName))
					return false;
			for (const auto &output : node.DynamicOutputs)
				if (!textSlack(output.Id)) return false;
			for (const auto *list :
				 {&node.InstanceOverrides, &node.SourceAnimatedInputs, &node.SourceStaticInputs})
				for (const auto &text : *list)
					if (!textSlack(text)) return false;
			for (const auto &expression : node.SourceInputExpressions)
				if (!textSlack(expression.Port) || !textSlack(expression.Code)) return false;
			for (const auto &binding : node.NativeSamplerBindings)
				if (!textSlack(binding.Argument) || !textSlack(binding.Texture)) return false;
			return true;
		}
	}
	namespace detail {
		std::optional<uint64_t> SourceFontAuthoredRetainedBytes(const Node &node) {
			FontBytes bytes;
			if (!FontAuthoredBytes(node, bytes)) return {};
			return bytes.Bytes;
		}
	}
	std::optional<uint64_t> SourceFontValueRetainedBytes(const FontValue &font) {
		if (!detail::ValidFontPayload(font)) return {};
		const auto bytes = detail::FontStorageBytes(font, true);
		if (bytes > Limits::MaximumEvaluationBytes) return {};
		return bytes;
	}
	std::optional<uint64_t> SourceFontContextRetainedBytes(const SourceFontContext &context) {
		FontBytes bytes{sizeof(context)};
		if (uint8_t(context.TextCaseProfile) > uint8_t(FontTextCaseProfile::UnicodeDefault) ||
			uint8_t(context.BitmapTextureProfile) > uint8_t(FontBitmapTextureProfile::NativeFrameUv) ||
			context.Aliases.size() > detail::MaximumFontGlyphs ||
			(!context.AliasMapKnown && !context.Aliases.empty()) ||
			!bytes.Add(context.Aliases.capacity(), sizeof(std::pair<std::string, std::string>)))
			return {};
		for (const auto &[name, path] : context.Aliases)
			if (!bytes.Text(name) || !bytes.Text(path)) return {};
		for (const auto *text :
			 {&context.Directory,
			  &context.ApplicationLocation,
			  &context.ProjectPath,
			  &context.DefaultFontPath})
			if (*text && !bytes.Text(**text)) return {};
		if (context.TextTransforms.size() > detail::MaximumFontGlyphs ||
			!bytes.Add(context.TextTransforms.capacity(), sizeof(SourceFontTextTransform)))
			return {};
		uint64_t transformBytes = 0;
		for (const auto &record : context.TextTransforms) {
			if (record.ChangeCase < 1 || record.ChangeCase > 3 || !bytes.Text(record.Original) ||
				!bytes.Text(record.Transformed))
				return {};
			if (record.Original.capacity() > Limits::MaximumArrayBytes - transformBytes) return {};
			transformBytes += record.Original.capacity();
			if (record.Transformed.capacity() > Limits::MaximumArrayBytes - transformBytes) return {};
			transformBytes += record.Transformed.capacity();
			for (const auto *text : {&record.Original, &record.Transformed}) {
				detail::FontScalarCursor cursor{*text};
				uint32_t point = 0;
				while (cursor.Next(point)) {}
				if (cursor.Invalid) return {};
			}
		}
		if (!context.TextTransforms.empty() &&
			transformBytes > (16 * 1024 * 1024) / context.TextTransforms.size())
			return {};
		for (size_t i = 0; i < context.TextTransforms.size(); ++i)
			for (size_t j = 0; j < i; ++j)
				if (context.TextTransforms[i].Original == context.TextTransforms[j].Original &&
					context.TextTransforms[i].ChangeCase == context.TextTransforms[j].ChangeCase)
					return {};

		if (context.InitialFont && (!detail::ValidFontPayload(*context.InitialFont) ||
									!bytes.Add(detail::FontStorageBytes(*context.InitialFont, true))))
			return {};
		if (context.InitialTextFonts.size() > Limits::MaximumNodes ||
			!bytes.Add(context.InitialTextFonts.capacity(), sizeof(SourceFontInitialTextState)))
			return {};
		uint64_t seedNames = 0;
		for (const auto &state : context.InitialTextFonts) {
			if (state.NodeId.empty() || !bytes.Text(state.NodeId)) return {};
			seedNames += state.NodeId.size();
			for (const auto *font : {&state.Primary, &state.Fallback})
				if (*font &&
					(!detail::ValidFontPayload(**font) || !bytes.Add(detail::FontStorageBytes(**font, true))))
					return {};
		}
		if (!context.InitialTextFonts.empty() &&
			seedNames > (16 * 1024 * 1024) / context.InitialTextFonts.size())
			return {};
		for (size_t i = 0; i < context.InitialTextFonts.size(); ++i)
			for (size_t j = 0; j < i; ++j)
				if (context.InitialTextFonts[i].NodeId == context.InitialTextFonts[j].NodeId) return {};
		return bytes.Bytes;
	}
	namespace {
		bool BitmapObservationMatchesInput(const SourceFontRequest &request, const FontValue &value) {
			if (!request.FontInput || !request.FontInput->Data || !value.Data) return false;
			const auto &input = *request.FontInput->Data;
			const auto &output = *value.Data;
			if (input.Raster != FontRasterProfile::BitmapSurface || output.Raster != input.Raster ||
				output.Characters != input.Characters || output.GlyphMapComplete != input.GlyphMapComplete ||
				output.Frames != input.Frames || output.Measurements != input.Measurements ||
				output.DistanceSpread != input.DistanceSpread || output.LineHeight != input.LineHeight ||
				output.MissingAdvance != input.MissingAdvance || output.SpaceAdvance != input.SpaceAdvance ||
				output.FirstCharacter != input.FirstCharacter ||
				output.LastCharacter != input.LastCharacter ||
				output.HasCharacterRange != input.HasCharacterRange || output.Identity != input.Identity ||
				output.Glyphs.size() != input.Glyphs.size() || !output.SourceTexture ||
				(input.SourceTexture && output.SourceTexture != input.SourceTexture))
				return false;
			for (size_t i = 0; i < input.Glyphs.size(); ++i) {
				const auto &before = input.Glyphs[i];
				const auto &after = output.Glyphs[i];
				if (before.Character != after.Character || before.Present != after.Present ||
					before.Frame != after.Frame || before.Advance != after.Advance ||
					before.Width != after.Width || before.Height != after.Height ||
					before.Offset != after.Offset ||
					before.DistancePaddingPixels != after.DistancePaddingPixels ||
					(before.TextureRectangle && after.TextureRectangle != before.TextureRectangle))
					return false;
			}
			const auto hasRectangle = [&](uint32_t character) {
				const auto glyph = std::lower_bound(
					output.Glyphs.begin(),
					output.Glyphs.end(),
					character,
					[](const FontGlyph &entry, uint32_t point) { return entry.Character < point; }
				);
				return glyph == output.Glyphs.end() || glyph->Character != character || !glyph->Present ||
					   !glyph->Frame || glyph->TextureRectangle.has_value();
			};
			for (uint32_t character : request.Characters) {
				if (output.Characters == FontCharacterProfile::Utf16 && character > 0xffff) {
					character -= 0x10000;
					if (!hasRectangle(0xd800 + (character >> 10)) ||
						!hasRectangle(0xdc00 + (character & 1023)))
						return false;
				} else if (!hasRectangle(character))
					return false;
			}
			return true;
		}
	}
	std::optional<uint64_t> SourceFontRequestRetainedBytes(const SourceFontRequest &request) {
		FontBytes bytes{sizeof(request)};
		const bool bitmap = request.Role == "bitmap_texture";
		if (bitmap) {
			if (request.Authored.Type != "pc.text" || !request.ResolvedPath.empty() ||
				request.SignedDistanceField || !request.Measurements.empty() || !request.FontInput ||
				!detail::ValidFontPayload(*request.FontInput) ||
				request.FontInput->Data->Raster != FontRasterProfile::BitmapSurface ||
				!bytes.Add(detail::FontStorageBytes(*request.FontInput, true)))
				return {};
		} else if (request.FontInput)
			return {};

		const auto contextBytes = SourceFontContextRetainedBytes(request.Context);
		if (!contextBytes || !bytes.Add(*contextBytes - sizeof(SourceFontContext))) return {};
		if ((request.Authored.Type != "pc.text" && request.Authored.Type != "pc.font_data") ||
			request.Authored.Id.empty() ||
			!ValidFrameTime({request.Tick, request.Subframe, request.NegativeFrame}) ||
			request.ProcessorRow >= Limits::MaximumArrayElements || request.PixelSize == 0 ||
			request.PixelSize > 512 ||
			(!bitmap && request.Role != "font" && request.Role != "fallback_font" &&
			 request.Role != "initial_font") ||
			!FontAuthoredBytes(request.Authored, bytes) || !bytes.Text(request.Role) ||
			!bytes.Text(request.ResolvedPath) || request.Characters.size() > detail::MaximumFontGlyphs ||
			request.Measurements.size() > detail::MaximumFontGlyphs ||
			!bytes.Add(request.Characters.capacity(), sizeof(uint32_t)) ||
			!bytes.Add(request.Measurements.capacity(), sizeof(FontMeasurement)))
			return {};
		for (size_t i = 0; i < request.Characters.size(); ++i) {
			const auto character = request.Characters[i];
			if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff) ||
				(i && request.Characters[i - 1] >= character))
				return {};
		}
		for (const auto &measurement : request.Measurements)
			if (!bytes.Text(measurement.Text) || !std::isfinite(measurement.MaximumLineWidth) ||
				!std::isfinite(measurement.LineGap) || measurement.Width != 0 || measurement.Height != 0)
				return {};
		return bytes.Bytes;
	}
	std::optional<uint64_t> SourceFontObservationRetainedBytes(const SourceFontObservation &observation) {
		const auto &request = observation.Request;
		const auto retained = SourceFontRequestRetainedBytes(request);
		if (!retained) return {};
		FontBytes bytes{*retained + sizeof(observation) - sizeof(request)};
		if (observation.Presence == SourceFontPresence::AbsentFile)
			return observation.Font || request.Role == "bitmap_texture"
					   ? std::nullopt
					   : std::optional<uint64_t>{bytes.Bytes};
		if (observation.Presence != SourceFontPresence::Present || !observation.Font ||
			!detail::ValidFontPayload(*observation.Font) ||
			(request.SignedDistanceField &&
			 observation.Font->Data->Raster != FontRasterProfile::SourceObserved &&
			 observation.Font->Data->Raster != FontRasterProfile::NativeSignedDistance) ||
			(!request.SignedDistanceField &&
			 observation.Font->Data->Raster == FontRasterProfile::NativeSignedDistance) ||
			!bytes.Add(detail::FontStorageBytes(*observation.Font, true)))
			return {};
		if (request.Role == "bitmap_texture") {
			if (!BitmapObservationMatchesInput(request, *observation.Font)) return {};
			return bytes.Bytes;
		}
		if (observation.Font->Data->Characters != FontCharacterProfile::UnicodeScalar) return {};
		const auto &glyphs = observation.Font->Data->Glyphs;
		for (const auto character : request.Characters) {
			const auto found = std::lower_bound(
				glyphs.begin(), glyphs.end(), character, [](const FontGlyph &glyph, uint32_t value) {
					return glyph.Character < value;
				}
			);
			if (found == glyphs.end() || found->Character != character) return {};
		}
		return bytes.Bytes;
	}
	Status ValidateSourceFontObservations(
		const SourceFontContext *context,
		std::span<const SourceFontObservation> observations,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status status, std::string message, std::string_view node = {}) {
			diagnostic = {status, std::string(node), "font", std::move(message)};
			return status;
		};
		if (observations.size() > Limits::MaximumNodes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "font observations exceed request bounds");
		uint64_t bytes = 0;
		if (context) {
			const auto retained = SourceFontContextRetainedBytes(*context);
			if (!retained) return fail(Status::InvalidValue, "font namespace observation is malformed");
			if (*retained > maximumBytes)
				return fail(Status::LimitExceeded, "font namespace exceeds request budget");
			bytes = *retained;
		}
		uint64_t keyBytes = 0;
		for (const auto &observation : observations) {
			const auto &request = observation.Request;
			if (request.Authored.Id.size() > Limits::MaximumTextBytes ||
				request.Role.size() > Limits::MaximumTextBytes)
				return fail(
					Status::LimitExceeded, "font observation key exceeds text limits", request.Authored.Id
				);
			keyBytes += request.Authored.Id.size() + request.Role.size() + 1;
		}
		constexpr uint64_t comparisonWork = 16 * 1024 * 1024;
		if (!observations.empty() && keyBytes > comparisonWork / observations.size())
			return fail(Status::LimitExceeded, "font observation key comparison exceeds work bound");
		for (size_t i = 0; i < observations.size(); ++i) {
			const auto &observation = observations[i];
			const auto retained = SourceFontObservationRetainedBytes(observation);
			if (!retained)
				return fail(
					Status::InvalidValue, "font observation is malformed", observation.Request.Authored.Id
				);
			if (*retained > maximumBytes - bytes)
				return fail(
					Status::LimitExceeded,
					"font observations exceed request budget",
					observation.Request.Authored.Id
				);
			bytes += *retained;
			for (size_t j = 0; j < i; ++j)
				if (const auto &prior = observations[j].Request;
					prior.ProcessorRow == observation.Request.ProcessorRow &&
					prior.Tick == observation.Request.Tick &&
					prior.Subframe == observation.Request.Subframe &&
					prior.NegativeFrame == observation.Request.NegativeFrame &&
					prior.Authored.Id == observation.Request.Authored.Id &&
					prior.Role == observation.Request.Role)
					return fail(
						Status::DuplicateId,
						"font observation request is duplicated",
						observation.Request.Authored.Id
					);
		}
		diagnostic = {};
		return Status::Ok;
	}
}
