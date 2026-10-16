#pragma once

#include <engine/procedural/Canonical.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <span>

namespace engine::procedural::detail {

	inline Status Refuse(Diagnostic &diagnostic, Status code, std::string_view field = {}) {
		diagnostic.Code = code;
		diagnostic.Field.assign(field.substr(0, 128));
		return code;
	}

	struct Writer {
		std::vector<uint8_t> Bytes{};
		size_t Maximum = 0;

		bool Reserve(size_t count) const {
			return count <= Maximum && Bytes.size() <= Maximum - count;
		}

		bool U8(uint8_t value) {
			if (!Reserve(1)) return false;
			Bytes.push_back(value);
			return true;
		}
		bool U16(uint16_t value) {
			return U8(static_cast<uint8_t>(value)) && U8(static_cast<uint8_t>(value >> 8));
		}
		bool U32(uint32_t value) {
			for (unsigned shift = 0; shift < 32; shift += 8)
				if (!U8(static_cast<uint8_t>(value >> shift))) return false;
			return true;
		}
		bool U64(uint64_t value) {
			for (unsigned shift = 0; shift < 64; shift += 8)
				if (!U8(static_cast<uint8_t>(value >> shift))) return false;
			return true;
		}
		bool Raw(std::span<const uint8_t> bytes) {
			if (!Reserve(bytes.size())) return false;
			Bytes.insert(Bytes.end(), bytes.begin(), bytes.end());
			return true;
		}
		bool Text(std::string_view text) {
			if (text.size() > std::numeric_limits<uint16_t>::max() || !Reserve(2 + text.size())) return false;
			return U16(static_cast<uint16_t>(text.size())) &&
				   Raw({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
		}
		bool Real(double value) {
			if (!std::isfinite(value)) return false;
			const uint64_t bits = std::bit_cast<uint64_t>(value == 0.0 ? 0.0 : value);
			return U64(bits);
		}
	};
}
