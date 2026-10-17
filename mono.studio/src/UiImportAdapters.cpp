#include <engine/core/Name.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <studio/UiImportAdapters.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studio {
	namespace {
		using engine::gui::DocumentIssue;
		using engine::gui::DocumentReport;
		using nlohmann::json;

		void Issue(DocumentReport &report, std::string path, std::string message, const uint32_t line) {
			if (report.Issues.size() < engine::gui::DocumentLimits::HARD_MAXIMUM_ISSUES)
				report.Issues.push_back({std::move(path), std::move(message), line});
			else
				report.Truncated = true;
		}

		uint32_t LineAt(const std::string_view source, const size_t offset) {
			uint32_t line = 1;
			for (size_t index = 0; index < offset && index < source.size(); index++) {
				if (source[index] == '\n') line++;
			}
			return line;
		}

		uint32_t KeyLine(const std::string_view source, const std::string_view key, const size_t from = 0) {
			const std::string needle = "\"" + std::string(key) + "\"";
			const size_t found = source.find(needle, from);
			return LineAt(source, found == std::string_view::npos ? 0 : found);
		}

		bool LimitsValid(const UiDesignImportLimits &limits, DocumentReport &report) {
			if (limits.MaximumBytes == 0 || limits.MaximumBytes > UiDesignImportLimits::HARD_MAXIMUM_BYTES ||
				limits.MaximumDepth == 0 || limits.MaximumDepth > UiDesignImportLimits::HARD_MAXIMUM_DEPTH) {
				Issue(report, "$", "import limits exceed adapter ceilings", 1);
				return false;
			}
			return true;
		}

		bool JsonDepthWithin(const std::string_view source, const size_t maximum, DocumentReport &report) {
			size_t depth = 0;
			bool quoted = false;
			bool escaped = false;
			for (size_t index = 0; index < source.size(); index++) {
				const char character = source[index];
				if (quoted) {
					if (escaped)
						escaped = false;
					else if (character == '\\')
						escaped = true;
					else if (character == '\"')
						quoted = false;
					continue;
				}
				if (character == '\"')
					quoted = true;
				else if (character == '{' || character == '[') {
					if (++depth > maximum) {
						Issue(report, "$", "JSON nesting limit exceeded", LineAt(source, index));
						return false;
					}
				} else if ((character == '}' || character == ']') && depth > 0) {
					depth--;
				}
			}
			return true;
		}

		bool ExactKeys(
			const json &value,
			const std::initializer_list<std::string_view> keys,
			const std::string_view source,
			const std::string &path,
			DocumentReport &report
		) {
			if (!value.is_object()) {
				Issue(report, path, "value must be an object", 1);
				return false;
			}
			for (const auto &[key, unused] : value.items()) {
				const bool known =
					std::ranges::any_of(keys, [&](const std::string_view allowed) { return key == allowed; });
				if (!known) Issue(report, path + "." + key, "unknown field", KeyLine(source, key));
			}
			return report.Ok();
		}

		bool FiniteFloat(const json &value, float &out) {
			if (!value.is_number()) return false;
			const double number = value.get<double>();
			if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
				number > std::numeric_limits<float>::max())
				return false;
			out = static_cast<float>(number);
			return true;
		}

		class TokenKeySax final : public nlohmann::json_sax<json> {
		  public:
			bool null() override {
				return true;
			}
			bool boolean(bool) override {
				return true;
			}
			bool number_integer(number_integer_t) override {
				return true;
			}
			bool number_unsigned(number_unsigned_t) override {
				return true;
			}
			bool number_float(number_float_t, const string_t &) override {
				return true;
			}
			bool string(string_t &) override {
				return true;
			}
			bool binary(binary_t &) override {
				return true;
			}
			bool start_object(std::size_t) override {
				Scopes.push_back(PendingKey == "tokens" ? Scope::Tokens : Scope::Other);
				PendingKey.clear();
				return true;
			}
			bool key(string_t &value) override {
				if (!Scopes.empty() && Scopes.back() == Scope::Tokens && !TokenNames.insert(value).second) {
					Duplicate = value;
					return false;
				}
				PendingKey = value;
				return true;
			}
			bool end_object() override {
				Scopes.pop_back();
				return true;
			}
			bool start_array(std::size_t) override {
				return true;
			}
			bool end_array() override {
				return true;
			}
			bool parse_error(
				std::size_t position, const std::string &, const nlohmann::detail::exception &
			) override {
				ErrorByte = position;
				return false;
			}

			std::string Duplicate;
			size_t ErrorByte = 0;

		  private:
			enum class Scope : uint8_t { Other, Tokens };
			std::vector<Scope> Scopes;
			std::unordered_set<std::string> TokenNames;
			std::string PendingKey;
		};

	}

	bool ImportUiDesignTokensJson(
		const std::string_view source,
		engine::gui::UiDocument &out,
		DocumentReport &report,
		const UiDesignImportLimits &limits
	) {
		report = {};
		if (!LimitsValid(limits, report)) return false;
		if (source.size() > limits.MaximumBytes) {
			Issue(report, "$", "design token source exceeds byte limit", 1);
			return false;
		}
		if (!JsonDepthWithin(source, limits.MaximumDepth, report)) return false;
		TokenKeySax sax;
		if (!json::sax_parse(source, &sax)) {
			if (!sax.Duplicate.empty())
				Issue(
					report,
					"$.theme.tokens." + sax.Duplicate,
					"token name is duplicated",
					KeyLine(source, sax.Duplicate)
				);
			else
				Issue(report, "$", "malformed JSON", LineAt(source, sax.ErrorByte));
			return false;
		}
		json parsed;
		try {
			parsed = json::parse(source);
		} catch (const json::parse_error &error) {
			Issue(report, "$", "malformed JSON", LineAt(source, error.byte));
			return false;
		} catch (const std::exception &) {
			Issue(report, "$", "could not parse JSON", 1);
			return false;
		}
		if (!ExactKeys(parsed, {"version", "theme"}, source, "$", report)) return false;
		const auto version = parsed.find("version");
		const auto theme = parsed.find("theme");
		if (version == parsed.end() || !version->is_number_integer() || version->get<int64_t>() != 1) {
			Issue(report, "$.version", "design token schema version must be 1", KeyLine(source, "version"));
		}
		if (theme == parsed.end()) Issue(report, "$.theme", "theme is required", 1);
		if (!report.Ok()) return false;
		if (!ExactKeys(*theme, {"id", "name", "tokens"}, source, "$.theme", report)) return false;
		const auto id = theme->find("id");
		const auto name = theme->find("name");
		const auto tokens = theme->find("tokens");
		if (id == theme->end() || !id->is_string() || id->get_ref<const std::string &>().empty())
			Issue(report, "$.theme.id", "theme id must be a non-empty string", KeyLine(source, "id"));
		if (name == theme->end() || !name->is_string())
			Issue(report, "$.theme.name", "theme name must be a string", KeyLine(source, "name"));
		if (tokens == theme->end() || !tokens->is_object())
			Issue(report, "$.theme.tokens", "tokens must be an object", KeyLine(source, "tokens"));
		if (!report.Ok()) return false;
		if (tokens->size() > engine::gui::StyleSet::MAXIMUM_DECLARATIONS) {
			Issue(report, "$.theme.tokens", "theme token limit exceeded", KeyLine(source, "tokens"));
			return false;
		}
		if (id->get_ref<const std::string &>().size() > engine::gui::DocumentLimits{}.MaximumStringBytes)
			Issue(report, "$.theme.id", "theme id exceeds string limit", KeyLine(source, "id"));
		if (name->get_ref<const std::string &>().size() > engine::gui::DocumentLimits{}.MaximumStringBytes)
			Issue(report, "$.theme.name", "theme name exceeds string limit", KeyLine(source, "name"));
		if (!report.Ok()) return false;

		engine::gui::DocumentTheme imported;
		imported.Id = id->get<std::string>();
		imported.Name = name->get<std::string>();
		for (const auto &[tokenName, token] : tokens->items()) {
			const std::string path = "$.theme.tokens." + tokenName;
			const uint32_t line = KeyLine(source, tokenName);
			if (tokenName.empty()) Issue(report, path, "token name must not be empty", line);
			if (tokenName.size() > engine::gui::DocumentLimits{}.MaximumStringBytes)
				Issue(report, path, "token name exceeds string limit", line);
			if (!report.Ok()) continue;
			if (!ExactKeys(token, {"type", "value"}, source, path, report)) continue;
			const auto type = token.find("type");
			const auto value = token.find("value");
			if (type == token.end() || !type->is_string() || value == token.end()) {
				Issue(report, path, "token needs string type and value", line);
				continue;
			}
			engine::gui::StyleValue style;
			const std::string kind = type->get<std::string>();
			if (kind == "number") {
				float number = 0.0f;
				if (!FiniteFloat(*value, number))
					Issue(report, path, "number token must be finite", line);
				else
					style = engine::gui::StyleValue::FromNumber(number);
			} else if (kind == "color") {
				if (!value->is_array() || value->size() != 3) {
					Issue(report, path, "color token must contain three finite RGB values", line);
					continue;
				}
				float rgb[3]{};
				bool valid = true;
				for (size_t index = 0; index < 3; index++)
					valid &= FiniteFloat((*value)[index], rgb[index]);
				if (!valid)
					Issue(report, path, "color token must contain three finite RGB values", line);
				else
					style = engine::gui::StyleValue::FromColor({rgb[0], rgb[1], rgb[2]});
			} else {
				Issue(report, path + ".type", "token type must be color or number", line);
				continue;
			}
			if (report.Ok() && !imported.Tokens.Set({engine::core::Name(tokenName), style}))
				Issue(report, path, "token duplicates a name with a different type", line);
		}
		if (!report.Ok()) return false;
		engine::gui::UiDocument document;
		document.Themes.push_back(std::move(imported));
		if (!engine::gui::ValidateDocument(document, report)) return false;
		out = std::move(document);
		return true;
	}

	bool ImportUiLocalizationCsv(
		const std::string_view source,
		engine::gui::LocalizationCatalogue &out,
		DocumentReport &report,
		const UiDesignImportLimits &limits
	) {
		report = {};
		if (!LimitsValid(limits, report)) return false;
		if (source.size() > limits.MaximumBytes) {
			Issue(report, "$", "localization source exceeds byte limit", 1);
			return false;
		}
		return engine::gui::DecodeLocalizationCsv(source, out, report);
	}
}
