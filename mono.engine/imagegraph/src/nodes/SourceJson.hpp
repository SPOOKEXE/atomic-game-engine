#pragma once

// Bounded native JSON syntax with owned source values and an explicit undefined/null carrier.
#include "../Utf8TextOps.hpp"
#include "../ValuePayload.hpp"
#include "ArraySource.hpp"

#include <charconv>

namespace engine::imagegraph::detail {
	struct JsonCursor {
		std::string_view Input;
		size_t Offset = 0, Nodes = 0;
		bool Limit = false;
		void Space() {
			while (Offset < Input.size() && (Input[Offset] == ' ' || Input[Offset] == '\n' ||
											 Input[Offset] == '\r' || Input[Offset] == '\t'))
				++Offset;
		}
		bool Take(char c) {
			Space();
			if (Offset < Input.size() && Input[Offset] == c) {
				++Offset;
				return true;
			}
			return false;
		}
		bool Literal(std::string_view text) {
			if (Input.substr(Offset).starts_with(text)) {
				Offset += text.size();
				return true;
			}
			return false;
		}
		bool Hex(uint32_t &value) {
			value = 0;
			for (int i = 0; i < 4; ++i) {
				if (Offset == Input.size()) return false;
				const char c = Input[Offset++];
				int d = c >= '0' && c <= '9'   ? c - '0'
						: c >= 'a' && c <= 'f' ? c - 'a' + 10
						: c >= 'A' && c <= 'F' ? c - 'A' + 10
											   : -1;
				if (d < 0) return false;
				value = value * 16 + uint32_t(d);
			}
			return true;
		}
		static void Utf8(std::string &text, uint32_t c) {
			if (c < 128)
				text += char(c);
			else if (c < 2048) {
				text += char(0xc0 | (c >> 6));
				text += char(0x80 | (c & 63));
			} else if (c < 65536) {
				text += char(0xe0 | (c >> 12));
				text += char(0x80 | ((c >> 6) & 63));
				text += char(0x80 | (c & 63));
			} else {
				text += char(0xf0 | (c >> 18));
				text += char(0x80 | ((c >> 12) & 63));
				text += char(0x80 | ((c >> 6) & 63));
				text += char(0x80 | (c & 63));
			}
		}
		bool String(std::string &text) {
			if (!Take('"')) return false;
			while (Offset < Input.size()) {
				char c = Input[Offset++];
				if (c == '"') {
					size_t count = 0;
					return CountText(text, count) == TextOpStatus::Ok;
				}
				if (uint8_t(c) < 32) return false;
				if (c != '\\') {
					text += c;
					continue;
				}
				if (Offset == Input.size()) return false;
				c = Input[Offset++];
				switch (c) {
				case '"':
				case '\\':
				case '/':
					text += c;
					break;
				case 'b':
					text += '\b';
					break;
				case 'f':
					text += '\f';
					break;
				case 'n':
					text += '\n';
					break;
				case 'r':
					text += '\r';
					break;
				case 't':
					text += '\t';
					break;
				case 'u': {
					uint32_t scalar = 0;
					if (!Hex(scalar)) return false;
					if (scalar >= 0xd800 && scalar <= 0xdbff) {
						if (!Literal("\\u")) return false;
						uint32_t low = 0;
						if (!Hex(low) || low < 0xdc00 || low > 0xdfff) return false;
						scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00);
					} else if (scalar >= 0xdc00 && scalar <= 0xdfff)
						return false;
					Utf8(text, scalar);
					break;
				}
				default:
					return false;
				}
			}
			return false;
		}
		bool Parse(Value &value, size_t depth = 1) {
			Space();
			if (depth > Limits::MaximumArrayDepth || ++Nodes > Limits::MaximumArrayElements) {
				Limit = true;
				return false;
			}
			if (Offset == Input.size()) return false;
			const char c = Input[Offset];
			if (c == '{') {
				++Offset;
				StructValue result;
				auto &fields = result.Data.emplace().Fields;
				if (Take('}')) {
					value = std::move(result);
					return true;
				}
				while (true) {
					std::string key;
					if (!String(key) || !Take(':')) return false;
					Value member;
					if (!Parse(member, depth + 1)) return false;
					auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &field) {
						return field.first == key;
					});
					if (found == fields.end())
						fields.emplace_back(std::move(key), std::move(member));
					else
						found->second = std::move(member);
					if (Take('}')) {
						value = std::move(result);
						return true;
					}
					if (!Take(',')) return false;
				}
			}
			if (c == '[') {
				++Offset;
				ArrayValue array{ValueType::Any, {}};
				if (Take(']')) {
					value = std::move(array);
					return true;
				}
				while (true) {
					Value member;
					if (!Parse(member, depth + 1)) return false;
					if (auto *child = std::get_if<ArrayValue>(&member))
						array.Items.push_back({source_array::FromValues(*child)});
					else {
						auto leaf = ArrayElement(std::move(member));
						if (!leaf) return false;
						array.Items.push_back({std::move(*leaf)});
					}
					if (Take(']')) {
						value = std::move(array);
						return true;
					}
					if (!Take(',')) return false;
				}
			}
			if (c == '"') {
				std::string text;
				if (!String(text)) return false;
				value = std::move(text);
				return true;
			}
			if (Literal("true")) {
				value = true;
				return true;
			}
			if (Literal("false")) {
				value = false;
				return true;
			}
			if (Literal("null")) {
				value = UndefinedValue{};
				return true;
			}
			const size_t start = Offset;
			if (Input[Offset] == '-') ++Offset;
			if (Offset == Input.size()) return false;
			if (Input[Offset] == '0')
				++Offset;
			else {
				if (Input[Offset] < '1' || Input[Offset] > '9') return false;
				while (Offset < Input.size() && Input[Offset] >= '0' && Input[Offset] <= '9')
					++Offset;
			}
			if (Offset < Input.size() && Input[Offset] == '.') {
				++Offset;
				const size_t digits = Offset;
				while (Offset < Input.size() && Input[Offset] >= '0' && Input[Offset] <= '9')
					++Offset;
				if (digits == Offset) return false;
			}
			if (Offset < Input.size() && (Input[Offset] == 'e' || Input[Offset] == 'E')) {
				++Offset;
				if (Offset < Input.size() && (Input[Offset] == '+' || Input[Offset] == '-')) ++Offset;
				const size_t digits = Offset;
				while (Offset < Input.size() && Input[Offset] >= '0' && Input[Offset] <= '9')
					++Offset;
				if (digits == Offset) return false;
			}
			double number = 0;
			const auto parsed = std::from_chars(Input.data() + start, Input.data() + Offset, number);
			if (parsed.ec != std::errc{} || parsed.ptr != Input.data() + Offset || !std::isfinite(number))
				return false;
			value = number;
			return true;
		}
	};
}
