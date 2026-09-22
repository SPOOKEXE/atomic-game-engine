#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Localization.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::gui {
	namespace {
		constexpr size_t MAXIMUM_FORMAT_DEPTH = 8;
		constexpr size_t MAXIMUM_SELECTOR_CASES = 16;
		constexpr std::string_view FSI = "\xE2\x81\xA8";
		constexpr std::string_view PDI = "\xE2\x81\xA9";

		std::string_view Trim(std::string_view value) {
			const size_t first = value.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) return {};
			return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
		}

		const LocalizedArgument *Find(const LocalizedMessage &message, std::string_view name) {
			for (size_t index = 0; index < std::min(message.ArgumentCount, message.Arguments.size()); ++index)
				if (message.Arguments[index].Name.Text() == name) return &message.Arguments[index];
			return nullptr;
		}

		bool Add(std::string &output, std::string_view value, bool &limited) {
			if (value.size() > LocalizationCatalogue::MAXIMUM_OUTPUT_BYTES - output.size()) {
				limited = true;
				return false;
			}
			output += value;
			return true;
		}

		std::string Number(double value) {
			char buffer[64];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
			return result.ec == std::errc{} ? std::string(buffer, result.ptr) : "[invalid number]";
		}

		std::string Date(int64_t seconds) {
			int64_t days = seconds / 86400;
			if (seconds < 0 && seconds % 86400) --days;
			const int64_t z = days + 719468;
			const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
			const unsigned dayOfEra = static_cast<unsigned>(z - era * 146097);
			const unsigned yearOfEra =
				(dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
			int64_t year = static_cast<int64_t>(yearOfEra) + era * 400;
			const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
			const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
			const unsigned day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
			const int month = static_cast<int>(monthIndex) + (monthIndex < 10 ? 3 : -9);
			year += month <= 2;
			char buffer[32];
			std::snprintf(
				buffer, sizeof(buffer), "%04lld-%02d-%02u", static_cast<long long>(year), month, day
			);
			return buffer;
		}

		bool Format(
			std::string_view source,
			const LocalizedMessage &message,
			std::string &output,
			size_t depth,
			std::optional<double> pluralCount,
			bool &limited
		);

		bool FormatSelector(
			std::string_view cases,
			std::string_view desired,
			std::string_view exact,
			const LocalizedMessage &message,
			std::string &output,
			size_t depth,
			std::optional<double> pluralCount,
			bool &limited
		) {
			std::optional<std::string_view> selected;
			std::optional<std::string_view> fallback;
			size_t cursor = 0;
			for (size_t parsed = 0; cursor < cases.size(); ++parsed) {
				if (parsed == MAXIMUM_SELECTOR_CASES) return false;
				while (cursor < cases.size() && std::isspace(static_cast<unsigned char>(cases[cursor])))
					++cursor;
				if (cursor == cases.size()) break;
				const size_t keyBegin = cursor;
				while (cursor < cases.size() && !std::isspace(static_cast<unsigned char>(cases[cursor])) &&
					   cases[cursor] != '{')
					++cursor;
				const std::string_view key = cases.substr(keyBegin, cursor - keyBegin);
				while (cursor < cases.size() && std::isspace(static_cast<unsigned char>(cases[cursor])))
					++cursor;
				if (key.empty() || cursor == cases.size() || cases[cursor] != '{') return false;
				const size_t bodyBegin = ++cursor;
				int braces = 1;
				for (; cursor < cases.size() && braces; ++cursor)
					braces += cases[cursor] == '{' ? 1 : cases[cursor] == '}' ? -1 : 0;
				if (braces != 0) return false;
				const std::string_view body = cases.substr(bodyBegin, cursor - bodyBegin - 1);
				if (key == exact)
					selected = body;
				else if (key == desired && !selected)
					selected = body;
				else if (key == "other")
					fallback = body;
			}
			if (!selected) selected = fallback;
			return selected && Format(*selected, message, output, depth + 1, pluralCount, limited);
		}

		bool Format(
			std::string_view source,
			const LocalizedMessage &message,
			std::string &output,
			size_t depth,
			std::optional<double> pluralCount,
			bool &limited
		) {
			if (depth > MAXIMUM_FORMAT_DEPTH) return false;
			for (size_t index = 0; index < source.size();) {
				if (source[index] == '#' && pluralCount) {
					if (!Add(output, Number(*pluralCount), limited)) return false;
					++index;
					continue;
				}
				if (source[index] != '{') {
					if (!Add(output, source.substr(index, 1), limited)) return false;
					++index;
					continue;
				}

				size_t end = index + 1;
				int braces = 1;
				for (; end < source.size() && braces; ++end)
					braces += source[end] == '{' ? 1 : source[end] == '}' ? -1 : 0;
				if (braces != 0) return false;

				const std::string_view token = source.substr(index + 1, end - index - 2);
				const size_t firstComma = token.find(',');
				const LocalizedArgument *argument = Find(message, Trim(token.substr(0, firstComma)));
				if (argument == nullptr) return false;
				if (firstComma == std::string_view::npos) {
					if (argument->Type != LocalizedArgumentType::String || !Add(output, FSI, limited) ||
						!Add(output, argument->Value, limited) || !Add(output, PDI, limited))
						return false;
				} else {
					const size_t secondComma = token.find(',', firstComma + 1);
					const std::string_view type =
						Trim(token.substr(firstComma + 1, secondComma - firstComma - 1));
					if (type == "number" && secondComma == std::string_view::npos &&
						argument->Type == LocalizedArgumentType::Number) {
						if (!Add(output, Number(argument->Number), limited)) return false;
					} else if (type == "date" && secondComma == std::string_view::npos &&
							   argument->Type == LocalizedArgumentType::Date) {
						if (!Add(output, Date(argument->UnixSeconds), limited)) return false;
					} else if (type == "plural" && secondComma != std::string_view::npos &&
							   argument->Type == LocalizedArgumentType::Number) {
						const std::string value = Number(argument->Number);
						const std::string_view category = argument->Number == 1.0 ? "one" : "other";
						if (!FormatSelector(
								Trim(token.substr(secondComma + 1)),
								category,
								"=" + value,
								message,
								output,
								depth,
								argument->Number,
								limited
							))
							return false;
					} else if (type == "select" && secondComma != std::string_view::npos &&
							   argument->Type == LocalizedArgumentType::String) {
						if (!FormatSelector(
								Trim(token.substr(secondComma + 1)),
								argument->Value,
								{},
								message,
								output,
								depth,
								pluralCount,
								limited
							))
							return false;
					} else {
						return false;
					}
				}
				index = end;
			}
			return true;
		}

		std::string Render(std::string_view source, const LocalizedMessage &message) {
			if (source.size() > LocalizationCatalogue::MAXIMUM_MESSAGE_BYTES) return "[localization limit]";
			std::string output;
			bool limited = false;
			if (Format(source, message, output, 0, std::nullopt, limited)) return output;
			return limited ? "[localization limit]" : std::string(source);
		}
	}

	LocalizedArgument LocalizedArgument::FromNumber(core::Name name, double value) {
		LocalizedArgument argument;
		argument.Name = name;
		argument.Type = LocalizedArgumentType::Number;
		argument.Number = value;
		return argument;
	}

	LocalizedArgument LocalizedArgument::FromDate(core::Name name, int64_t unixSeconds) {
		LocalizedArgument argument;
		argument.Name = name;
		argument.Type = LocalizedArgumentType::Date;
		argument.UnixSeconds = unixSeconds;
		return argument;
	}

	bool LocalizedMessage::AddArgument(LocalizedArgument argument) {
		if (!argument.Name.IsValid() || ArgumentCount == Arguments.size() ||
			argument.Value.size() > LocalizationCatalogue::MAXIMUM_OUTPUT_BYTES ||
			(argument.Type == LocalizedArgumentType::Number && !std::isfinite(argument.Number)))
			return false;
		Arguments[ArgumentCount++] = std::move(argument);
		return true;
	}

	bool LocalizationCatalogue::Set(CatalogueEntry entry) {
		if (entry.Locale.empty() || entry.Locale.size() > MAXIMUM_LOCALE_BYTES || !entry.Key.IsValid() ||
			entry.Text.size() > MAXIMUM_MESSAGE_BYTES)
			return false;
		for (size_t index = 0; index < Count; ++index)
			if (Entries[index].Locale == entry.Locale && Entries[index].Key == entry.Key) {
				Entries[index] = std::move(entry);
				++RevisionNumber;
				return true;
			}
		if (Count == Entries.size()) return false;
		Entries[Count++] = std::move(entry);
		++RevisionNumber;
		return true;
	}

	bool LocalizationCatalogue::Merge(const LocalizationCatalogue &other) {
		for (size_t index = 0; index < other.Count; ++index)
			if (!Set(other.Entries[index])) return false;
		return true;
	}

	bool LocalizationCatalogue::SetSourceLocale(std::string locale) {
		if (locale.empty() || locale.size() > MAXIMUM_LOCALE_BYTES) return false;
		if (locale != SourceLocale) {
			SourceLocale = std::move(locale);
			++RevisionNumber;
		}
		return true;
	}

	std::string LocalizationCatalogue::Resolve(
		const LocalizedMessage &message, std::string_view locale, bool studioMissingMarker
	) const {
		auto find = [&](std::string_view candidate) {
			for (size_t index = 0; index < Count; ++index)
				if (Entries[index].Locale == candidate && Entries[index].Key == message.Key)
					return &Entries[index];
			return static_cast<const CatalogueEntry *>(nullptr);
		};
		if (locale.size() > MAXIMUM_LOCALE_BYTES) locale = {};
		for (auto candidate = locale; !candidate.empty();) {
			if (const CatalogueEntry *entry = find(candidate)) return Render(entry->Text, message);
			const size_t parent = candidate.rfind('-');
			candidate = parent == std::string_view::npos ? std::string_view{} : candidate.substr(0, parent);
		}
		if (const CatalogueEntry *entry = find(SourceLocale)) return Render(entry->Text, message);
		const std::string rendered = Render(message.Source, message);
		if (!studioMissingMarker || !message.Key.IsValid()) return rendered;
		const std::string marker = "[missing " + std::string(message.Key.Text()) + "] ";
		return marker.size() > MAXIMUM_OUTPUT_BYTES - rendered.size() ? "[localization limit]"
																	  : marker + rendered;
	}

	bool
	DecodeLocalizationCsv(const std::string_view source, LocalizationCatalogue &out, DocumentReport &report) {
		report = {};
		auto issue = [&](std::string path, std::string message, const uint32_t line) {
			if (report.Issues.size() < DocumentLimits::HARD_MAXIMUM_ISSUES)
				report.Issues.push_back({std::move(path), std::move(message), line});
			else
				report.Truncated = true;
		};
		auto validUtf8 = [](const std::string_view text) {
			size_t at = 0;
			while (at < text.size()) {
				const uint8_t first = static_cast<uint8_t>(text[at++]);
				if (first < 0x80) continue;
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
				} else
					return false;
				if (at + continuation > text.size()) return false;
				for (size_t index = 0; index < continuation; ++index) {
					const uint8_t next = static_cast<uint8_t>(text[at++]);
					if ((next & 0xc0u) != 0x80u) return false;
					codepoint = (codepoint << 6u) | (next & 0x3fu);
				}
				if (codepoint < minimum || codepoint > 0x10ffffu ||
					(codepoint >= 0xd800u && codepoint <= 0xdfffu))
					return false;
			}
			return true;
		};
		if (source.size() > LocalizationCatalogue::MAXIMUM_SOURCE_BYTES) {
			issue("$", "localization source exceeds byte limit", 1);
			return false;
		}
		struct Row {
			std::vector<std::string> Fields;
			uint32_t Line = 1;
		};
		std::vector<Row> rows;
		std::vector<std::string> row;
		std::string field;
		bool quoted = false;
		bool quoteClosed = false;
		uint32_t line = 1;
		uint32_t recordLine = 1;
		for (size_t index = 0; index < source.size(); ++index) {
			const char character = source[index];
			if (quoted) {
				if (character == '"') {
					if (index + 1 < source.size() && source[index + 1] == '"') {
						field.push_back(character);
						++index;
					} else {
						quoted = false;
						quoteClosed = true;
					}
				} else {
					field.push_back(character);
					if (character == '\n') ++line;
				}
				continue;
			}
			if (quoteClosed && character != ',' && character != '\n' && character != '\r') {
				issue("$", "CSV text follows a closing quote", line);
				return false;
			}
			if (character == '"') {
				if (!field.empty()) {
					issue("$", "quote begins after unquoted CSV text", line);
					return false;
				}
				quoted = true;
			} else if (character == ',') {
				row.push_back(std::move(field));
				field.clear();
				quoteClosed = false;
			} else if (character == '\n' || character == '\r') {
				if (character == '\r' && index + 1 < source.size() && source[index + 1] == '\n') ++index;
				row.push_back(std::move(field));
				field.clear();
				quoteClosed = false;
				rows.push_back({std::move(row), recordLine});
				row.clear();
				if (rows.size() > LocalizationCatalogue::MAXIMUM_ENTRIES + 1) {
					issue("$", "localization entry limit exceeded", recordLine);
					return false;
				}
				++line;
				recordLine = line;
			} else
				field.push_back(character);
		}
		if (quoted) {
			issue("$", "CSV quoted field is unterminated", line);
			return false;
		}
		if (!field.empty() || !row.empty()) {
			row.push_back(std::move(field));
			rows.push_back({std::move(row), recordLine});
			if (rows.size() > LocalizationCatalogue::MAXIMUM_ENTRIES + 1) {
				issue("$", "localization entry limit exceeded", recordLine);
				return false;
			}
		}
		if (rows.empty() || rows.front().Fields != std::vector<std::string>{"locale", "key", "text"}) {
			issue("$", "CSV header must be locale,key,text", 1);
			return false;
		}
		LocalizationCatalogue decoded;
		std::unordered_set<std::string> identities;
		for (size_t index = 1; index < rows.size(); ++index) {
			const Row &sourceRow = rows[index];
			const auto &fields = sourceRow.Fields;
			if (fields.size() != 3) {
				issue(
					"$.rows[" + std::to_string(index - 1) + "]",
					"CSV row must have three fields",
					sourceRow.Line
				);
				continue;
			}
			if (!validUtf8(fields[0]) || !validUtf8(fields[1]) || !validUtf8(fields[2])) {
				issue(
					"$.rows[" + std::to_string(index - 1) + "]",
					"CSV locale, key, and text must be valid UTF-8",
					sourceRow.Line
				);
				continue;
			}
			if (fields[0].empty() || fields[0].size() > LocalizationCatalogue::MAXIMUM_LOCALE_BYTES ||
				fields[1].empty() || fields[1].size() > DocumentLimits{}.MaximumStringBytes ||
				fields[2].size() > LocalizationCatalogue::MAXIMUM_MESSAGE_BYTES) {
				issue(
					"$.rows[" + std::to_string(index - 1) + "]",
					"CSV locale, key, or text exceeds catalog limits",
					sourceRow.Line
				);
				continue;
			}
			const std::string identity = fields[0] + "\n" + fields[1];
			if (!identities.insert(identity).second) {
				issue(
					"$.rows[" + std::to_string(index - 1) + "]",
					"locale and key are duplicated",
					sourceRow.Line
				);
				continue;
			}
			if (!decoded.Set({fields[0], core::Name(fields[1]), fields[2]}))
				issue(
					"$.rows[" + std::to_string(index - 1) + "]",
					"could not add localization entry",
					sourceRow.Line
				);
		}
		if (!report.Ok()) return false;
		out = std::move(decoded);
		return true;
	}

	bool CollectLocalizationTables(const ecs::Store &store, LocalizationCatalogue &out) {
		const ecs::Entity storage = store.FindFirstRoot("ReplicatedStorage");
		const ecs::ClassId table = ecs::Classes::Find(core::Name("LocalizationTable"));
		if (storage == ecs::NULL_ENTITY || !table.IsValid()) {
			out = {};
			return true;
		}
		LocalizationCatalogue combined;
		bool valid = true;
		store.EachDescendant(storage, [&](const ecs::Entity instance) {
			if (!valid || !store.IsA(instance, table)) return;
			std::string source;
			if (!store.GetProperty(instance, core::Name("Value"), &source, sizeof(source))) {
				valid = false;
				return;
			}
			LocalizationCatalogue one;
			DocumentReport report;
			if (!DecodeLocalizationCsv(source, one, report))
				valid = false;
			else
				valid = combined.Merge(one);
		});
		if (valid) out = std::move(combined);
		return valid;
	}

	const LocalizationCatalogue *LocalizationCache::Refresh(ecs::Store &store) {
		const uint64_t identity = store.Identity();
		const ecs::ComponentId text = ecs::Components::Find(core::Name("scene.TextContent"));
		const ecs::ComponentId hierarchy = ecs::Components::Find(core::Name("ecs.Hierarchy"));
		if (text.IsValid()) store.Observe(text);
		if (hierarchy.IsValid()) store.Observe(hierarchy);
		const uint64_t textRevision = text.IsValid() ? store.ComponentChangeVersion(text) : 0;
		const uint64_t hierarchyRevision = hierarchy.IsValid() ? store.ComponentChangeVersion(hierarchy) : 0;
		if (identity != StoreIdentity || textRevision != TextRevision ||
			hierarchyRevision != HierarchyRevision) {
			LocalizationCatalogue next;
			if (!CollectLocalizationTables(store, next)) next = {};
			Catalogue = std::move(next);
			StoreIdentity = identity;
			TextRevision = textRevision;
			HierarchyRevision = hierarchyRevision;
		}
		return &Catalogue;
	}
}
