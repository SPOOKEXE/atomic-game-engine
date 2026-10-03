#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace studio::detail {
	// Missing source attributes retain the verified WAV constructor default.
	inline bool ReadWavFileCheckerEnabled(
		const engine::imagegraph::Node &node, bool &enabled, engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		if (node.Type != "pc.wav_file_read" ||
			node.SourceProperties.size() > Limits::MaximumPropertiesPerNode) {
			diagnostic = {
				Status::InvalidValue, node.Id, "file_checker", "File Watcher requires a bounded WAV Read node"
			};
			return false;
		}
		bool value = true, found = false;
		for (const auto &property : node.SourceProperties)
			if (property.Port == "file_checker") {
				const auto *flag = std::get_if<bool>(&property.Data);
				if (!flag || found) {
					diagnostic = {
						Status::InvalidValue,
						node.Id,
						"file_checker",
						"WAV checker requires one boolean source property"
					};
					return false;
				}
				found = true;
				value = *flag;
			}
		enabled = value;
		diagnostic = {};
		return true;
	}
	inline bool SetWavFileCheckerEnabled(
		engine::imagegraph::Node &node, bool enabled, engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		bool current = false;
		if (!ReadWavFileCheckerEnabled(node, current, diagnostic)) return false;
		for (auto &property : node.SourceProperties)
			if (property.Port == "file_checker") {
				property.Data = enabled;
				diagnostic = {};
				return true;
			}
		if (enabled) {
			diagnostic = {};
			return true;
		}
		if (node.SourceProperties.size() == Limits::MaximumPropertiesPerNode) {
			diagnostic = {
				Status::LimitExceeded, node.Id, "file_checker", "WAV source property count exceeds its bound"
			};
			return false;
		}
		node.SourceProperties.push_back({"file_checker", false});
		diagnostic = {};
		return true;
	}
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
