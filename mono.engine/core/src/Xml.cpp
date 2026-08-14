#include <engine/core/Xml.hpp>

#include <cstddef>

namespace engine::core::xml {

	namespace {
		bool IsSpace(char character) {
			return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
				   character == '\f';
		}

		bool IsDigit(char character) {
			return character >= '0' && character <= '9';
		}

		char Lowered(char character) {
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
														: character;
		}

		// XML's name rule, narrowed. Letters, digits, underscore, hyphen, dot
		// and colon - and colon only because it is legal, not because anything
		// here means anything by it.
		//
		// **Narrowed rather than complete on purpose.** XML admits most of
		// Unicode in a name, and no format read through this scanner writes one:
		// what a wider rule would buy is a document nobody produces, and what it
		// costs is a table of code point ranges in the one file that faces
		// hostile input.
		bool IsNameStart(char character) {
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				   character == '_' || character == ':';
		}

		bool IsNameChar(char character) {
			return IsNameStart(character) || IsDigit(character) || character == '-' || character == '.';
		}

		bool IsName(std::string_view text) {
			if (text.empty() || !IsNameStart(text.front())) {
				return false;
			}
			for (const char character : text) {
				if (!IsNameChar(character)) {
					return false;
				}
			}
			return true;
		}

		bool HexDigit(char character, uint32_t &value) {
			if (IsDigit(character)) {
				value = static_cast<uint32_t>(character - '0');
				return true;
			}
			const char lowered = Lowered(character);
			if (lowered >= 'a' && lowered <= 'f') {
				value = static_cast<uint32_t>(lowered - 'a') + 10u;
				return true;
			}
			return false;
		}

		void SkipSpace(std::string_view &text) {
			while (!text.empty() && IsSpace(text.front())) {
				text.remove_prefix(1);
			}
		}

		std::string_view Trimmed(std::string_view text) {
			while (!text.empty() && IsSpace(text.front())) {
				text.remove_prefix(1);
			}
			while (!text.empty() && IsSpace(text.back())) {
				text.remove_suffix(1);
			}
			return text;
		}

		// Fills in a refusal and answers `false`, so that a caller reads
		// `return Refuse(...)` rather than three lines of assignment.
		bool Refuse(Failure &failure, const Options &options, Fault reason, std::string_view rest) {
			failure.Reason = reason;
			failure.Message = std::string(options.Format) + ": " + std::string(rest);
			return false;
		}

		// The last code point Unicode has. A character reference past it names
		// nothing, and accumulating one unchecked overflows the accumulator
		// rather than failing.
		constexpr uint32_t LAST_CODE_POINT = 0x10FFFFu;

		void AppendUtf8(uint32_t codePoint, std::string &out) {
			if (codePoint < 0x80u) {
				out.push_back(static_cast<char>(codePoint));
				return;
			}
			if (codePoint < 0x800u) {
				out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
				out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
				return;
			}
			if (codePoint < 0x10000u) {
				out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
				out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
				return;
			}
			out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
			out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
			out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
			out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
		}

		// One entity reference, `text` beginning at its `&`.
		//
		// **The five predefines and a numeric character reference, and nothing
		// else.** Every other name would have to have been declared, this refuses
		// every declaration, so a reference to one is a document that was written
		// against a parser this is not - named rather than dropped, because a
		// dropped one turns an entity bomb into what looks like a typo.
		//
		// A name that is refused is `Fault::Refused` and a numeric reference that
		// is not a number is `Fault::Malformed`: `&payload;` is somebody trying
		// something and `&#zz;` is a typo, and a caller counting the first must
		// not be handed the second.
		//
		// @param[out] out    Appended to when not null. A caller that only wants
		//                    the refusal passes nothing and copies nothing.
		// @param[out] length What to step over, the `&` and the `;` included.
		bool ReadEntity(
			std::string_view text, const Options &options, std::string *out, size_t &length, Failure &failure
		) {
			const size_t semicolon = text.find(';');

			// Nine characters is past every name this admits - `#x10FFFF` is the
			// longest, at eight - and the bound is what stops a lone `&` in a
			// document from swallowing the rest of it looking for a terminator.
			if (semicolon == std::string_view::npos || semicolon > 10) {
				const bool numeric = text.size() > 1 && text[1] == '#';
				return Refuse(
					failure,
					options,
					numeric ? Fault::Malformed : Fault::Refused,
					"an unterminated entity reference"
				);
			}

			length = semicolon + 1;
			const std::string_view name = text.substr(1, semicolon - 1);

			const auto predefined = [&](std::string_view spelling, char character) {
				if (name != spelling) {
					return false;
				}
				if (out != nullptr) {
					out->push_back(character);
				}
				return true;
			};

			if (predefined("lt", '<') || predefined("gt", '>') || predefined("amp", '&') ||
				predefined("quot", '"') || predefined("apos", '\'')) {
				return true;
			}

			// `&#48;` and `&#x30;`, which expand to exactly one character each and
			// so cannot be a bomb however many of them there are.
			if (!name.empty() && name.front() == '#') {
				const bool hexadecimal = name.size() > 2 && Lowered(name[1]) == 'x';
				const size_t start = hexadecimal ? 2u : 1u;

				uint32_t codePoint = 0;
				bool numeric = start < name.size();
				for (size_t index = start; numeric && index < name.size(); index++) {
					uint32_t digit = 0;
					numeric = hexadecimal ? HexDigit(name[index], digit) : IsDigit(name[index]);
					if (!numeric) {
						break;
					}
					if (!hexadecimal) {
						digit = static_cast<uint32_t>(name[index] - '0');
					}
					codePoint = codePoint * (hexadecimal ? 16u : 10u) + digit;
					if (codePoint > LAST_CODE_POINT) {
						return Refuse(
							failure,
							options,
							Fault::Malformed,
							"character reference '&" + std::string(name) + ";' is past the last code point"
						);
					}
				}

				if (numeric) {
					if (out != nullptr) {
						AppendUtf8(codePoint, *out);
					}
					return true;
				}

				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"character reference '&" + std::string(name) + ";' is not a number"
				);
			}

			return Refuse(
				failure,
				options,
				Fault::Refused,
				"entity reference '&" + std::string(name) +
					";' - only the five predefined entities are read, because anything else would need a "
					"declaration this refuses"
			);
		}
	}

	std::string_view WithoutPrefix(std::string_view name) {
		const size_t colon = name.find(':');
		return colon == std::string_view::npos ? name : name.substr(colon + 1);
	}

	Scan NextTag(std::string_view &text, const Options &options, Tag &tag, Failure &failure) {
		while (true) {
			const size_t open = text.find('<');
			if (open == std::string_view::npos) {
				text = {};
				return Scan::End;
			}
			text.remove_prefix(open + 1);

			if (text.starts_with("!--")) {
				const size_t end = text.find("-->");
				if (end == std::string_view::npos) {
					Refuse(failure, options, Fault::Truncated, "a comment is never closed");
					return Scan::Error;
				}
				text.remove_prefix(end + 3);
				continue;
			}

			// **The one `<!` that is not a declaration.** A CDATA section is
			// character data wearing markup's punctuation - it declares nothing
			// and expands to nothing - so it is stepped over here and captured by
			// `ReadContent` where a caller actually wants the text.
			if (text.starts_with("![CDATA[")) {
				const size_t end = text.find("]]>");
				if (end == std::string_view::npos) {
					Refuse(failure, options, Fault::Truncated, "a CDATA section is never closed");
					return Scan::Error;
				}
				text.remove_prefix(end + 3);
				continue;
			}

			if (text.starts_with("!")) {
				// **DOCTYPE and ENTITY, refused outright rather than bounded.**
				// An entity declaration is the billion-laughs expansion - a
				// kilobyte of markup that unfolds into gigabytes - and an external
				// one is a file read performed by a parser that has no business
				// touching a filesystem. No format read through here needs either.
				Refuse(
					failure,
					options,
					Fault::Refused,
					"a DOCTYPE or ENTITY declaration is refused - entity expansion is how a kilobyte of "
					"XML becomes a gigabyte, and an external entity is a file read"
				);
				return Scan::Error;
			}

			if (text.starts_with("?")) {
				const size_t end = text.find("?>");
				if (end == std::string_view::npos) {
					Refuse(failure, options, Fault::Truncated, "an XML declaration is never closed");
					return Scan::Error;
				}
				text.remove_prefix(end + 2);
				continue;
			}

			break;
		}

		tag = {};
		if (text.starts_with("/")) {
			tag.Closing = true;
			text.remove_prefix(1);
		}

		size_t nameEnd = 0;
		while (nameEnd < text.size() && !IsSpace(text[nameEnd]) && text[nameEnd] != '>' &&
			   text[nameEnd] != '/') {
			nameEnd++;
		}
		if (nameEnd == 0) {
			Refuse(failure, options, Fault::Malformed, "a tag has no name");
			return Scan::Error;
		}

		const std::string_view name = text.substr(0, nameEnd);
		if (!IsName(name)) {
			Refuse(failure, options, Fault::Malformed, "'" + std::string(name) + "' is not an element name");
			return Scan::Error;
		}
		tag.Name = options.DropNamespacePrefix ? WithoutPrefix(name) : name;
		text.remove_prefix(nameEnd);

		// The attribute run, ending at the first `>` that is not inside a quoted
		// value - a `>` in an attribute is legal XML and ending the tag on it
		// would read the rest of the document as character data.
		size_t index = 0;
		char quote = 0;
		while (index < text.size()) {
			const char character = text[index];
			if (quote != 0) {
				if (character == quote) {
					quote = 0;
				}
			} else if (character == '"' || character == '\'') {
				quote = character;
			} else if (character == '>') {
				break;
			}
			index++;
		}
		if (index >= text.size()) {
			Refuse(failure, options, Fault::Truncated, "a tag is never closed");
			return Scan::Error;
		}

		std::string_view attributes = text.substr(0, index);
		text.remove_prefix(index + 1);

		attributes = Trimmed(attributes);
		if (!attributes.empty() && attributes.back() == '/') {
			tag.SelfClosing = true;
			attributes.remove_suffix(1);
		}

		tag.Attributes = attributes;
		return Scan::Tag;
	}

	bool ReadAttributes(
		std::string_view text, const Options &options, std::vector<Attribute> &out, Failure &failure
	) {
		out.clear();

		while (true) {
			SkipSpace(text);
			if (text.empty()) {
				return true;
			}
			if (out.size() >= options.MaximumAttributes) {
				return Refuse(
					failure,
					options,
					Fault::TooManyAttributes,
					"an element carries more than " + std::to_string(options.MaximumAttributes) +
						" attributes"
				);
			}

			const size_t equals = text.find('=');
			if (equals == std::string_view::npos) {
				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"attribute '" + std::string(Trimmed(text)) + "' has no value"
				);
			}

			Attribute attribute;
			attribute.Name = Trimmed(text.substr(0, equals));
			text.remove_prefix(equals + 1);
			SkipSpace(text);

			if (!IsName(attribute.Name)) {
				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"'" + std::string(attribute.Name) + "' is not an attribute name"
				);
			}

			if (text.empty() || (text.front() != '"' && text.front() != '\'')) {
				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"attribute '" + std::string(attribute.Name) + "' has an unquoted value"
				);
			}

			const char quote = text.front();
			text.remove_prefix(1);
			const size_t end = text.find(quote);
			if (end == std::string_view::npos) {
				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"attribute '" + std::string(attribute.Name) + "' is never closed"
				);
			}

			attribute.Value = text.substr(0, end);
			text.remove_prefix(end + 1);

			// A `<` in a value is not legal XML, and a scanner that allowed one
			// would read a document whose markup is ambiguous to everything else.
			if (attribute.Value.find('<') != std::string_view::npos) {
				return Refuse(
					failure,
					options,
					Fault::Malformed,
					"attribute '" + std::string(attribute.Name) + "' has a '<' in its value"
				);
			}

			out.push_back(attribute);
		}
	}

	const Attribute *Find(const std::vector<Attribute> &attributes, std::string_view name) {
		for (const Attribute &attribute : attributes) {
			if (attribute.Name == name) {
				return &attribute;
			}
		}
		return nullptr;
	}

	bool CheckEntityReferences(std::string_view text, const Options &options, Failure &failure) {
		size_t position = 0;
		while (true) {
			position = text.find('&', position);
			if (position == std::string_view::npos) {
				return true;
			}

			size_t length = 0;
			if (!ReadEntity(text.substr(position), options, nullptr, length, failure)) {
				return false;
			}
			position += length;
		}
	}

	bool Unescape(std::string_view text, const Options &options, std::string &out, Failure &failure) {
		while (true) {
			const size_t ampersand = text.find('&');
			if (ampersand == std::string_view::npos) {
				out.append(text);
				return true;
			}

			out.append(text.substr(0, ampersand));
			text.remove_prefix(ampersand);

			size_t length = 0;
			if (!ReadEntity(text, options, &out, length, failure)) {
				return false;
			}
			text.remove_prefix(length);
		}
	}

	bool ReadContent(std::string_view &text, const Options &options, std::string &out, Failure &failure) {
		out.clear();

		while (true) {
			const size_t open = text.find('<');
			if (open == std::string_view::npos) {
				const bool unescaped = Unescape(text, options, out, failure);
				text = {};
				return unescaped;
			}

			if (!Unescape(text.substr(0, open), options, out, failure)) {
				return false;
			}
			text.remove_prefix(open);

			if (!text.starts_with("<![CDATA[")) {
				// Left on the `<`, which is what the next `NextTag` looks for.
				return true;
			}

			const size_t end = text.find("]]>");
			if (end == std::string_view::npos) {
				return Refuse(failure, options, Fault::Truncated, "a CDATA section is never closed");
			}

			// **Verbatim, escapes included.** A CDATA section is how a script's
			// source is written, and a `&` in Luau is an operator rather than the
			// start of anything.
			constexpr size_t OPENING = 9;
			out.append(text.substr(OPENING, end - OPENING));
			text.remove_prefix(end + 3);
		}
	}
}
