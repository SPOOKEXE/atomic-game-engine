#include "DatasetValue.hpp"

#include <engine/script/Codec.hpp>

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace studio {
	namespace {
		using engine::script::ScriptValue;
		using engine::script::ValueTag;
		using nlohmann::json;

		constexpr size_t MAXIMUM_DOCUMENT_BYTES = engine::script::CODEC_MAX_BYTES * 4u;
		constexpr char HEX[] = "0123456789abcdef";

		bool ValidUtf8(const std::string_view text) {
			size_t at = 0;
			while (at < text.size()) {
				const uint8_t first = static_cast<uint8_t>(text[at++]);
				if (first < 0x80) {
					continue;
				}
				size_t continuation = 0;
				uint32_t codepoint = 0;
				uint32_t minimum = 0;
				if ((first & 0xe0u) == 0xc0u) {
					continuation = 1;
					codepoint = first & 0x1fu;
					minimum = 0x80;
				} else if ((first & 0xf0u) == 0xe0u) {
					continuation = 2;
					codepoint = first & 0x0fu;
					minimum = 0x800;
				} else if ((first & 0xf8u) == 0xf0u) {
					continuation = 3;
					codepoint = first & 0x07u;
					minimum = 0x10000;
				} else {
					return false;
				}
				if (at + continuation > text.size()) {
					return false;
				}
				for (size_t index = 0; index < continuation; index++) {
					const uint8_t next = static_cast<uint8_t>(text[at++]);
					if ((next & 0xc0u) != 0x80u) {
						return false;
					}
					codepoint = (codepoint << 6u) | (next & 0x3fu);
				}
				if (codepoint < minimum || codepoint > 0x10ffffu ||
					(codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
					return false;
				}
			}
			return true;
		}

		std::string Hex(const std::string_view text) {
			std::string encoded(text.size() * 2, '0');
			for (size_t index = 0; index < text.size(); index++) {
				const uint8_t byte = static_cast<uint8_t>(text[index]);
				encoded[index * 2] = HEX[byte >> 4u];
				encoded[index * 2 + 1] = HEX[byte & 0x0fu];
			}
			return encoded;
		}

		json TextJson(const std::string_view text) {
			return ValidUtf8(text) ? json{{"encoding", "utf8"}, {"value", text}}
								   : json{{"encoding", "hex"}, {"value", Hex(text)}};
		}

		int Nibble(const char digit) {
			if (digit >= '0' && digit <= '9') {
				return digit - '0';
			}
			if (digit >= 'a' && digit <= 'f') {
				return digit - 'a' + 10;
			}
			if (digit >= 'A' && digit <= 'F') {
				return digit - 'A' + 10;
			}
			return -1;
		}

		bool TextFromJson(const json &document, std::string &out) {
			if (!document.is_object()) {
				return false;
			}
			const auto encoding = document.find("encoding");
			const auto value = document.find("value");
			if (encoding == document.end() || !encoding->is_string() || value == document.end() ||
				!value->is_string()) {
				return false;
			}
			const std::string encoded = value->get<std::string>();
			if (*encoding == "utf8") {
				if (!ValidUtf8(encoded)) {
					return false;
				}
				out = encoded;
				return true;
			}
			if (*encoding != "hex" || encoded.size() % 2 != 0) {
				return false;
			}
			out.resize(encoded.size() / 2);
			for (size_t index = 0; index < out.size(); index++) {
				const int high = Nibble(encoded[index * 2]);
				const int low = Nibble(encoded[index * 2 + 1]);
				if (high < 0 || low < 0) {
					out.clear();
					return false;
				}
				out[index] = static_cast<char>((high << 4) | low);
			}
			return true;
		}

		json ToJson(const ScriptValue &value) {
			switch (value.Tag) {
			case ValueTag::Nil:
				return {{"type", "nil"}};
			case ValueTag::False:
			case ValueTag::True:
				return {{"type", "boolean"}, {"value", value.Tag == ValueTag::True}};
			case ValueTag::Number:
				return {{"type", "number"}, {"value", value.Number}};
			case ValueTag::String:
				return {{"type", "string"}, {"value", TextJson(value.Text)}};
			case ValueTag::Array: {
				json items = json::array();
				for (const ScriptValue &item : value.Items) {
					items.push_back(ToJson(item));
				}
				return {{"type", "array"}, {"value", std::move(items)}};
			}
			case ValueTag::Map: {
				json entries = json::array();
				for (const auto &[key, item] : value.Entries) {
					entries.push_back({{"key", TextJson(key)}, {"value", ToJson(item)}});
				}
				return {{"type", "map"}, {"value", std::move(entries)}};
			}
			case ValueTag::Vector3:
				return {
					{"type", "vector3"},
					{"value", {value.Vector.X, value.Vector.Y, value.Vector.Z}},
				};
			case ValueTag::Color3:
				return {
					{"type", "color3"},
					{"value", {value.Colour.R, value.Colour.G, value.Colour.B}},
				};
			case ValueTag::CFrame:
				return {
					{"type", "cframe"},
					{"value",
					 {value.Frame.Position.X,
					  value.Frame.Position.Y,
					  value.Frame.Position.Z,
					  value.Frame.QuaternionX,
					  value.Frame.QuaternionY,
					  value.Frame.QuaternionZ,
					  value.Frame.QuaternionW}},
				};
			}
			return {{"type", "nil"}};
		}

		bool ParseFloat(const json &value, float &out) {
			if (!value.is_number()) {
				return false;
			}
			const double number = value.get<double>();
			if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
				return false;
			}
			out = static_cast<float>(number);
			return true;
		}

		bool Tuple(const json &value, std::span<float> out) {
			if (!value.is_array() || value.size() != out.size()) {
				return false;
			}
			for (size_t index = 0; index < out.size(); index++) {
				if (!ParseFloat(value[index], out[index])) {
					return false;
				}
			}
			return true;
		}

		bool FromJson(const json &document, ScriptValue &out, std::string &error, const uint32_t depth) {
			if (depth > engine::script::CODEC_MAX_DEPTH) {
				error = "value nests past the script codec limit";
				return false;
			}
			if (!document.is_object()) {
				error = "every value must be an object with a type";
				return false;
			}
			const auto type = document.find("type");
			if (type == document.end() || !type->is_string()) {
				error = "value has no string type";
				return false;
			}

			const std::string kind = type->get<std::string>();
			const auto value = document.find("value");
			if (kind == "nil") {
				out = ScriptValue{};
				return true;
			}
			if (value == document.end()) {
				error = kind + " has no value";
				return false;
			}
			if (kind == "boolean") {
				if (!value->is_boolean()) {
					error = "boolean value is not true or false";
					return false;
				}
				out = ScriptValue(value->get<bool>() ? ValueTag::True : ValueTag::False);
				out.Boolean = value->get<bool>();
				return true;
			}
			if (kind == "number") {
				if (!value->is_number()) {
					error = "number value is not a number";
					return false;
				}
				const double number = value->get<double>();
				if (!std::isfinite(number)) {
					error = "number value is not finite";
					return false;
				}
				out = ScriptValue(ValueTag::Number);
				out.Number = number;
				return true;
			}
			if (kind == "string") {
				out = ScriptValue(ValueTag::String);
				if (!TextFromJson(*value, out.Text)) {
					error = "string value has invalid text encoding";
					return false;
				}
				return true;
			}
			if (kind == "array") {
				if (!value->is_array()) {
					error = "array value is not a list";
					return false;
				}
				out = ScriptValue(ValueTag::Array);
				out.Items.reserve(value->size());
				for (const json &item : *value) {
					ScriptValue decoded;
					if (!FromJson(item, decoded, error, depth + 1)) {
						return false;
					}
					out.Items.push_back(std::move(decoded));
				}
				return true;
			}
			if (kind == "map") {
				if (!value->is_array()) {
					error = "map value is not an entry list";
					return false;
				}
				out = ScriptValue(ValueTag::Map);
				out.Entries.reserve(value->size());
				for (const json &entry : *value) {
					if (!entry.is_object() || !entry.contains("key") || !entry.contains("value")) {
						error = "map entry has no key or value";
						return false;
					}
					std::string key;
					if (!TextFromJson(entry["key"], key)) {
						error = "map entry has an invalid key encoding";
						return false;
					}
					ScriptValue decoded;
					if (!FromJson(entry["value"], decoded, error, depth + 1)) {
						return false;
					}
					out.Entries.emplace_back(key, std::move(decoded));
				}
				return true;
			}

			float components[7]{};
			if (kind == "vector3" && Tuple(*value, std::span<float>(components, 3))) {
				out = ScriptValue(ValueTag::Vector3);
				out.Vector = {components[0], components[1], components[2]};
				return true;
			}
			if (kind == "color3" && Tuple(*value, std::span<float>(components, 3))) {
				out = ScriptValue(ValueTag::Color3);
				out.Colour = {components[0], components[1], components[2]};
				return true;
			}
			if (kind == "cframe" && Tuple(*value, components)) {
				out = ScriptValue(ValueTag::CFrame);
				out.Frame.Position = {components[0], components[1], components[2]};
				out.Frame.QuaternionX = components[3];
				out.Frame.QuaternionY = components[4];
				out.Frame.QuaternionZ = components[5];
				out.Frame.QuaternionW = components[6];
				return true;
			}

			error = "unknown type or wrong component count: " + kind;
			return false;
		}
	}

	bool DatasetValueToText(const std::span<const std::byte> bytes, std::string &text, std::string &error) {
		ScriptValue value;
		const engine::script::CodecStatus status = engine::script::Decode(bytes, value);
		if (status != engine::script::CodecStatus::Ok) {
			error = engine::script::Describe(status);
			return false;
		}
		text = ToJson(value).dump(2);
		error.clear();
		return true;
	}

	bool
	DatasetValueFromText(const std::string_view text, std::vector<std::byte> &bytes, std::string &error) {
		if (text.size() > MAXIMUM_DOCUMENT_BYTES) {
			error = "document is too large";
			return false;
		}
		const json document = json::parse(text, nullptr, false);
		if (document.is_discarded()) {
			error = "document is not valid JSON";
			return false;
		}

		ScriptValue value;
		if (!FromJson(document, value, error, 0)) {
			return false;
		}
		const engine::script::CodecStatus status = engine::script::Encode(value, bytes);
		if (status != engine::script::CodecStatus::Ok) {
			error = engine::script::Describe(status);
			return false;
		}
		error.clear();
		return true;
	}
}
