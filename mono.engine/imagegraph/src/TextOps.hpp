#pragma once

// Bounded byte-string operations used by source-authored text value nodes.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph::detail {
	inline size_t CountText(std::string_view text, std::string_view find) {
		if (find.empty()) return 0;
		size_t count = 0;
		for (size_t offset = 0; (offset = text.find(find, offset)) != std::string_view::npos;
			 offset += find.size())
			count++;
		return count;
	}

	inline std::optional<size_t> ReplacementTextSize(
		std::string_view text,
		std::string_view find,
		std::string_view replacement,
		bool all,
		size_t maximumBytes
	) {
		if (find.empty() || text.size() > maximumBytes) return std::nullopt;
		size_t resultBytes = text.size();
		for (size_t offset = 0; (offset = text.find(find, offset)) != std::string_view::npos;
			 offset += find.size()) {
			if (replacement.size() >= find.size()) {
				const size_t growth = replacement.size() - find.size();
				if (growth > maximumBytes - resultBytes) return std::nullopt;
				resultBytes += growth;
			} else
				resultBytes -= find.size() - replacement.size();
			if (!all) break;
		}
		return resultBytes;
	}

	inline std::optional<std::string> CombineText(std::span<const std::string> pieces, size_t maximumBytes) {
		std::string result;
		for (const std::string &piece : pieces) {
			if (piece.size() > maximumBytes - result.size()) return std::nullopt;
			result += piece;
		}
		return result;
	}

	// The caller pre-admits expectedBytes as retained output storage. Fill a sized string so the builder
	// never grows its capacity while copying replacement segments.
	inline std::optional<std::string> BuildReplacementText(
		std::string_view text,
		std::string_view find,
		std::string_view replacement,
		bool all,
		size_t maximumBytes,
		size_t expectedBytes
	) {
		const auto exactBytes = ReplacementTextSize(text, find, replacement, all, maximumBytes);
		if (!exactBytes || *exactBytes != expectedBytes) return std::nullopt;
		std::string result(expectedBytes, '\0');
		size_t destination = 0;
		const auto copy = [&](std::string_view value) {
			if (value.size() > result.size() - destination) return false;
			std::copy(value.begin(), value.end(), result.begin() + destination);
			destination += value.size();
			return true;
		};
		size_t offset = 0;
		while (offset < text.size()) {
			const size_t match = text.find(find, offset);
			if (match == std::string_view::npos) break;
			if (!copy(text.substr(offset, match - offset)) || !copy(replacement)) return std::nullopt;
			offset = match + find.size();
			if (!all) break;
		}
		if (!copy(text.substr(offset)) || destination != result.size()) return std::nullopt;
		return result;
	}

	inline std::optional<std::string> ReplaceText(
		std::string_view text,
		std::string_view find,
		std::string_view replacement,
		bool all,
		size_t maximumBytes,
		size_t expectedBytes
	) {
		return BuildReplacementText(text, find, replacement, all, maximumBytes, expectedBytes);
	}

	inline std::optional<std::string> ReplaceText(
		std::string_view text,
		std::string_view find,
		std::string_view replacement,
		bool all,
		size_t maximumBytes
	) {
		const auto expectedBytes = ReplacementTextSize(text, find, replacement, all, maximumBytes);
		if (!expectedBytes) return std::nullopt;
		return BuildReplacementText(text, find, replacement, all, maximumBytes, *expectedBytes);
	}

	inline std::optional<std::vector<std::string>>
	SplitText(std::string_view text, std::string_view delimiter, size_t maximumElements) {
		if (delimiter.empty() || maximumElements == 0) return std::nullopt;
		std::vector<std::string> result;
		size_t offset = 0;
		while (true) {
			if (result.size() == maximumElements) return std::nullopt;
			const size_t match = text.find(delimiter, offset);
			result.emplace_back(
				text.substr(offset, match == std::string_view::npos ? match : match - offset)
			);
			if (match == std::string_view::npos) return result;
			offset = match + delimiter.size();
		}
	}
}
