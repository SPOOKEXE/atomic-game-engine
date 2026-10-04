#pragma once
#include <cstdint>
#include <string_view>

namespace engine::imagegraph::detail {
	// No locale or allocation. Invalid UTF8 is never replaced with an invented glyph.
	struct FontScalarCursor {
		std::string_view Bytes;
		bool Invalid = false;
		bool Next(uint32_t &point) {
			if (Invalid || Bytes.empty()) return false;
			const auto first = static_cast<uint8_t>(Bytes.front());
			const size_t size = first < 0x80					 ? 1
								: first >= 0xc2 && first <= 0xdf ? 2
								: first >= 0xe0 && first <= 0xef ? 3
								: first >= 0xf0 && first <= 0xf4 ? 4
																 : 0;
			if (!size || Bytes.size() < size) {
				Invalid = true;
				return false;
			}
			point = first & (size == 1 ? 0x7f : size == 2 ? 0x1f : size == 3 ? 0xf : 7);
			for (size_t index = 1; index < size; ++index) {
				const auto next = static_cast<uint8_t>(Bytes[index]);
				if ((next & 0xc0) != 0x80) {
					Invalid = true;
					return false;
				}
				point = (point << 6) | (next & 0x3f);
			}
			if ((size == 2 && point < 0x80) || (size == 3 && point < 0x800) ||
				(size == 4 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) {
				Invalid = true;
				return false;
			}
			Bytes.remove_prefix(size);
			return true;
		}
	};
	struct FontUtf16Cursor {
		FontScalarCursor Source;
		uint32_t Pending = 0;
		bool Next(uint32_t &unit) {
			if (Pending) {
				unit = Pending;
				Pending = 0;
				return true;
			}
			uint32_t point = 0;
			if (!Source.Next(point)) return false;
			if (point <= 0xffff)
				unit = point;
			else {
				point -= 0x10000;
				unit = 0xd800 + (point >> 10);
				Pending = 0xdc00 + (point & 0x3ff);
			}
			return true;
		}
	};
}
