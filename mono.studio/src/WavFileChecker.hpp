#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace studio::detail {
	struct WavFileCheckerState {
		int64_t EditSecond = 0;
		std::array<uint64_t, 2> DueFrames{};
		size_t Pending = 0;
		std::optional<uint64_t> LastHostFrame;
		bool operator==(const WavFileCheckerState &) const = default;
	};
	// The source schedules a distinct callback two host frames after each newer edit.
	// Pending callbacks remain due after disabling the checker; timeline seeks do not drive this clock.
	inline engine::imagegraph::Status AdvanceWavFileChecker(
		WavFileCheckerState &state,
		uint64_t hostFrame,
		bool enabled,
		std::optional<int64_t> modifiedSecond,
		size_t &reloads,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		const auto fail = [&](const char *message, Status status) {
			diagnostic = {status, {}, {}, message};
			return status;
		};
		if (state.Pending > state.DueFrames.size())
			return fail("WAV checker pending callbacks exceed their bound", Status::LimitExceeded);
		if (state.LastHostFrame && hostFrame < *state.LastHostFrame)
			return fail("WAV checker host clock cannot rewind", Status::InvalidValue);
		if (state.LastHostFrame && hostFrame == *state.LastHostFrame) {
			reloads = 0;
			diagnostic = {};
			return Status::Ok;
		}
		WavFileCheckerState candidate = state;
		size_t due = 0, pending = 0;
		for (size_t index = 0; index < state.Pending; ++index) {
			if (state.DueFrames[index] <= hostFrame)
				++due;
			else
				candidate.DueFrames[pending++] = state.DueFrames[index];
		}
		if (enabled && modifiedSecond && *modifiedSecond > state.EditSecond) {
			if (hostFrame > std::numeric_limits<uint64_t>::max() - 2 || pending == state.DueFrames.size())
				return fail("WAV checker scheduling exceeds its bound", Status::LimitExceeded);
			candidate.DueFrames[pending++] = hostFrame + 2;
			candidate.EditSecond = *modifiedSecond;
		}
		std::fill(candidate.DueFrames.begin() + pending, candidate.DueFrames.end(), uint64_t{0});
		candidate.Pending = pending;
		candidate.LastHostFrame = hostFrame;
		state = candidate;
		reloads = due;
		diagnostic = {};
		return Status::Ok;
	}
}
