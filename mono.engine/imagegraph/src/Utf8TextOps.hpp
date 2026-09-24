#pragma once

// Bounded text operations for source-authored string nodes.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace engine::imagegraph::detail {
	enum class TextOpStatus : uint8_t { Ok, InvalidUtf8, InvalidMode, UnsupportedIndex, LimitExceeded };

	inline bool NextUtf8(std::string_view text, size_t &offset) {
		if (offset >= text.size()) return false;
		const uint8_t first = static_cast<uint8_t>(text[offset]);
		if (first < 0x80) {
			offset++;
			return true;
		}
		const size_t width = first >= 0xC2 && first <= 0xDF	  ? 2
							 : first >= 0xE0 && first <= 0xEF ? 3
							 : first >= 0xF0 && first <= 0xF4 ? 4
															  : 0;
		if (width == 0 || width > text.size() - offset) return false;
		uint32_t scalar = first & (width == 2 ? 0x1F : width == 3 ? 0x0F : 0x07);
		for (size_t index = 1; index < width; index++) {
			const uint8_t byte = static_cast<uint8_t>(text[offset + index]);
			if ((byte & 0xC0) != 0x80) return false;
			scalar = (scalar << 6) | (byte & 0x3F);
		}
		if ((width == 2 && scalar < 0x80) || (width == 3 && scalar < 0x800) ||
			(width == 4 && scalar < 0x10000) || (scalar >= 0xD800 && scalar <= 0xDFFF) || scalar > 0x10FFFF)
			return false;
		offset += width;
		return true;
	}

	inline TextOpStatus CountText(std::string_view text, size_t &characters) {
		characters = 0;
		if (text.size() > Limits::MaximumTextBytes) return TextOpStatus::LimitExceeded;
		for (size_t offset = 0; offset < text.size();) {
			if (!NextUtf8(text, offset)) return TextOpStatus::InvalidUtf8;
			characters++;
		}
		return TextOpStatus::Ok;
	}

	inline size_t ByteOffset(std::string_view text, size_t characterOffset) {
		size_t offset = 0;
		for (size_t index = 0; index < characterOffset; index++)
			NextUtf8(text, offset);
		return offset;
	}

	inline TextOpStatus TextLength(std::string_view text, int64_t mode, int64_t &length) {
		size_t characters = 0;
		const TextOpStatus status = CountText(text, characters);
		if (status != TextOpStatus::Ok) return status;
		if (mode == 0) {
			length = static_cast<int64_t>(characters);
			return TextOpStatus::Ok;
		}
		if (mode != 1) return TextOpStatus::InvalidMode;
		// Pinned string_splice keeps empty segments, including one for empty input.
		length = 1 + static_cast<int64_t>(std::count(text.begin(), text.end(), ' '));
		return TextOpStatus::Ok;
	}

	inline TextOpStatus CopyText(std::string_view text, int64_t index, int64_t amount, std::string &output) {
		output.clear();
		size_t characters = 0;
		const TextOpStatus status = CountText(text, characters);
		if (status != TextOpStatus::Ok) return status;
		if (index <= 0 || amount < 0) return TextOpStatus::UnsupportedIndex;
		if (amount == 0 || static_cast<uint64_t>(index) > characters) return TextOpStatus::Ok;
		const size_t start = static_cast<size_t>(index - 1);
		const size_t available = characters - start;
		const size_t count =
			static_cast<uint64_t>(amount) > available ? available : static_cast<size_t>(amount);
		const size_t firstByte = ByteOffset(text, start);
		const size_t lastByte = ByteOffset(text, start + count);
		output.assign(text.substr(firstByte, lastByte - firstByte));
		return TextOpStatus::Ok;
	}

	inline TextOpStatus
	DeleteText(std::string_view text, int64_t index, int64_t amount, std::string &output) {
		output.clear();
		size_t characters = 0;
		const TextOpStatus status = CountText(text, characters);
		if (status != TextOpStatus::Ok) return status;
		if (index == std::numeric_limits<int64_t>::max()) return TextOpStatus::UnsupportedIndex;
		const int64_t sourceIndex = index + 1;
		if (sourceIndex == 0) return TextOpStatus::UnsupportedIndex;
		if (characters == 0 || amount == 0) {
			output = text;
			return TextOpStatus::Ok;
		}
		size_t position = characters;
		if (sourceIndex > 0) {
			if (static_cast<uint64_t>(sourceIndex) <= characters)
				position = static_cast<size_t>(sourceIndex - 1);
		} else {
			// Rearranged subtraction also handles INT64_MIN without overflow.
			const uint64_t fromEnd = static_cast<uint64_t>(-(sourceIndex + 1));
			if (fromEnd < characters) position = characters - 1 - static_cast<size_t>(fromEnd);
		}
		if (position == characters) return TextOpStatus::UnsupportedIndex;
		size_t first = position;
		size_t last = position + 1;
		if (amount > 0) {
			last = static_cast<uint64_t>(amount) >= characters - position
					   ? characters
					   : position + static_cast<size_t>(amount);
		} else {
			const uint64_t preceding = static_cast<uint64_t>(-(amount + 1));
			first = preceding >= position ? 0 : position - static_cast<size_t>(preceding);
		}
		const size_t firstByte = ByteOffset(text, first);
		const size_t lastByte = ByteOffset(text, last);
		output.assign(text.substr(0, firstByte));
		output.append(text.substr(lastByte));
		return TextOpStatus::Ok;
	}
}
