#include <engine/core/Chars.hpp>
#include <engine/graphcore/Document.hpp>

#include <array>
#include <charconv>

namespace engine::graphcore::text {

	void AppendQuoted(std::string &out, std::string_view text) {
		out.push_back('"');
		for (const char character : text) {
			switch (character) {
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out.push_back(character);
				break;
			}
		}
		out.push_back('"');
	}

	bool TakeQuoted(std::string_view &line, std::string &out) {
		while (!line.empty() && line.front() == ' ') {
			line.remove_prefix(1);
		}
		if (line.empty() || line.front() != '"') {
			return false;
		}
		line.remove_prefix(1);
		out.clear();
		while (!line.empty()) {
			const char character = line.front();
			line.remove_prefix(1);
			if (character == '"') {
				return true;
			}
			if (character != '\\') {
				out.push_back(character);
				continue;
			}
			if (line.empty()) {
				return false;
			}
			const char escaped = line.front();
			line.remove_prefix(1);
			switch (escaped) {
			case '\\':
				out.push_back('\\');
				break;
			case '"':
				out.push_back('"');
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			default:
				return false;
			}
		}
		// Ran off the end with the quote still open.
		return false;
	}

	std::string_view TakeWord(std::string_view &line) {
		while (!line.empty() && line.front() == ' ') {
			line.remove_prefix(1);
		}
		const size_t end = line.find(' ');
		const std::string_view word = line.substr(0, end);
		line.remove_prefix(end == std::string_view::npos ? line.size() : end);
		return word;
	}

	namespace {
		template <typename T> bool TakeWhole(std::string_view &line, T &out) {
			const std::string_view word = TakeWord(line);
			if (word.empty()) {
				return false;
			}
			const char *first = word.data();
			const char *last = first + word.size();
			const auto result = std::from_chars(first, last, out);
			return result.ec == std::errc{} && result.ptr == last;
		}
	}

	bool TakeUnsigned(std::string_view &line, uint32_t &out) {
		return TakeWhole(line, out);
	}

	bool TakeInteger(std::string_view &line, int32_t &out) {
		return TakeWhole(line, out);
	}

	bool TakeFloat(std::string_view &line, float &out) {
		const std::string_view word = TakeWord(line);
		if (word.empty()) {
			return false;
		}
		const char *first = word.data();
		const char *last = first + word.size();
		const auto result = core::FromChars(first, last, out);
		return result.ec == std::errc{} && result.ptr == last;
	}

	void AppendFloat(std::string &out, float value) {
		std::array<char, 32> buffer{};
		const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
		out.append(buffer.data(), result.ptr);
	}

	bool TakeFlag(std::string_view &line, bool &out) {
		const std::string_view word = TakeWord(line);
		if (word == "yes" || word == "no") {
			out = word == "yes";
			return true;
		}
		return false;
	}

	std::string_view FlagText(bool value) {
		return value ? "yes" : "no";
	}

	bool AtEnd(std::string_view line) {
		return TakeWord(line).empty();
	}

	std::string_view NextLine(std::string_view &text) {
		const size_t end = text.find('\n');
		std::string_view line = text.substr(0, end);
		text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		return line;
	}

	int TakeHeader(std::string_view &text, std::span<const std::string_view> accepted) {
		if (text.empty()) {
			return -1;
		}
		const std::string_view header = NextLine(text);
		for (size_t index = 0; index < accepted.size(); ++index) {
			if (header == accepted[index]) {
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	void AppendSection(std::string &out, std::string_view name) {
		out += SECTION;
		out.push_back(' ');
		AppendQuoted(out, name);
		out.push_back('\n');
	}

	bool ReadSections(
		std::string_view text,
		const std::function<void(std::string_view name)> &open,
		const std::function<bool(std::string_view line)> &line,
		std::string &offender
	) {
		bool opened = false;
		while (!text.empty()) {
			const std::string_view whole = NextLine(text);
			if (whole.empty()) {
				continue;
			}
			std::string_view rest = whole;
			if (TakeWord(rest) == SECTION) {
				std::string name;
				if (!TakeQuoted(rest, name) || !AtEnd(rest) || name.empty()) {
					offender = std::string(whole);
					return false;
				}
				open(name);
				opened = true;
				continue;
			}
			// A line belonging to no section is a shape a set cannot write back.
			if (!opened || !line(whole)) {
				offender = std::string(whole);
				return false;
			}
		}
		return true;
	}
}
