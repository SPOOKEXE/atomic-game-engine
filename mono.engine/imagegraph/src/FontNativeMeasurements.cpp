#include "FontNativeMeasurements.hpp"

#include "FontPayload.hpp"
#include "FontTextLayout.hpp"
#include "FontUnicode.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace {
		struct NativeMeasurementUnit {
			uint32_t Code = 0;
			double Advance = 0;
		};
		bool NativeMeasurementAdd(uint64_t &total, uint64_t count, uint64_t unit, uint64_t maximum) {
			if (total > maximum || (unit && count > (maximum - total) / unit)) return false;
			total += count * unit;
			return true;
		}
		bool NativeMeasurementIdentity(const FontMeasurement &a, const FontMeasurement &b) {
			return a.MaximumLineWidth == b.MaximumLineWidth && a.LineGap == b.LineGap && a.Text == b.Text;
		}
		bool NativeMeasurementControl(const FontMeasurement &measurement, bool request) {
			return measurement.Text.size() <= Limits::MaximumTextBytes &&
				   std::isfinite(measurement.MaximumLineWidth) && std::isfinite(measurement.LineGap) &&
				   std::isfinite(measurement.Width) && measurement.Width >= 0 &&
				   std::isfinite(measurement.Height) && measurement.Height >= 0 &&
				   (!request || (measurement.Width == 0 && measurement.Height == 0));
		}
	}
	Status MeasureNativeFontData(
		const FontData &font,
		std::span<const FontMeasurement> requests,
		uint64_t maximumBytes,
		uint64_t maximumWork,
		std::vector<FontMeasurement> &output,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph.font.measure_native");
		const auto fail = [&](Status status, const char *message) {
			failure = message;
			return status;
		};
		if (requests.size() > MaximumFontGlyphs || output.size() > MaximumFontGlyphs ||
			font.Glyphs.size() > MaximumFontGlyphs || font.Frames.size() > MaximumFontGlyphs ||
			font.Measurements.size() > MaximumFontGlyphs)
			return fail(Status::LimitExceeded, "native font measurement count exceeds bound");
		const auto fontBytes = FontStorageBytes(font, true);
		uint64_t fixedBytes = sizeof(FontValue) + 3 * sizeof(std::vector<FontMeasurement>);
		uint64_t requestText = 0, recordedText = 0, maximumUnits = 0;
		uint64_t work = 0;
		if (fontBytes > Limits::MaximumArrayBytes ||
			!NativeMeasurementAdd(fixedBytes, fontBytes, 1, maximumBytes) ||
			!NativeMeasurementAdd(fixedBytes, requests.size(), sizeof(FontMeasurement), maximumBytes) ||
			!NativeMeasurementAdd(fixedBytes, output.capacity(), sizeof(FontMeasurement), maximumBytes))
			return fail(Status::LimitExceeded, "native font measurement inputs exceed byte budget");
		for (const auto &prior : output) {
			if (!NativeMeasurementControl(prior, false))
				return fail(Status::InvalidValue, "prior native font measurements are malformed");
			if (!NativeMeasurementAdd(fixedBytes, prior.Text.capacity(), 1, maximumBytes))
				return fail(Status::LimitExceeded, "prior font measurement backing exceeds byte budget");
		}
		for (const auto &record : font.Measurements)
			if (!NativeMeasurementAdd(recordedText, record.Text.size(), 1, maximumBytes))
				return fail(Status::LimitExceeded, "font measurement records exceed byte budget");
		for (const auto &request : requests) {
			if (!NativeMeasurementControl(request, true))
				return fail(Status::InvalidValue, "native font measurement request is malformed");
			if (!NativeMeasurementAdd(requestText, request.Text.size(), 1, maximumBytes) ||
				!NativeMeasurementAdd(fixedBytes, request.Text.capacity(), 1, maximumBytes))
				return fail(Status::LimitExceeded, "native font measurement request text exceeds budget");
		}
		// Quotes bound all decode/wrap/advance scans and worst-case identity text comparisons.
		for (const auto &request : requests) {
			const auto quote = NativeSourceFontMeasurementWork(request.Text, request.MaximumLineWidth);
			if (!quote || !NativeMeasurementAdd(work, *quote, 1, maximumWork))
				return fail(Status::LimitExceeded, "native font measurement work exceeds bound");
		}
		const uint64_t earlierRequests = requests.empty() ? 0 : requests.size() - 1;
		if (!NativeMeasurementAdd(work, requests.size(), earlierRequests, maximumWork) ||
			!NativeMeasurementAdd(work, requestText, earlierRequests, maximumWork) ||
			!NativeMeasurementAdd(work, requests.size(), font.Measurements.size(), maximumWork) ||
			!NativeMeasurementAdd(work, requestText, font.Measurements.size(), maximumWork) ||
			!NativeMeasurementAdd(work, recordedText, requests.size(), maximumWork))
			return fail(Status::LimitExceeded, "native font measurement work exceeds bound");
		if (!ValidFontData(font))
			return fail(Status::InvalidValue, "native font measurement payload is malformed");
		for (size_t index = 0; index < requests.size(); ++index) {
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (NativeMeasurementIdentity(requests[index], requests[earlier]))
					return fail(Status::DuplicateId, "native font measurement request is duplicated");
			FontScalarCursor cursor{requests[index].Text};
			uint32_t point = 0;
			uint64_t scalars = 0, units = 0;
			while (cursor.Next(point)) {
				if (++scalars > Limits::MaximumArrayElements)
					return fail(
						Status::LimitExceeded, "native font measurement text exceeds character bound"
					);
				units += font.Characters == FontCharacterProfile::Utf16 && point > 0xffff ? 2 : 1;
			}
			if (cursor.Invalid)
				return fail(Status::InvalidValue, "native font measurement text is not valid Unicode");
			maximumUnits = std::max(maximumUnits, units);
		}
		uint64_t candidateBytes = sizeof(std::vector<FontMeasurement>);
		if (!NativeMeasurementAdd(candidateBytes, requests.size(), sizeof(FontMeasurement), maximumBytes))
			return fail(Status::LimitExceeded, "native measurement result table exceeds budget");
		for (const auto &request : requests)
			if (!NativeMeasurementAdd(
					candidateBytes, std::max(request.Text.size(), std::string{}.capacity()), 1, maximumBytes
				))
				return fail(Status::LimitExceeded, "native measurement result text exceeds budget");
		// The file host publishes measurements into the same owned font. Admit its
		// replacement payload before allocating, as well as the operation coexistence.
		uint64_t replacementFontBytes = 0;
		const bool outputInsideFont = &output == &font.Measurements;
		if (outputInsideFont) {
			uint64_t oldMeasurements = uint64_t(font.Measurements.capacity()) * sizeof(FontMeasurement);
			for (const auto &measurement : font.Measurements)
				if (!NativeMeasurementAdd(oldMeasurements, measurement.Text.capacity(), 1, fontBytes))
					return fail(Status::InvalidValue, "font measurement backing is inconsistent");
			if (oldMeasurements > fontBytes)
				return fail(Status::InvalidValue, "font measurement backing is inconsistent");
			replacementFontBytes = fontBytes - oldMeasurements;
			if (candidateBytes > Limits::MaximumArrayBytes - replacementFontBytes)
				return fail(
					Status::LimitExceeded, "native measurement replacement exceeds font payload bound"
				);
		}
		uint64_t workspace = sizeof(std::vector<NativeMeasurementUnit>);
		if (!NativeMeasurementAdd(workspace, maximumUnits, sizeof(NativeMeasurementUnit), maximumBytes) ||
			!NativeMeasurementAdd(fixedBytes, workspace, 1, maximumBytes) ||
			!NativeMeasurementAdd(fixedBytes, candidateBytes, 1, maximumBytes))
			return fail(Status::LimitExceeded, "native font measurement replacement exceeds byte budget");
		std::vector<NativeMeasurementUnit> units;
		std::vector<FontMeasurement> candidate;
		units.reserve(static_cast<size_t>(maximumUnits));
		candidate.reserve(requests.size());
		const uint64_t actualWorkspace = sizeof(units) + units.capacity() * sizeof(NativeMeasurementUnit);
		uint64_t actualCandidate = sizeof(candidate) + candidate.capacity() * sizeof(FontMeasurement);
		if (actualWorkspace > workspace || actualCandidate > candidateBytes)
			return fail(Status::LimitExceeded, "native measurement capacities exceed admitted workspace");
		for (const auto &request : requests) {
			const FontMeasurement *recorded = nullptr;
			for (const auto &record : font.Measurements)
				if (NativeMeasurementIdentity(record, request)) {
					if (recorded) return fail(Status::DuplicateId, "font measurement record is duplicated");
					recorded = &record;
				}
			if (recorded) {
				candidate.push_back(*recorded);
			} else {
				if (font.Raster == FontRasterProfile::SourceObserved)
					return fail(
						Status::UnsupportedExecution,
						"source font measurement requires its exact observed record"
					);
				if (request.MaximumLineWidth < std::numeric_limits<int32_t>::min() ||
					request.MaximumLineWidth > std::numeric_limits<int32_t>::max() ||
					request.LineGap < std::numeric_limits<int32_t>::min() ||
					request.LineGap > std::numeric_limits<int32_t>::max())
					return fail(
						Status::UnsupportedExecution,
						"native font measurement int32 conversion is outside represented range"
					);
				const int32_t width = static_cast<int32_t>(request.MaximumLineWidth);
				const int32_t separation = static_cast<int32_t>(request.LineGap);
				units.clear();
				const auto addUnit = [&](uint32_t point) {
					const auto *glyph = FontGlyphForUnit(font, point);
					if (!font.GlyphMapComplete && point != 10 && point != 13 && !glyph) return false;
					const bool specialSpace =
						point == 32 && (!font.HasCharacterRange || point < font.FirstCharacter ||
										point > font.LastCharacter);
					units.push_back(
						{point,
						 glyph			? glyph->Advance
						 : specialSpace ? font.SpaceAdvance
										: font.MissingAdvance}
					);
					return true;
				};
				FontScalarCursor scalar{request.Text};
				FontUtf16Cursor utf16{{request.Text}};
				uint32_t point = 0;
				while (font.Characters == FontCharacterProfile::Utf16 ? utf16.Next(point)
																	  : scalar.Next(point))
					if (!addUnit(point))
						return fail(
							Status::UnsupportedExecution,
							"native font measurement needs unobserved glyph advances"
						);
				double measuredWidth = 0;
				size_t lines = 0, start = 0, end = 0;
				const size_t length = units.size();
				const bool noWrap = width < 0 || width == 10000000;
				uint32_t last = length ? units.front().Code : 0;
				const auto code = [&](size_t at) { return at < length ? units[at].Code : uint32_t{0}; };
				const auto emit = [&](size_t begin, size_t finish) {
					// JavaScript substring swaps endpoints after the source's space trim.
					if (finish < begin) std::swap(begin, finish);
					double lineWidth = 0;
					for (size_t at = begin; at < finish; ++at)
						lineWidth += units[at].Advance;
					measuredWidth = std::max(measuredWidth, lineWidth);
					++lines;
					return std::isfinite(lineWidth) && lines <= Limits::MaximumArrayElements;
				};
				while (start < length) {
					if (noWrap) {
						while (end < length && code(end) != 10 && code(end) != 13) {
							++end;
							last = code(end);
						}
						// Preserve the inspected sentinel branch's literal CR/LF conditions.
						if ((last == 10 && code(end) == 13) || (last == 13 && code(end) == 10)) {
							++end;
							continue;
						}
						last = code(end);
						if (!emit(start, end))
							return fail(
								Status::LimitExceeded, "native font measurement line bounds exceeded"
							);
					} else {
						double total = 0;
						while (end < length && total < width && code(end) == 32) {
							total += units[end].Advance;
							++end;
						}
						while (end < length && total < width && code(end) != 10) {
							total += units[end].Advance;
							++end;
						}
						if (!std::isfinite(total))
							return fail(
								Status::LimitExceeded, "native font wrap accumulation exceeds finite range"
							);
						if (total > width) --end;
						if (code(end) == 10) {
							if (!emit(start, end))
								return fail(
									Status::LimitExceeded, "native font measurement line bounds exceeded"
								);
						} else {
							if (end == start) break;
							if (end != length && code(end) != 32) {
								size_t beforeWord = end;
								while (beforeWord > start && code(--beforeWord) != 32) {}
								if (beforeWord != start)
									end = beforeWord;
								else
									while (end < length && code(end) != 32)
										++end;
							}
							size_t finish = end;
							while (finish > 0 && code(finish - 1) == 32)
								--finish;
							if (finish != start && !emit(start, finish))
								return fail(
									Status::LimitExceeded, "native font measurement line bounds exceeded"
								);
						}
					}
					start = ++end;
				}
				const double lineStep = separation < 0 ? font.LineHeight : double(separation);
				const double measuredHeight = lines ? double(lines - 1) * lineStep + font.LineHeight : 0;
				if (!std::isfinite(measuredWidth) || !std::isfinite(measuredHeight) || measuredHeight < 0)
					return fail(Status::LimitExceeded, "native font measured dimensions exceed finite range");
				candidate.push_back(
					{request.Text, request.MaximumLineWidth, request.LineGap, measuredWidth, measuredHeight}
				);
			}
			if (!NativeMeasurementAdd(actualCandidate, candidate.back().Text.capacity(), 1, candidateBytes))
				return fail(
					Status::LimitExceeded, "native font measurement result capacities exceed admission"
				);
		}
		core::Metrics::Count("imagegraph.font.measure_work_units", work);
		core::Metrics::Count("imagegraph.font.measure_workspace_bytes", actualWorkspace);
		core::Metrics::Count("imagegraph.font.measure_candidate_bytes", actualCandidate);
		output = std::move(candidate);
		failure.clear();
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		failure = "native font measurement allocation failed";
		return Status::LimitExceeded;
	} catch (const std::length_error &) {
		failure = "native font measurement allocation length exceeds bound";
		return Status::LimitExceeded;
	}
}
namespace engine::imagegraph {
	std::optional<uint64_t> NativeSourceFontMeasurementWork(std::string_view text, double maximumLineWidth) {
		if (text.size() > Limits::MaximumTextBytes || !std::isfinite(maximumLineWidth)) return {};
		uint64_t maximumSpaces = 0, run = 0;
		if (maximumLineWidth >= 1 && !(maximumLineWidth >= 10000000 && maximumLineWidth < 10000001))
			for (const auto byte : text) {
				run = byte == ' ' ? run + 1 : 0;
				maximumSpaces = std::max(maximumSpaces, run);
			}
		const uint64_t perByte = NativeFontMeasurementWorkPerByte + 2 * maximumSpaces;
		if (text.size() > (std::numeric_limits<uint64_t>::max() - NativeFontMeasurementWorkPerByte) / perByte)
			return {};
		return uint64_t(text.size()) * perByte + NativeFontMeasurementWorkPerByte;
	}
	Status MeasureNativeSourceFont(
		const FontValue &font,
		std::span<const FontMeasurement> requests,
		uint64_t maximumOperationBytes,
		uint64_t maximumWorkUnits,
		std::vector<FontMeasurement> &output,
		std::string &failure
	) {
		if (!font.Data) {
			failure = "native font measurement payload is empty";
			return Status::InvalidValue;
		}
		return detail::MeasureNativeFontData(
			*font.Data, requests, maximumOperationBytes, maximumWorkUnits, output, failure
		);
	}
}
