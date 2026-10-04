#pragma once

#include "FontUnicode.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace engine::imagegraph::detail {
#include "FontCaseTables.inc"

	inline bool NativeCaseProperty(uint32_t point, auto &ranges) {
		const auto found = std::lower_bound(
			ranges.begin(), ranges.end(), point, [](const FontCaseRange &range, uint32_t value) {
				return range.Last < value;
			}
		);
		return found != ranges.end() && point >= found->First;
	}
	inline const FontCaseMapping *NativeCaseMapping(uint32_t point, bool upper) {
		const auto lookup = [&](auto &mappings) -> const FontCaseMapping * {
			const auto found = std::lower_bound(
				mappings.begin(), mappings.end(), point, [](const FontCaseMapping &mapping, uint32_t value) {
					return mapping.Character < value;
				}
			);
			return found != mappings.end() && found->Character == point ? &*found : nullptr;
		};
		return upper ? lookup(FontCaseUpper) : lookup(FontCaseLower);
	}
	inline void NativeCaseAppend(std::string &text, uint32_t point) {
		if (point < 0x80)
			text += char(point);
		else if (point < 0x800) {
			text += char(0xc0 | (point >> 6));
			text += char(0x80 | (point & 63));
		} else if (point < 0x10000) {
			text += char(0xe0 | (point >> 12));
			text += char(0x80 | ((point >> 6) & 63));
			text += char(0x80 | (point & 63));
		} else {
			text += char(0xf0 | (point >> 18));
			text += char(0x80 | ((point >> 12) & 63));
			text += char(0x80 | ((point >> 6) & 63));
			text += char(0x80 | (point & 63));
		}
	}
	// Full Unicode 16 default casing; title uses the source's literal-space start rule.
	inline Status NativeFontTextCase(
		std::string_view input,
		uint8_t choice,
		uint64_t maximumBytes,
		std::string &output,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph.font.unicode_case");
		core::Metrics::Count("imagegraph.font.case_input_bytes", input.size());
		if (choice < 1 || choice > 3 || input.size() > Limits::MaximumTextBytes) {
			failure = "native Text case input is malformed";
			return Status::InvalidValue;
		}
		FontScalarCursor cursor{input};
		uint32_t point = 0;
		size_t count = 0;
		while (cursor.Next(point))
			if (++count > Limits::MaximumArrayElements) {
				failure = "native Text case scalar count exceeds bound";
				return Status::LimitExceeded;
			}
		if (cursor.Invalid) {
			failure = "native Text casing requires valid UTF-8";
			return Status::InvalidValue;
		}
		// Logical growth is admitted first; actual backing capacities are reconciled before use.
		const uint64_t fixedBytes = 3 * sizeof(std::vector<uint32_t>) + 64;
		const uint64_t required = uint64_t(count) * 17 + fixedBytes;
		if (required > maximumBytes || output.capacity() >= maximumBytes - required) {
			failure = "native Text case workspace exceeds byte budget";
			return Status::LimitExceeded;
		}
		const uint64_t previousBytes = output.capacity() + 1;
		const auto capacitiesFit =
			[&](size_t pointsCapacity, size_t suffixCapacity, size_t candidateCapacity) {
				uint64_t available = maximumBytes - fixedBytes - previousBytes;
				if (pointsCapacity > available / sizeof(uint32_t)) return false;
				available -= uint64_t(pointsCapacity) * sizeof(uint32_t);
				if (suffixCapacity > available) return false;
				available -= suffixCapacity;
				return !candidateCapacity || candidateCapacity < available;
			};
		std::vector<uint32_t> points;
		points.reserve(count);
		if (!capacitiesFit(points.capacity(), count, 0)) {
			failure = "native Text case workspace exceeds byte budget";
			return Status::LimitExceeded;
		}
		cursor = FontScalarCursor{input};
		while (cursor.Next(point))
			points.push_back(point);
		std::vector<uint8_t> nextCased(count, 0);
		if (!capacitiesFit(points.capacity(), nextCased.capacity(), 0)) {
			failure = "native Text case workspace exceeds byte budget";
			return Status::LimitExceeded;
		}
		bool suffix = false;
		for (size_t i = count; i > 0; --i) {
			nextCased[i - 1] = suffix;
			if (NativeCaseProperty(points[i - 1], FontCaseCased))
				suffix = true;
			else if (!NativeCaseProperty(points[i - 1], FontCaseCaseIgnorable))
				suffix = false;
		}
		// Measure the complete mapping before allocating its backing, including contextual sigma.
		auto mappedPoints = [&](auto emit) {
			bool prefix = false, titleStart = true;
			for (size_t i = 0; i < count; ++i) {
				const auto current = points[i];
				if (choice == 1 && current == 0x3a3 && prefix && !nextCased[i]) {
					if (!emit(0x3c2)) return false;
				} else {
					const auto *mapping =
						choice == 3 && !titleStart ? nullptr : NativeCaseMapping(current, choice != 1);
					if (mapping) {
						for (size_t j = 0; j < mapping->Count; ++j)
							if (!emit(mapping->Output[j])) return false;
					} else if (!emit(current))
						return false;
				}
				titleStart = current == 0x20;
				if (NativeCaseProperty(current, FontCaseCased))
					prefix = true;
				else if (!NativeCaseProperty(current, FontCaseCaseIgnorable))
					prefix = false;
			}
			return true;
		};
		size_t outputPoints = 0, outputBytes = 0;
		if (!mappedPoints([&](uint32_t value) {
				++outputPoints;
				outputBytes += value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
				return outputPoints <= Limits::MaximumArrayElements &&
					   outputBytes <= Limits::MaximumTextBytes;
			})) {
			failure = "native Text case output exceeds scalar or text bound";
			return Status::LimitExceeded;
		}
		const size_t inlineCapacity = std::string{}.capacity();
		const size_t requestedBacking = outputBytes > inlineCapacity ? outputBytes : 0;
		if (!capacitiesFit(points.capacity(), nextCased.capacity(), requestedBacking)) {
			failure = "native Text case workspace exceeds byte budget";
			return Status::LimitExceeded;
		}
		std::string candidate;
		candidate.reserve(outputBytes);
		const size_t retainedBacking = candidate.capacity() > inlineCapacity ? candidate.capacity() : 0;
		if (!capacitiesFit(points.capacity(), nextCased.capacity(), retainedBacking)) {
			failure = "native Text case workspace exceeds byte budget";
			return Status::LimitExceeded;
		}
		mappedPoints([&](uint32_t value) {
			NativeCaseAppend(candidate, value);
			return true;
		});
		core::Metrics::Count("imagegraph.font.case_output_bytes", candidate.size());
		core::Metrics::Count("imagegraph.font.case_candidate_backing_bytes", retainedBacking);
		output = std::move(candidate);
		failure.clear();
		return Status::Ok;
	} catch (...) {
		failure = "native Text case allocation failed";
		return Status::LimitExceeded;
	}
}
