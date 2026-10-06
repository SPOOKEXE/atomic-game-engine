#pragma once

#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <algorithm>
#include <new>

namespace studio {
	// Pins come from the document or bounded key capture. The core transaction owns alias resolution.
	template <class Select, class Change>
	bool EditCapturedImageGraphKeys(
		engine::imagegraph::Document &document,
		std::span<const engine::imagegraph::Keyframe> originals,
		const Select &select,
		const Change &change,
		engine::imagegraph::Diagnostic &error,
		uint64_t extraPerKey = 0,
		uint64_t borrowedExtraBytes = 0,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
		std::span<const int8_t> axes = {}
	) try {
		using namespace engine::imagegraph;
		const auto fail = [&] {
			error = {Status::LimitExceeded, {}, {}, "captured key draft exceeds the payload budget"};
			return false;
		};
		if ((!axes.empty() && axes.size() != originals.size()) ||
			std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis < -1 || axis > 1; })) {
			error = {Status::InvalidValue, {}, {}, "captured metadata component selectors are invalid"};
			return false;
		}
		const auto resident = DocumentRetainedPayloadBytes(document);
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (borrowedExtraBytes > maximumBytes) return fail();
		maximumBytes -= borrowedExtraBytes;
		if (!resident || *resident > maximumBytes || originals.size() > Limits::MaximumKeyframes)
			return fail();
		uint64_t remaining = maximumBytes - *resident;
		size_t count = 0;
		for (size_t index = 0; index < originals.size(); ++index) {
			if (!select(originals[index], index)) continue;
			const auto bytes = KeyframePayloadBytes(originals[index]);
			if (!bytes || *bytes > remaining / 2) return fail();
			remaining -= 2 * *bytes;
			if (extraPerKey > remaining || sizeof(SourceKeyframeEdit) > remaining - extraPerKey)
				return fail();
			remaining -= extraPerKey + sizeof(SourceKeyframeEdit);
			++count;
		}
		if (!count) {
			error = {Status::InvalidValue, {}, {}, "captured key edit has no targets"};
			return false;
		}
		std::vector<Keyframe> replacements;
		std::vector<SourceKeyframeEdit> edits;
		replacements.reserve(count);
		edits.reserve(count);
		for (size_t index = 0; index < originals.size(); ++index) {
			const auto &original = originals[index];
			if (!select(original, index)) continue;
			replacements.push_back(original);
			change(replacements.back(), index);
			edits.push_back(
				{&original, &replacements.back(), false, axes.empty() ? int8_t{-1} : axes[index]}
			);
		}
		const uint64_t spareSlots = (replacements.capacity() - replacements.size()) * sizeof(Keyframe) +
									(edits.capacity() - edits.size()) * sizeof(SourceKeyframeEdit);
		if (spareSlots > maximumBytes) return fail();
		return ApplySourceKeyframeEdits(document, edits, document, error, maximumBytes - spareSlots) ==
			   Status::Ok;
	} catch (const std::bad_alloc &) {
		error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "captured key draft allocation failed"};
		return false;
	}
}
