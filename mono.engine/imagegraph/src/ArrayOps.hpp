#pragma once

// Bounded authored array shape and deterministic array-processor index schedules.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace engine::imagegraph::detail {
	enum class ArrayProcessMode : uint8_t {
		Loop,
		Hold,
		Expand,
		ExpandInverse,
	};

	// Spread appends one input array's children; without Spread it appends the array itself.
	template <class T>
	Status CollectArray(
		std::span<const ArrayItem<T>> inputs,
		bool spread,
		size_t maximumItems,
		std::vector<ArrayItem<T>> &output
	) {
		if (maximumItems == 0 || maximumItems > Limits::MaximumArrayElements) return Status::LimitExceeded;
		std::vector<const ArrayItem<T> *> selected;
		for (const ArrayItem<T> &input : inputs) {
			if (spread) {
				if (const auto *children = std::get_if<std::vector<ArrayItem<T>>>(&input.Data)) {
					if (children->size() > maximumItems - selected.size()) return Status::LimitExceeded;
					for (const ArrayItem<T> &child : *children)
						selected.push_back(&child);
					continue;
				}
			}
			if (selected.size() == maximumItems) return Status::LimitExceeded;
			selected.push_back(&input);
		}

		// Bound the full nested shape, not only the visible top-level count.
		std::vector<std::pair<const ArrayItem<T> *, size_t>> pending;
		for (const ArrayItem<T> *item : selected)
			pending.emplace_back(item, 1);
		size_t itemCount = 0;
		while (!pending.empty()) {
			const auto [item, depth] = pending.back();
			pending.pop_back();
			if (itemCount == maximumItems) return Status::LimitExceeded;
			++itemCount;
			if (const auto *children = std::get_if<std::vector<ArrayItem<T>>>(&item->Data)) {
				if (depth >= 16 && !children->empty()) return Status::LimitExceeded;
				if (pending.size() > maximumItems - itemCount ||
					children->size() > maximumItems - itemCount - pending.size())
					return Status::LimitExceeded;
				for (const ArrayItem<T> &child : *children)
					pending.emplace_back(&child, depth + 1);
			}
		}
		std::vector<ArrayItem<T>> candidate;
		candidate.reserve(selected.size());
		for (const ArrayItem<T> *item : selected)
			candidate.push_back(*item);
		output = std::move(candidate);
		return Status::Ok;
	}

	// Each output row gives the selected element index for every input array.
	struct ArrayScheduleFootprint {
		size_t Rows = 0;
		uint64_t RetainedBytes = 0;
		uint64_t PeakBytes = 0;
	};
	inline Status MeasureArraySchedule(
		std::span<const size_t> lengths,
		ArrayProcessMode mode,
		size_t maximumOutputs,
		ArrayScheduleFootprint &footprint
	) {
		if (lengths.empty() || lengths.size() > Limits::MaximumDynamicInputsPerNode || maximumOutputs == 0 ||
			maximumOutputs > Limits::MaximumArrayElements)
			return Status::LimitExceeded;
		if (mode != ArrayProcessMode::Loop && mode != ArrayProcessMode::Hold &&
			mode != ArrayProcessMode::Expand && mode != ArrayProcessMode::ExpandInverse)
			return Status::InvalidValue;
		size_t count = 1;
		for (size_t length : lengths) {
			if (!length) return Status::InvalidValue;
			if (length > Limits::MaximumArrayElements) return Status::LimitExceeded;
			if (mode == ArrayProcessMode::Loop || mode == ArrayProcessMode::Hold)
				count = std::max(count, length);
			else {
				if (length > maximumOutputs / count) return Status::LimitExceeded;
				count *= length;
			}
		}
		if (count > maximumOutputs) return Status::LimitExceeded;
		const uint64_t retained =
			uint64_t(count) * (sizeof(std::vector<size_t>) + lengths.size() * sizeof(size_t));
		// Prefix/suffix tables and the row initializer overlap the allocated result.
		footprint = {count, retained, retained + 3 * lengths.size() * sizeof(size_t)};
		return Status::Ok;
	}

	inline Status BuildArraySchedule(
		std::span<const size_t> lengths,
		ArrayProcessMode mode,
		size_t maximumOutputs,
		std::vector<std::vector<size_t>> &output,
		uint64_t maximumScheduleBytes = Limits::MaximumEvaluationBytes
	) {
		ArrayScheduleFootprint footprint;
		const Status measured = MeasureArraySchedule(lengths, mode, maximumOutputs, footprint);
		if (measured != Status::Ok) return measured;
		if (footprint.PeakBytes > maximumScheduleBytes) return Status::LimitExceeded;
		const size_t outputCount = footprint.Rows;

		std::vector<size_t> suffix(lengths.size(), 1);
		std::vector<size_t> prefix(lengths.size(), 1);
		if (mode == ArrayProcessMode::Expand || mode == ArrayProcessMode::ExpandInverse) {
			for (size_t index = lengths.size(); index > 1; index--)
				suffix[index - 2] = suffix[index - 1] * lengths[index - 1];
			for (size_t index = 1; index < lengths.size(); index++)
				prefix[index] = prefix[index - 1] * lengths[index - 1];
		}

		std::vector<std::vector<size_t>> candidate(outputCount, std::vector<size_t>(lengths.size()));
		for (size_t row = 0; row < outputCount; row++) {
			for (size_t input = 0; input < lengths.size(); input++) {
				switch (mode) {
				case ArrayProcessMode::Loop:
					candidate[row][input] = row % lengths[input];
					break;
				case ArrayProcessMode::Hold:
					candidate[row][input] = std::min(row, lengths[input] - 1);
					break;
				case ArrayProcessMode::Expand:
					candidate[row][input] = (row / suffix[input]) % lengths[input];
					break;
				case ArrayProcessMode::ExpandInverse:
					// Documentation promises all combinations in reverse order. The pinned
					// source reverses suffix indices, which repeats pairs for unequal lengths.
					candidate[row][input] = (row / prefix[input]) % lengths[input];
					break;
				}
			}
		}
		output = std::move(candidate);
		return Status::Ok;
	}
	// Source inverse reverses the full suffix table, including singleton source input slots.
	// BuildArraySchedule retains the native Cartesian inverse contract.
	inline Status BuildSourceArraySchedule(
		std::span<const size_t> lengths,
		ArrayProcessMode mode,
		size_t maximumOutputs,
		std::vector<std::vector<size_t>> &output,
		uint64_t maximumScheduleBytes = Limits::MaximumEvaluationBytes
	) {
		if (mode != ArrayProcessMode::ExpandInverse)
			return BuildArraySchedule(lengths, mode, maximumOutputs, output, maximumScheduleBytes);
		std::vector<std::vector<size_t>> candidate;
		const Status status = BuildArraySchedule(
			lengths, ArrayProcessMode::Expand, maximumOutputs, candidate, maximumScheduleBytes
		);
		if (status != Status::Ok) return status;
		std::vector<size_t> suffix(lengths.size(), 1);
		for (size_t index = lengths.size(); index > 1; index--)
			suffix[index - 2] = suffix[index - 1] * lengths[index - 1];
		for (size_t row = 0; row < candidate.size(); row++)
			for (size_t input = 0; input < lengths.size(); input++)
				candidate[row][input] = (row / suffix[lengths.size() - 1 - input]) % lengths[input];
		output = std::move(candidate);
		return Status::Ok;
	}

}
