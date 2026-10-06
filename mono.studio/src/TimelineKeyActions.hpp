#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <new>
#include <numeric>
#include <vector>

namespace studio {
	enum class TimelineKeyAction { Quantize, AlignLeft, AlignCenter, AlignRight, Distribute, Reverse };

	// Source commands transform clocks only. The existing host transaction moves the pinned keys.
	inline bool PrepareTimelineKeyDestinations(
		std::span<const engine::imagegraph::Keyframe> keys,
		TimelineKeyAction action,
		std::vector<engine::imagegraph::FrameTime> &output,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes,
		std::span<const int8_t> axes = {}
	) try {
		using namespace engine::imagegraph;
		const auto fail = [&](Status code, const char *message) {
			error = {code, {}, {}, message};
			return false;
		};
		if ((!axes.empty() && axes.size() != keys.size()) ||
			std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis < -1 || axis > 1; }))
			return fail(Status::InvalidValue, "key action has invalid component selectors");
		if (keys.empty() || keys.size() > Limits::MaximumKeyframes)
			return fail(Status::InvalidValue, "key action has no bounded selection");
		if (uint64_t(keys.size()) * keys.size() > 64'000'000)
			return fail(Status::LimitExceeded, "key action exceeds the work bound");
		if (action < TimelineKeyAction::Quantize || action > TimelineKeyAction::Reverse)
			return fail(Status::InvalidValue, "unknown key action");
		uint64_t held = output.capacity() * sizeof(FrameTime);
		const uint64_t needed = keys.size() * (sizeof(FrameTime) + sizeof(size_t));
		if (held > maximumBytes || needed > maximumBytes - held)
			return fail(Status::LimitExceeded, "key action exceeds the staged payload budget");
		std::vector<FrameTime> candidate;
		std::vector<size_t> order;
		candidate.reserve(keys.size());
		order.resize(keys.size());
		const uint64_t backing = candidate.capacity() * sizeof(FrameTime) + order.capacity() * sizeof(size_t);
		if (backing > maximumBytes - held)
			return fail(Status::LimitExceeded, "key action vector capacity exceeds the payload budget");
		std::iota(order.begin(), order.end(), 0);
		FrameTime first = GetFrameTime(keys.front()), last = first;
		double sum = 0;
		for (const auto &key : keys) {
			const auto clock = GetFrameTime(key);
			if (!ValidFrameTime(clock))
				return fail(Status::InvalidValue, "key action has an invalid authored clock");
			if (CompareFrameTime(clock, first) < 0) first = clock;
			if (CompareFrameTime(clock, last) > 0) last = clock;
			sum += FrameTimeToReal(clock);
			candidate.push_back(clock);
		}
		if (action == TimelineKeyAction::Distribute) {
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				const int order = CompareFrameTime(GetFrameTime(keys[a]), GetFrameTime(keys[b]));
				return order < 0 || (order == 0 && a < b);
			});
			// Native source-action interpolation uses an offset from the first clock.
			// Preserve exact endpoints without snapping intermediate fractional results.
			if (keys.size() > 2)
				for (size_t i = 0; i < order.size(); ++i) {
					if (i == 0)
						candidate[order[i]] = first;
					else if (i + 1 == order.size())
						candidate[order[i]] = last;
					else if (!SplitFrameTime(
								 double(FrameTimeToReal(first)) +
									 (double(FrameTimeToReal(last)) - double(FrameTimeToReal(first))) *
										 (double(i) / double(order.size() - 1)),
								 candidate[order[i]]
							 ))
						return fail(Status::InvalidValue, "distributed key clock exceeds the authored range");
				}
		} else {
			FrameTime center;
			if (action == TimelineKeyAction::AlignCenter &&
				!SplitFrameTime(sum / double(keys.size()), center, true))
				return fail(Status::InvalidValue, "aligned key clock exceeds the authored range");
			for (size_t i = 0; i < keys.size(); ++i) {
				switch (action) {
				case TimelineKeyAction::Quantize:
					if (!SplitFrameTime(double(FrameTimeToReal(candidate[i])), candidate[i], true))
						return fail(Status::InvalidValue, "quantized key clock exceeds the authored range");
					break;
				case TimelineKeyAction::AlignLeft:
					candidate[i] = first;
					break;
				case TimelineKeyAction::AlignCenter:
					candidate[i] = center;
					break;
				case TimelineKeyAction::AlignRight:
					candidate[i] = last;
					break;
				case TimelineKeyAction::Reverse: {
					size_t channelCount = 0;
					for (size_t other = 0; other < keys.size(); ++other)
						if (keys[other].NodeId == keys[i].NodeId && keys[other].Port == keys[i].Port &&
							(axes.empty() || axes[other] == axes[i]))
							++channelCount;
					if (channelCount > 1 &&
						!SplitFrameTime(
							double(FrameTimeToReal(last)) -
								(double(FrameTimeToReal(candidate[i])) - double(FrameTimeToReal(first))),
							candidate[i]
						))
						return fail(Status::InvalidValue, "reversed key clock exceeds the authored range");
					break;
				}
				case TimelineKeyAction::Distribute:
					break;
				}
			}
		}
		output = std::move(candidate);
		error = {};
		return true;
	} catch (const std::bad_alloc &) {
		error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "key action allocation failed"};
		return false;
	}
}
