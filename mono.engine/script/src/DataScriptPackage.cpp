#include <engine/core/Chars.hpp>
#include <engine/script/DataScriptPackage.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::script {
	namespace {
		constexpr uint32_t MAX_DEPTH = 16;
		constexpr size_t MAX_STRING_BYTES = 4096;

		struct JsonValue {
			enum class Kind : uint8_t { Object, Array, String, Number, Boolean, Null };

			Kind Type = Kind::Null;
			std::string String;
			bool Boolean = false;
			std::vector<std::pair<std::string, JsonValue>> Object;
			std::vector<JsonValue> Array;
		};

		class Parser final {
		  public:
			explicit Parser(std::string_view input) : Input(input) {}

			bool Parse(JsonValue &out, std::string &error) {
				if (Input.empty() || Input.size() > DATA_SCRIPT_PACKAGE_MAX_MANIFEST_BYTES) {
					error = "manifest exceeds the byte limit";
					return false;
				}
				if (!Read(out, 0)) {
					error = Error;
					return false;
				}
				SkipSpace();
				if (At != Input.size()) {
					error = "manifest has trailing bytes";
					return false;
				}
				return true;
			}

		  private:
			bool Done() const {
				return At == Input.size();
			}
			char Peek() const {
				return Done() ? '\0' : Input[At];
			}
			void SkipSpace() {
				while (!Done() && (Peek() == ' ' || Peek() == '\n' || Peek() == '\r' || Peek() == '\t'))
					At++;
			}
			bool Fail(std::string message) {
				if (Error.empty()) Error = std::move(message);
				return false;
			}
			bool Take(char expected) {
				SkipSpace();
				if (Peek() != expected) return Fail("manifest token is malformed");
				At++;
				return true;
			}
			bool HexUnit(uint32_t &unit) {
				if (Input.size() - At < 4) return Fail("manifest unicode escape is incomplete");
				unit = 0;
				for (size_t index = 0; index < 4; index++) {
					const char value = Input[At++];
					const uint32_t digit = value >= '0' && value <= '9'	  ? value - '0'
										   : value >= 'a' && value <= 'f' ? value - 'a' + 10
										   : value >= 'A' && value <= 'F' ? value - 'A' + 10
																		  : 16;
					if (digit == 16) return Fail("manifest unicode escape is malformed");
					unit = (unit << 4) | digit;
				}
				return true;
			}
			void AppendUtf8(uint32_t unit, std::string &out) {
				if (unit < 0x80) {
					out += static_cast<char>(unit);
				} else if (unit < 0x800) {
					out += static_cast<char>(0xC0 | (unit >> 6));
					out += static_cast<char>(0x80 | (unit & 0x3F));
				} else if (unit < 0x10000) {
					out += static_cast<char>(0xE0 | (unit >> 12));
					out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (unit & 0x3F));
				} else {
					out += static_cast<char>(0xF0 | (unit >> 18));
					out += static_cast<char>(0x80 | ((unit >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (unit & 0x3F));
				}
			}
			bool ReadString(std::string &out) {
				if (!Take('\"')) return false;
				out.clear();
				while (!Done() && Peek() != '\"') {
					const char value = Input[At++];
					if (static_cast<unsigned char>(value) < 0x20)
						return Fail("manifest string has a control byte");
					if (value != '\\') {
						out += value;
					} else {
						if (Done()) return Fail("manifest string ends in an escape");
						switch (Input[At++]) {
						case '\"':
							out += '\"';
							break;
						case '\\':
							out += '\\';
							break;
						case '/':
							out += '/';
							break;
						case 'b':
							out += '\b';
							break;
						case 'f':
							out += '\f';
							break;
						case 'n':
							out += '\n';
							break;
						case 'r':
							out += '\r';
							break;
						case 't':
							out += '\t';
							break;
						case 'u': {
							uint32_t unit = 0;
							if (!HexUnit(unit)) return false;
							if (unit >= 0xD800 && unit <= 0xDBFF) {
								if (Input.size() - At < 6 || Input[At++] != '\\' || Input[At++] != 'u')
									return Fail("manifest unicode high surrogate is unpaired");
								uint32_t low = 0;
								if (!HexUnit(low)) return false;
								if (low < 0xDC00 || low > 0xDFFF)
									return Fail("manifest unicode high surrogate is unpaired");
								unit = 0x10000 + ((unit - 0xD800) << 10) + low - 0xDC00;
							} else if (unit >= 0xDC00 && unit <= 0xDFFF) {
								return Fail("manifest unicode low surrogate is unpaired");
							}
							AppendUtf8(unit, out);
							break;
						}
						default:
							return Fail("manifest string has an unsupported escape");
						}
					}
					if (out.size() > MAX_STRING_BYTES) return Fail("manifest string exceeds the byte limit");
				}
				if (Done()) return Fail("manifest string is not closed");
				At++;
				return true;
			}
			bool ReadNumber(JsonValue &out) {
				const size_t start = At;
				if (Peek() == '-') At++;
				if (Peek() == '0') {
					At++;
				} else if (Peek() >= '1' && Peek() <= '9') {
					do
						At++;
					while (Peek() >= '0' && Peek() <= '9');
				} else {
					return Fail("manifest number is malformed");
				}
				if (Peek() == '.') {
					At++;
					const size_t fraction = At;
					while (Peek() >= '0' && Peek() <= '9')
						At++;
					if (fraction == At) return Fail("manifest number is malformed");
				}
				if (Peek() == 'e' || Peek() == 'E') {
					At++;
					if (Peek() == '+' || Peek() == '-') At++;
					const size_t exponent = At;
					while (Peek() >= '0' && Peek() <= '9')
						At++;
					if (exponent == At) return Fail("manifest number is malformed");
				}
				out = {};
				out.Type = JsonValue::Kind::Number;
				out.String = std::string(Input.substr(start, At - start));
				return true;
			}
			bool ReadObject(JsonValue &out, uint32_t depth) {
				out = {};
				out.Type = JsonValue::Kind::Object;
				At++;
				SkipSpace();
				if (Peek() == '}') {
					At++;
					return true;
				}
				while (true) {
					std::string key;
					if (!ReadString(key) || !Take(':')) return false;
					JsonValue value;
					if (!Read(value, depth + 1)) return false;
					for (const auto &[known, ignored] : out.Object) {
						(void)ignored;
						if (known == key) return Fail("manifest object has a duplicate key");
					}
					out.Object.emplace_back(std::move(key), std::move(value));
					SkipSpace();
					if (Peek() == '}') {
						At++;
						return true;
					}
					if (Peek() != ',') return Fail("manifest object is not closed");
					At++;
				}
			}
			bool ReadArray(JsonValue &out, uint32_t depth) {
				out = {};
				out.Type = JsonValue::Kind::Array;
				At++;
				SkipSpace();
				if (Peek() == ']') {
					At++;
					return true;
				}
				while (true) {
					if (out.Array.size() >= DATA_SCRIPT_PACKAGE_MAX_ASSETS)
						return Fail("manifest array exceeds the item limit");
					JsonValue value;
					if (!Read(value, depth + 1)) return false;
					out.Array.push_back(std::move(value));
					SkipSpace();
					if (Peek() == ']') {
						At++;
						return true;
					}
					if (Peek() != ',') return Fail("manifest array is not closed");
					At++;
				}
			}
			bool Read(JsonValue &out, uint32_t depth) {
				if (depth > MAX_DEPTH) return Fail("manifest exceeds the nesting limit");
				SkipSpace();
				switch (Peek()) {
				case '{':
					return ReadObject(out, depth);
				case '[':
					return ReadArray(out, depth);
				case '\"':
					out = {};
					out.Type = JsonValue::Kind::String;
					return ReadString(out.String);
				case 't':
					if (Input.substr(At, 4) != "true") return Fail("manifest value is malformed");
					At += 4;
					out = {};
					out.Type = JsonValue::Kind::Boolean;
					out.Boolean = true;
					return true;
				case 'f':
					if (Input.substr(At, 5) != "false") return Fail("manifest value is malformed");
					At += 5;
					out = {};
					out.Type = JsonValue::Kind::Boolean;
					return true;
				case 'n':
					if (Input.substr(At, 4) != "null") return Fail("manifest value is malformed");
					At += 4;
					out = JsonValue{};
					return true;
				default:
					return ReadNumber(out);
				}
			}

			std::string_view Input;
			size_t At = 0;
			std::string Error;
		};

		const JsonValue *Member(const JsonValue &object, std::string_view name) {
			if (object.Type != JsonValue::Kind::Object) return nullptr;
			for (const auto &[key, value] : object.Object)
				if (key == name) return &value;
			return nullptr;
		}

		bool OnlyMembers(
			const JsonValue &object, std::initializer_list<std::string_view> names, std::string &error
		) {
			if (object.Type != JsonValue::Kind::Object) {
				error = "manifest value must be an object";
				return false;
			}
			for (const auto &[key, ignored] : object.Object) {
				(void)ignored;
				bool known = false;
				for (const std::string_view name : names)
					known = known || key == name;
				if (!known) {
					error = "manifest has an unknown field: " + key;
					return false;
				}
			}
			return true;
		}

		bool RelativePath(std::string_view path) {
			if (path.empty() || path.size() > 256 || path.front() == '/' ||
				path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos)
				return false;
			size_t segment = 0;
			while (segment < path.size()) {
				const size_t end = path.find('/', segment);
				const std::string_view part = path.substr(
					segment, end == std::string_view::npos ? path.size() - segment : end - segment
				);
				if (part.empty() || part == "." || part == "..") return false;
				for (const char value : part)
					if (static_cast<unsigned char>(value) < 0x20) return false;
				if (end == std::string_view::npos) break;
				segment = end + 1;
			}
			return true;
		}

		template <typename Integer> bool ExactInteger(const JsonValue &value, Integer &out) {
			if (value.Type != JsonValue::Kind::Number ||
				value.String.find_first_of(".eE") != std::string::npos)
				return false;
			const auto read =
				std::from_chars(value.String.data(), value.String.data() + value.String.size(), out);
			return read.ec == std::errc{} && read.ptr == value.String.data() + value.String.size();
		}

		bool Hash(const JsonValue *value, assets::ContentHash &out) {
			if (value == nullptr || value->Type != JsonValue::Kind::String) return false;
			const auto parsed = assets::ContentHash::FromHex(value->String);
			if (!parsed) return false;
			out = *parsed;
			return true;
		}

		bool Capability(std::string_view name, ScriptCapabilities &out) {
			constexpr std::array entries{
				std::pair{"world", ScriptCapabilities::World},
				std::pair{"messaging", ScriptCapabilities::Messaging},
				std::pair{"persistence", ScriptCapabilities::Persistence},
				std::pair{"teleport", ScriptCapabilities::Teleport},
				std::pair{"input", ScriptCapabilities::Input},
				std::pair{"audio", ScriptCapabilities::Audio},
				std::pair{"studio-debug", ScriptCapabilities::StudioDebug},
				std::pair{"plugin-host", ScriptCapabilities::PluginHost},
			};
			for (const auto &[text, capability] : entries) {
				if (name == text) {
					out |= capability;
					return true;
				}
			}
			return false;
		}
	}

	DataScriptPackageParseResult ParseDataScriptPackage(std::string_view manifest) {
		JsonValue root;
		DataScriptPackageParseResult result;
		Parser parser(manifest);
		if (!parser.Parse(root, result.Error)) return result;
		if (!OnlyMembers(
				root,
				{"format", "entry", "source_hash", "assets", "parameters", "capabilities", "budget", "seed"},
				result.Error
			))
			return result;

		const JsonValue *format = Member(root, "format");
		const JsonValue *entry = Member(root, "entry");
		const JsonValue *assets = Member(root, "assets");
		const JsonValue *parameters = Member(root, "parameters");
		const JsonValue *capabilities = Member(root, "capabilities");
		const JsonValue *budget = Member(root, "budget");
		const JsonValue *seed = Member(root, "seed");
		if (format == nullptr || format->Type != JsonValue::Kind::String ||
			format->String != DataScriptPackage::FORMAT || entry == nullptr ||
			entry->Type != JsonValue::Kind::String || !RelativePath(entry->String) || assets == nullptr ||
			assets->Type != JsonValue::Kind::Array || parameters == nullptr ||
			parameters->Type != JsonValue::Kind::Array || capabilities == nullptr ||
			capabilities->Type != JsonValue::Kind::Array || budget == nullptr || seed == nullptr) {
			result.Error = "manifest has missing or invalid required fields";
			return result;
		}

		DataScriptPackage package;
		package.Entry = entry->String;
		if (!Hash(Member(root, "source_hash"), package.SourceHash)) {
			result.Error = "manifest source_hash is invalid";
			return result;
		}
		if (!ExactInteger(*seed, package.Seed)) {
			result.Error = "manifest seed must be an unsigned integer";
			return result;
		}

		const JsonValue *sourceBytes = Member(*budget, "source_bytes");
		const JsonValue *assetBytes = Member(*budget, "asset_bytes");
		const JsonValue *assetCount = Member(*budget, "assets");
		const JsonValue *parameterCount = Member(*budget, "parameters");
		if (!OnlyMembers(*budget, {"source_bytes", "asset_bytes", "assets", "parameters"}, result.Error) ||
			sourceBytes == nullptr || assetBytes == nullptr || assetCount == nullptr ||
			parameterCount == nullptr || !ExactInteger(*sourceBytes, package.Budget.SourceBytes) ||
			!ExactInteger(*assetBytes, package.Budget.AssetBytes) ||
			!ExactInteger(*assetCount, package.Budget.Assets) ||
			!ExactInteger(*parameterCount, package.Budget.Parameters) ||
			package.Budget.SourceBytes > DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES ||
			package.Budget.AssetBytes > DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES ||
			package.Budget.Assets > DATA_SCRIPT_PACKAGE_MAX_ASSETS ||
			package.Budget.Parameters > DATA_SCRIPT_PACKAGE_MAX_PARAMETERS) {
			result.Error = "manifest budget is invalid or exceeds package limits";
			return result;
		}

		for (const JsonValue &item : assets->Array) {
			if (!OnlyMembers(item, {"path", "hash"}, result.Error)) return result;
			const JsonValue *path = Member(item, "path");
			DataScriptPackageAsset asset;
			if (path == nullptr || path->Type != JsonValue::Kind::String || !RelativePath(path->String) ||
				!Hash(Member(item, "hash"), asset.Hash)) {
				result.Error = "manifest asset is invalid";
				return result;
			}
			asset.Path = path->String;
			for (const auto &known : package.Assets)
				if (known.Path == asset.Path) {
					result.Error = "manifest has duplicate asset paths";
					return result;
				}
			package.Assets.push_back(std::move(asset));
		}
		if (package.Assets.size() > package.Budget.Assets) {
			result.Error = "manifest asset count exceeds its budget";
			return result;
		}

		for (const JsonValue &item : parameters->Array) {
			if (!OnlyMembers(item, {"name", "type", "value"}, result.Error)) return result;
			const JsonValue *name = Member(item, "name");
			const JsonValue *type = Member(item, "type");
			const JsonValue *value = Member(item, "value");
			if (name == nullptr || name->Type != JsonValue::Kind::String || name->String.empty() ||
				name->String.find('\0') != std::string::npos || name->String.size() > 128 ||
				type == nullptr || type->Type != JsonValue::Kind::String || value == nullptr) {
				result.Error = "manifest parameter is invalid";
				return result;
			}
			DataScriptParameter parameter;
			parameter.Name = name->String;
			if (type->String == "boolean" && value->Type == JsonValue::Kind::Boolean) {
				parameter.Value.Type = DataScriptScalar::Kind::Boolean;
				parameter.Value.Boolean = value->Boolean;
			} else if (type->String == "integer" && ExactInteger(*value, parameter.Value.Integer))
				parameter.Value.Type = DataScriptScalar::Kind::Integer;
			else if (type->String == "number" && value->Type == JsonValue::Kind::Number) {
				const auto read = core::FromChars(
					value->String.data(), value->String.data() + value->String.size(), parameter.Value.Number
				);
				if (read.ec != std::errc{} || read.ptr != value->String.data() + value->String.size() ||
					!std::isfinite(parameter.Value.Number)) {
					result.Error = "manifest number parameter is invalid";
					return result;
				}
				parameter.Value.Type = DataScriptScalar::Kind::Number;
			} else if (type->String == "string" && value->Type == JsonValue::Kind::String) {
				parameter.Value.Type = DataScriptScalar::Kind::String;
				parameter.Value.String = value->String;
			} else {
				result.Error = "manifest parameter type does not match its value";
				return result;
			}
			for (const auto &known : package.Parameters)
				if (known.Name == parameter.Name) {
					result.Error = "manifest has duplicate parameter names";
					return result;
				}
			package.Parameters.push_back(std::move(parameter));
		}
		if (package.Parameters.size() > package.Budget.Parameters) {
			result.Error = "manifest parameter count exceeds its budget";
			return result;
		}

		for (const JsonValue &item : capabilities->Array) {
			ScriptCapabilities capability = ScriptCapabilities::None;
			if (item.Type != JsonValue::Kind::String || !Capability(item.String, capability) ||
				HasCapabilities(package.Capabilities, capability)) {
				result.Error = "manifest capability is invalid or duplicated";
				return result;
			}
			package.Capabilities |= capability;
		}
		result.Package = std::move(package);
		return result;
	}

	uint64_t DataScriptSeedStream(uint64_t seed, std::string_view stream) {
		assets::Hasher hasher;
		constexpr std::string_view domain = "atomic.data-script.seed.v1\0";
		hasher.Update(std::as_bytes(std::span(domain.data(), domain.size())));
		std::array<std::byte, sizeof(seed)> bytes{};
		for (size_t index = 0; index < bytes.size(); index++)
			bytes[index] = static_cast<std::byte>(seed >> (index * 8));
		hasher.Update(bytes);
		hasher.Update(std::as_bytes(std::span(stream.data(), stream.size())));
		const assets::ContentHash hash = hasher.Finish();
		uint64_t result = 0;
		for (size_t index = 0; index < sizeof(result); index++)
			result |= static_cast<uint64_t>(hash.Digest[index]) << (index * 8);
		return result;
	}
}
