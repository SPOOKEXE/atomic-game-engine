#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <tuple>

namespace engine::imagegraphio::detail {
	inline constexpr std::string_view CompactSourceKeyId = "pxc:compact";
	struct SourceKeyIdText {
		std::array<char, imagegraph::Limits::MaximumSourceKeyIdBytes> Bytes{};
		size_t Size = 0;
		std::string_view View() const {
			return {Bytes.data(), Size};
		}
	};
	// The retained archive fixes the original record occurrence. Node and port scope the identity;
	// timeline edits keep this name rather than rediscovering a record from its edited position.
	inline std::optional<SourceKeyIdText>
	SourceKeyId(const imagegraph::FrameTime &time, imagegraph::KeyframeKind kind, size_t occurrence) {
		SourceKeyIdText result;
		char *next = result.Bytes.data(), *end = next + result.Bytes.size();
		const auto number = [&](auto value) {
			if (next == end) return false;
			*next++ = ':';
			const auto encoded = std::to_chars(next, end, value);
			if (encoded.ec != std::errc{}) return false;
			next = encoded.ptr;
			return true;
		};
		*next++ = 'p';
		*next++ = 'x';
		*next++ = 'c';
		if (!number(uint8_t(kind)) || !number(time.Tick) || !number(time.NegativeFrame ? 1 : 0) ||
			!number(time.Subframe) || !number(occurrence))
			return std::nullopt;
		result.Size = size_t(next - result.Bytes.data());
		return result;
	}
	inline void CanonicalizeKeyOrder(imagegraph::Document &comparison) {
		std::sort(comparison.Keyframes.begin(), comparison.Keyframes.end(), [](const auto &a, const auto &b) {
			return std::tie(a.NodeId, a.Port, a.NegativeFrame, a.Tick, a.Subframe) <
				   std::tie(b.NodeId, b.Port, b.NegativeFrame, b.Tick, b.Subframe);
		});
	}

	// Rebase only the comparison copy after the writer has emitted and reimported these exact
	// key addresses. It never selects an old opaque source record or changes the caller's document.
	inline void RebaseKeyProvenance(imagegraph::Document &comparison, const imagegraph::Document &saved) {
		if (saved.Keyframes.size() > imagegraph::Limits::MaximumKeyframes) return;
		const auto before = [](const auto &a, const auto &b) {
			return std::tie(a.NodeId, a.Port, a.NegativeFrame, a.Tick, a.Subframe, a.Kind) <
				   std::tie(b.NodeId, b.Port, b.NegativeFrame, b.Tick, b.Subframe, b.Kind);
		};
		std::vector<const imagegraph::Keyframe *> index;
		index.reserve(saved.Keyframes.size());
		for (const auto &key : saved.Keyframes)
			index.push_back(&key);
		std::sort(index.begin(), index.end(), [&](const auto *a, const auto *b) { return before(*a, *b); });
		for (auto &key : comparison.Keyframes) {
			const auto found = std::lower_bound(
				index.begin(), index.end(), key, [&](const auto *candidate, const auto &target) {
					return before(*candidate, target);
				}
			);
			if (found != index.end() && !before(key, **found)) key.SourceKeyId = (*found)->SourceKeyId;
		}
	}
}
