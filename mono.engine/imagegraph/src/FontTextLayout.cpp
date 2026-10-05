#include "FontTextLayout.hpp"

#include "FontNativeMeasurements.hpp"
#include "FontPayload.hpp"
#include "FontUnicode.hpp"
#include "SourceGradient.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	const FontGlyph *FontGlyphForUnit(const FontData &font, uint32_t character) {
		const auto found = std::lower_bound(
			font.Glyphs.begin(), font.Glyphs.end(), character, [](const FontGlyph &glyph, uint32_t value) {
				return glyph.Character < value;
			}
		);
		return found == font.Glyphs.end() || found->Character != character ? nullptr : &*found;
	}
	double FontUnitAdvance(const FontData &font, uint32_t character) {
		if (const auto *glyph = FontGlyphForUnit(font, character)) return glyph->Advance;
		if (character == 32 &&
			(!font.HasCharacterRange || character < font.FirstCharacter || character > font.LastCharacter))
			return font.SpaceAdvance;
		return font.MissingAdvance;
	}
	double FontScalarAdvance(const FontData &font, uint32_t character) {
		if (font.Characters == FontCharacterProfile::UnicodeScalar || character <= 0xffff)
			return FontUnitAdvance(font, character);
		character -= 0x10000;
		return FontUnitAdvance(font, 0xd800 + (character >> 10)) +
			   FontUnitAdvance(font, 0xdc00 + (character & 1023));
	}
	std::optional<uint64_t> FontTextNativeMeasurementAdmissionBytes(const FontData &font, size_t textBytes) {
		if (textBytes > Limits::MaximumTextBytes || font.Glyphs.size() > MaximumFontGlyphs ||
			font.Frames.size() > MaximumFontGlyphs || font.Measurements.size() > MaximumFontGlyphs)
			return {};
		const auto fontBytes = FontStorageBytes(font, true);
		if (fontBytes > Limits::MaximumArrayBytes) return {};
		// Unit workspace is at most one 16-byte record per UTF8 byte. The remaining
		// allowance covers request/result string copies, table headers and SSO.
		return fontBytes + uint64_t(textBytes) * 24 + 512;
	}
	Status BuildFontTextLayout(
		const FontData &font,
		std::string_view text,
		const FontTextLayoutOptions &options,
		uint64_t maximumBytes,
		FontTextLayout &output,
		std::string &failure
	) try {
		const auto fail = [&](Status code, const char *message) {
			failure = message;
			return code;
		};
		if (text.size() > Limits::MaximumTextBytes || options.ChangeCase > 3 || options.TrimType > 2 ||
			!std::isfinite(options.Tracking) || !std::isfinite(options.LineGap) ||
			!std::isfinite(options.MaximumLineWidth) || !std::isfinite(options.Range.X) ||
			!std::isfinite(options.Range.Y))
			return fail(Status::InvalidValue, "text layout controls are malformed");
		FontScalarCursor cursor{text};
		size_t characters = 0;
		uint32_t point = 0;
		while (cursor.Next(point)) {
			if (++characters > Limits::MaximumArrayElements)
				return fail(Status::LimitExceeded, "text layout exceeds character bound");
			if (options.ChangeCase && point > 127 && !options.ObservedCasedText)
				return fail(
					Status::UnsupportedExecution,
					"non-ASCII source casing requires recorded Unicode transformation"
				);
		}
		if (cursor.Invalid) return fail(Status::UnsupportedExecution, "text layout requires valid Unicode");

		if (options.ObservedCasedText) {
			if (!options.ChangeCase || options.ObservedCasedText->size() > Limits::MaximumTextBytes)
				return fail(Status::InvalidValue, "recorded text casing is malformed");
			cursor = FontScalarCursor{*options.ObservedCasedText};
			characters = 0;
			while (cursor.Next(point))
				if (++characters > Limits::MaximumArrayElements)
					return fail(Status::LimitExceeded, "recorded text casing exceeds character bound");
			if (cursor.Invalid)
				return fail(Status::InvalidValue, "recorded text casing is not valid Unicode");
		}

		const size_t maximumLines = std::min(Limits::MaximumArrayElements, characters * 2 + 1);
		uint64_t admitted =
			sizeof(FontTextLayout) +
			4 * (text.size() + (options.ObservedCasedText ? options.ObservedCasedText->size() : 0) +
				 characters + 32) +
			maximumLines * (sizeof(FontTextLine) + 32);
		// Word/line trimming retains the source delimiter tokens. Admit its index before reserve.
		const uint64_t trimWorkspace =
			options.Trim && options.TrimType
				? sizeof(std::vector<std::string_view>) + (characters + 1) * sizeof(std::string_view)
				: 0;
		if (trimWorkspace > maximumBytes || admitted > maximumBytes - trimWorkspace)
			return fail(Status::LimitExceeded, "text trim token workspace exceeds byte budget");
		admitted += trimWorkspace;
		uint64_t nativeMeasurementBytes = 0;
		if (options.FullTextSize && options.MaximumLineWidth != 0) {
			const auto extra = FontTextNativeMeasurementAdmissionBytes(
				font, options.ObservedCasedText ? options.ObservedCasedText->size() : text.size()
			);
			if (!extra || *extra > maximumBytes || admitted > maximumBytes - *extra)
				return fail(
					Status::LimitExceeded, "native full-text measurement staging exceeds byte budget"
				);
			nativeMeasurementBytes = *extra;
			admitted += *extra;
		}
		if (admitted > maximumBytes)
			return fail(Status::LimitExceeded, "text layout staging exceeds byte budget");
		FontTextLayout candidate;
		candidate.RawText.assign(options.ObservedCasedText ? *options.ObservedCasedText : text);
		if (options.ChangeCase && !options.ObservedCasedText) {
			bool titleStart = true;
			for (char &character : candidate.RawText) {
				const char original = character;
				if (options.ChangeCase == 1 && character >= 'A' && character <= 'Z') character += 'a' - 'A';
				if ((options.ChangeCase == 2 || (options.ChangeCase == 3 && titleStart)) &&
					character >= 'a' && character <= 'z')
					character -= 'a' - 'A';
				titleStart = original == ' ';
			}
		}
		if (!font.GlyphMapComplete) {
			const auto known = [&](uint32_t character) {
				if (font.Characters == FontCharacterProfile::UnicodeScalar || character <= 0xffff)
					return FontGlyphForUnit(font, character) != nullptr;
				character -= 0x10000;
				return FontGlyphForUnit(font, 0xd800 + (character >> 10)) &&
					   FontGlyphForUnit(font, 0xdc00 + (character & 1023));
			};
			cursor = FontScalarCursor{candidate.RawText};
			while (cursor.Next(point))
				if (point != 10 && !known(point))
					return fail(
						Status::UnsupportedExecution, "retained font has no observation for a requested glyph"
					);
			if (options.Monospaced && !known('W'))
				return fail(Status::UnsupportedExecution, "monospaced font W advance is unobserved");
			if (options.MaximumLineWidth != 0 && !options.SplitWord && !known(' '))
				return fail(Status::UnsupportedExecution, "word wrap space advance is unobserved");
		}
		candidate.Text = candidate.RawText;
		if (options.Trim) {
			ENGINE_PROFILE("imagegraph.font.trim");
			const auto inputBytes = candidate.Text.size();
			const auto delimiter = [&](char c) { return c == '\n' || (options.TrimType == 1 && c == ' '); };
			size_t count = characters;
			std::vector<std::string_view> tokens;
			if (options.TrimType) {
				tokens.reserve(characters + 1);
				if (sizeof(tokens) + tokens.capacity() * sizeof(std::string_view) > trimWorkspace)
					return fail(Status::LimitExceeded, "text trim token capacity exceeds admission");
				size_t start = 0;
				for (size_t byte = 0; byte < candidate.Text.size(); ++byte)
					if (delimiter(candidate.Text[byte])) {
						tokens.emplace_back(std::string_view(candidate.Text).substr(start, byte + 1 - start));
						start = byte + 1;
					}
				tokens.emplace_back(std::string_view(candidate.Text).substr(start));
				count = tokens.size();
			}
			const double first = SourceRoundEven(options.Range.X * count);
			const double last = SourceRoundEven(options.Range.Y * count);
			const double length = last - first;
			// Keep the source signed indices; reject unrepresentable int32 arguments before conversion.
			constexpr double minimum = std::numeric_limits<int32_t>::min();
			constexpr double maximum = std::numeric_limits<int32_t>::max();
			if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(length) || first < minimum ||
				first >= maximum || last < minimum || last > maximum || length < minimum || length > maximum)
				return fail(Status::LimitExceeded, "text trim source indices exceed int32 bounds");
			const int64_t begin = static_cast<int64_t>(first), amount = static_cast<int64_t>(length);
			if (!options.TrimType) {
				// string_copy clamps its one-based start, then substring clamps and swaps its endpoints.
				const int64_t start = std::max<int64_t>(begin, 0);
				const auto clamp = [&](int64_t value) { return std::clamp<int64_t>(value, 0, count); };
				const size_t low = static_cast<size_t>(std::min(clamp(start), clamp(start + amount)));
				const size_t high = static_cast<size_t>(std::max(clamp(start), clamp(start + amount)));
				size_t offset = 0, byteBegin = candidate.Text.size(), byteEnd = candidate.Text.size(),
					   index = 0;
				cursor = FontScalarCursor{candidate.Text};
				while (!cursor.Bytes.empty()) {
					if (index == low) byteBegin = offset;
					if (index == high) {
						byteEnd = offset;
						break;
					}
					const size_t before = cursor.Bytes.size();
					(void)cursor.Next(point);
					offset += before - cursor.Bytes.size();
					++index;
				}
				candidate.Text = candidate.Text.substr(byteBegin, byteEnd - byteBegin);
			} else {
				// computeIterationValues indexes negative offsets from the end and walks negative lengths
				// backward.
				int64_t offset = std::clamp<int64_t>(begin, -int64_t(count), int64_t(count) - 1);
				if (offset < 0) offset += count;
				const int64_t loops = amount < 0 ? std::min(offset + 1, -amount)
												 : std::min<int64_t>(offset + amount, count) - offset;
				std::string trimmed;
				trimmed.reserve(candidate.Text.size());
				if (trimmed.capacity() > candidate.Text.capacity())
					return fail(
						Status::LimitExceeded, "text trim result capacity exceeds admitted source storage"
					);
				for (int64_t iteration = 0; iteration < loops; ++iteration) {
					trimmed.append(tokens[static_cast<size_t>(offset)]);
					offset += amount < 0 ? -1 : 1;
				}
				candidate.Text = std::move(trimmed);
			}
			core::Metrics::Count("imagegraph.font.trim_input_bytes", inputBytes);
			core::Metrics::Count("imagegraph.font.trim_output_bytes", candidate.Text.size());
			core::Metrics::Count(
				"imagegraph.font.trim_workspace_bytes",
				options.TrimType ? sizeof(tokens) + tokens.capacity() * sizeof(std::string_view) : 0
			);
			core::Metrics::Count("imagegraph.font.trim_tokens", count);
		}

		candidate.MonoWidth = FontScalarAdvance(font, 'W');
		candidate.Lines.reserve(maximumLines);
		const auto measure = [&](std::string_view value, size_t &count) {
			double width = 0;
			FontScalarCursor reader{value};
			count = 0;
			while (reader.Next(point)) {
				++count;
				width += FontScalarAdvance(font, point);
			}
			return width;
		};
		const auto emit = [&](std::string_view line, double width) -> bool {
			if (candidate.Lines.size() == Limits::MaximumArrayElements) return false;
			candidate.Lines.push_back({std::string(line), width, line.empty() ? 0 : font.LineHeight});
			return std::isfinite(width);
		};
		size_t start = 0;
		while (start <= candidate.Text.size()) {
			const size_t newline = candidate.Text.find('\n', start);
			const auto line =
				std::string_view(candidate.Text)
					.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
			if (options.MaximumLineWidth == 0) {
				size_t count = 0;
				double width = measure(line, count);
				if (options.Monospaced) width = candidate.MonoWidth * count;
				width += options.Tracking * (static_cast<double>(count) - 1);
				if (!emit(line, width))
					return fail(Status::LimitExceeded, "text line width or count exceeds bounds");
			} else {
				std::string pending;
				pending.reserve(line.size() + characters);
				double width = 0;
				size_t wordStart = 0;
				cursor = FontScalarCursor{line};
				while (!cursor.Bytes.empty() || (!options.SplitWord && wordStart <= line.size())) {
					std::string_view token;
					size_t count = 0;
					if (options.SplitWord) {
						const auto before = cursor.Bytes;
						(void)cursor.Next(point);
						token = before.substr(0, before.size() - cursor.Bytes.size());
					} else {
						const size_t space = line.find(' ', wordStart);
						token = line.substr(
							wordStart,
							space == std::string_view::npos ? std::string_view::npos : space - wordStart
						);
						wordStart = space == std::string_view::npos ? line.size() + 1 : space + 1;
						cursor.Bytes = {};
					}
					double next = measure(token, count);
					if (options.Monospaced) next = candidate.MonoWidth * count;
					next += options.SplitWord ? options.Tracking : options.Tracking * count - 1;
					if (width + next >= options.MaximumLineWidth) {
						if (!emit(pending, width - options.Tracking))
							return fail(Status::LimitExceeded, "text wrap exceeds line bounds");
						pending.clear();
						width = 0;
					}
					pending.append(token);
					if (!options.SplitWord) pending.push_back(' ');
					width += next;
					if (!options.SplitWord)
						width += options.Monospaced ? candidate.MonoWidth : FontScalarAdvance(font, ' ');
				}
				if (!pending.empty() && !emit(pending, width - options.Tracking))
					return fail(Status::LimitExceeded, "text wrap exceeds line bounds");
			}
			if (newline == std::string::npos) break;
			start = newline + 1;
		}
		for (const auto &line : candidate.Lines) {
			candidate.Width = std::max(candidate.Width, line.Width);
			candidate.Height += line.Height;
			if (&line != &candidate.Lines.front()) candidate.Height += options.LineGap;
			size_t count = 0;
			(void)measure(line.Text, count);
			candidate.CharacterCount += count;
		}
		if (options.FullTextSize) {
			const auto found = std::find_if(
				font.Measurements.begin(), font.Measurements.end(), [&](const auto &measurement) {
					return measurement.Text == candidate.RawText &&
						   measurement.MaximumLineWidth == options.MaximumLineWidth &&
						   measurement.LineGap == -1;
				}
			);
			if (found != font.Measurements.end()) {
				candidate.Width = found->Width;
				candidate.Height = found->Height;
			} else if (options.MaximumLineWidth != 0) {
				FontMeasurement request{candidate.RawText, options.MaximumLineWidth, -1, 0, 0};
				std::vector<FontMeasurement> measured;
				const auto status = MeasureNativeFontData(
					font, {&request, 1}, nativeMeasurementBytes, 16u * 1024u * 1024u, measured, failure
				);
				if (status != Status::Ok) return status;
				candidate.Width = measured.front().Width;
				candidate.Height = measured.front().Height;
			} else {
				candidate.Width = 0;
				candidate.Height = 0;
				size_t offset = 0;
				while (offset <= candidate.RawText.size()) {
					const size_t newline = candidate.RawText.find('\n', offset);
					const auto line =
						std::string_view(candidate.RawText)
							.substr(
								offset, newline == std::string::npos ? std::string::npos : newline - offset
							);
					size_t count = 0;
					const double width = measure(line, count);
					candidate.Width =
						std::max(candidate.Width, options.Monospaced ? candidate.MonoWidth * count : width);
					candidate.Height += font.LineHeight;
					if (newline == std::string::npos) break;
					offset = newline + 1;
				}
				if (options.Monospaced) candidate.Width = candidate.MonoWidth * characters;
				if (candidate.RawText.empty()) candidate.Height = 0;
			}
		}
		if (!std::isfinite(candidate.Width) || !std::isfinite(candidate.Height) ||
			candidate.CharacterCount > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "text layout dimensions or draw count exceed bounds");
		uint64_t retained = sizeof(candidate) + candidate.RawText.capacity() + candidate.Text.capacity() +
							candidate.Lines.capacity() * sizeof(FontTextLine);
		for (const auto &line : candidate.Lines)
			retained += line.Text.capacity();
		if (retained > admitted)
			return fail(Status::LimitExceeded, "text layout capacities exceed admitted staging");
		output = std::move(candidate);
		failure.clear();
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		failure = "text layout allocation failed";
		return Status::LimitExceeded;
	} catch (const std::length_error &) {
		failure = "text layout allocation length exceeds limits";
		return Status::LimitExceeded;
	}
}
