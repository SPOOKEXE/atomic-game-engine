#pragma once

// Bounded byte-string operations used by source-authored text value nodes.

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

	inline std::optional<std::string> ReplaceText(
		std::string_view text,
		std::string_view find,
		std::string_view replacement,
		bool all,
		size_t maximumBytes
	) {
		if (find.empty() || text.size() > maximumBytes) return std::nullopt;
		std::string result;
		result.reserve(text.size());
		size_t offset = 0;
		while (offset < text.size()) {
			const size_t match = text.find(find, offset);
			if (match == std::string_view::npos) break;
			if (match - offset > maximumBytes - result.size()) return std::nullopt;
			result.append(text.substr(offset, match - offset));
			if (replacement.size() > maximumBytes - result.size()) return std::nullopt;
			result.append(replacement);
			offset = match + find.size();
			if (!all) break;
		}
		if (text.size() - offset > maximumBytes - result.size()) return std::nullopt;
		result.append(text.substr(offset));
		return result;
	}

	inline std::optional<std::string> CombineText(std::span<const std::string> pieces, size_t maximumBytes) {
		std::string result;
		for (const std::string &piece : pieces) {
			if (piece.size() > maximumBytes - result.size()) return std::nullopt;
			result += piece;
		}
		return result;
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
