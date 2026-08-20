#include <algorithm>
#include <cctype>
#include <string_view>
#include <studio/CodeMetrics.hpp>

namespace studio {

	namespace {

		// The same identifier rule `Complete.cpp` scans with, restated because
		// that copy is deliberately private to its file. The two answer the
		// same question for different features and a drift between them shows
		// up as a minimap stripe ending mid-word, which is cosmetic.
		bool IsWordCharacter(const char character) {
			const auto value = static_cast<unsigned char>(character);
			return std::isalnum(value) != 0 || character == '_' || character == '$';
		}

		// Where a 0-based line begins, or `npos` when the file has no such
		// line.
		size_t LineStart(const std::string_view text, const size_t line) {
			size_t start = 0;
			for (size_t remaining = line; remaining > 0; remaining--) {
				const size_t newline = text.find('\n', start);
				if (newline == std::string_view::npos) {
					return std::string_view::npos;
				}
				start = newline + 1;
			}
			return start;
		}

	}

	size_t ColumnAt(const std::string_view text, size_t offset) {
		offset = std::min(offset, text.size());

		const size_t newline = text.rfind('\n', offset == 0 ? 0 : offset - 1);
		const size_t start = (newline == std::string_view::npos || offset == 0) ? 0 : newline + 1;

		size_t column = 0;
		for (size_t at = start; at < offset; at++) {
			column += text[at] == '\t' ? TAB_COLUMNS : 1;
		}
		return column;
	}

	size_t OffsetAtCell(const std::string_view text, const size_t line, const size_t column) {
		const size_t start = LineStart(text, line);
		if (start == std::string_view::npos) {
			return std::string_view::npos;
		}

		size_t rendered = 0;
		for (size_t at = start; at < text.size() && text[at] != '\n'; at++) {
			const size_t width = text[at] == '\t' ? TAB_COLUMNS : 1;
			if (column < rendered + width) {
				return at;
			}
			rendered += width;
		}

		// Past the line's last character - including column 0 of an empty
		// line - is empty space, and empty space is nothing.
		return std::string_view::npos;
	}

	void MinimapRunsOf(const std::string_view line, const size_t maxColumns, std::vector<MinimapRun> &into) {
		into.clear();

		size_t column = 0;
		for (size_t at = 0; at < line.size() && column < maxColumns; at++) {
			const char character = line[at];
			const size_t width = character == '\t' ? TAB_COLUMNS : 1;

			if (character == ' ' || character == '\t' || character == '\r') {
				column += width;
				continue;
			}

			const bool word = IsWordCharacter(character);
			if (!into.empty() && into.back().Word == word &&
				into.back().Column + into.back().Columns == column) {
				into.back().Columns += width;
			} else {
				into.push_back({column, width, word});
			}
			column += width;
		}

		// The cut, applied once at the end rather than per character.
		if (!into.empty() && into.back().Column + into.back().Columns > maxColumns) {
			into.back().Columns = maxColumns - into.back().Column;
		}
	}

	float MinimapRowHeight(const size_t lines, const float mapHeight, const float preferred) {
		if (lines == 0 || mapHeight <= 0.0f || preferred <= 0.0f) {
			return std::max(preferred, 0.001f);
		}
		return std::max(std::min(preferred, mapHeight / static_cast<float>(lines)), 0.001f);
	}

	float MinimapScrollFor(
		const float pickedLine, const size_t lines, const float rowHeight, const float viewHeight
	) {
		const float content = static_cast<float>(lines) * rowHeight;
		const float centred = (pickedLine * rowHeight) - (viewHeight * 0.5f);
		return std::clamp(centred, 0.0f, std::max(content - viewHeight, 0.0f));
	}

}
