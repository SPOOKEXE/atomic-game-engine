#include <docgen/Filter.hpp>

#include <vector>

namespace docgen {

	namespace {

		constexpr size_t NONE = std::string_view::npos;

		// A line and its terminator, kept apart so that rewriting the text
		// cannot turn a \r\n into a \n. A file that round-trips through the
		// filter with its endings changed shows up as a whole-file diff on
		// Windows and hides whatever actually changed.
		struct Line {
			std::string_view Text;
			std::string_view Ending;
		};

		std::vector<Line> Split(std::string_view source) {
			std::vector<Line> lines;
			size_t start = 0;
			while (start < source.size()) {
				const size_t newline = source.find('\n', start);
				if (newline == NONE) {
					lines.push_back({ source.substr(start), {} });
					break;
				}
				size_t end = newline;
				std::string_view ending = "\n";
				if (end > start && source[end - 1] == '\r') {
					end--;
					ending = "\r\n";
				}
				lines.push_back({ source.substr(start, end - start), ending });
				start = newline + 1;
			}
			return lines;
		}

		std::string_view TrimLeft(std::string_view text) {
			const size_t first = text.find_first_not_of(" \t");
			return first == NONE ? std::string_view {} : text.substr(first);
		}

		bool IsBlank(std::string_view text) {
			return text.find_first_not_of(" \t\r") == NONE;
		}

		bool IsLetter(char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}

		bool IsIdentifierChar(char c) {
			return IsLetter(c) || (c >= '0' && c <= '9') || c == '_';
		}

		// Doxygen already reads these, so a comment written with one is left
		// alone. Somebody who reached for a marker meant it.
		bool IsAlreadyMarked(std::string_view comment) {
			return comment.starts_with("///") || comment.starts_with("//!");
		}

		// A `"` preceded by `R`, or by an encoding prefix and an `R`, opens a
		// raw string. Worth the four lines: the shader tests hold GLSL in one,
		// and GLSL comments start with `//`.
		bool OpensRawString(std::string_view line, size_t quote) {
			if (quote == 0 || line[quote - 1] != 'R') {
				return false;
			}
			size_t i = quote - 1;
			while (i > 0 && (line[i - 1] == 'L' || line[i - 1] == 'u' || line[i - 1] == 'U'
							 || line[i - 1] == '8')) {
				i--;
			}
			return i == 0 || !IsIdentifierChar(line[i - 1]);
		}

		// Finds where a `//` comment starts on a line, ignoring the ones that
		// are not comments at all — inside a string, inside a character
		// literal, inside a /* */ that opened on an earlier line.
		//
		// Two of those states outlive a single line, so this is a scanner over
		// the file rather than a function of one line.
		class Scanner {
		  public:
			size_t CommentAt(std::string_view line);

		  private:
			bool InBlockComment = false;
			bool InRawString = false;
			std::string RawTerminator;
		};

		size_t Scanner::CommentAt(std::string_view line) {
			size_t i = 0;
			while (i < line.size()) {
				if (InRawString) {
					const size_t end = line.find(RawTerminator, i);
					if (end == NONE) {
						return NONE;
					}
					InRawString = false;
					i = end + RawTerminator.size();
					continue;
				}
				if (InBlockComment) {
					const size_t end = line.find("*/", i);
					if (end == NONE) {
						return NONE;
					}
					InBlockComment = false;
					i = end + 2;
					continue;
				}

				const char c = line[i];
				const char next = i + 1 < line.size() ? line[i + 1] : '\0';

				if (c == '/' && next == '/') {
					return i;
				}
				if (c == '/' && next == '*') {
					InBlockComment = true;
					i += 2;
					continue;
				}

				if (c == '"' && OpensRawString(line, i)) {
					const size_t open = line.find('(', i + 1);
					if (open == NONE) {
						// A raw string delimiter cannot contain a newline, so
						// this line is not something we understand. Reporting
						// no comment leaves it untouched, which is the safe
						// way to be wrong.
						return NONE;
					}
					RawTerminator = ")" + std::string(line.substr(i + 1, open - i - 1)) + "\"";
					const size_t end = line.find(RawTerminator, open + 1);
					if (end == NONE) {
						InRawString = true;
						return NONE;
					}
					i = end + RawTerminator.size();
					continue;
				}

				if (c == '"' || c == '\'') {
					size_t j = i + 1;
					while (j < line.size() && line[j] != c) {
						j += line[j] == '\\' ? 2 : 1;
					}
					if (j >= line.size()) {
						return NONE;
					}
					i = j + 1;
					continue;
				}

				i++;
			}
			return NONE;
		}

		// `<assets>/shaders/<module>/` is a path with two placeholders in it.
		// Doxygen reads it as two HTML tags, warns, and renders `/shaders//` —
		// the prose loses its meaning and the page does not say it happened.
		//
		// So a placeholder-shaped `<word>` is escaped on the way past. Literal
		// HTML in a comment is not supported and is not wanted; the prose in
		// this repository is prose and markdown.
		std::string EscapePlaceholders(std::string_view comment) {
			std::string result;
			result.reserve(comment.size());

			for (size_t i = 0; i < comment.size(); ++i) {
				if (comment[i] != '<') {
					result += comment[i];
					continue;
				}

				// `<word>`, where word is a bare identifier. `<engine/core>`
				// and `a < b` are not tags and Doxygen never took them for
				// tags, so leave them exactly as written.
				size_t end = i + 1;
				if (end < comment.size() && IsLetter(comment[end])) {
					end++;
					while (end < comment.size()
						   && (IsIdentifierChar(comment[end]) || comment[end] == '-')) {
						end++;
					}
				}

				const bool placeholder = end > i + 1 && end < comment.size() && comment[end] == '>';
				result += placeholder ? "&lt;" : "<";
			}
			return result;
		}

		// The line to overwrite with `/// @file`, or NONE.
		//
		// Overwriting a blank line that is already there is what keeps the line
		// count identical. There is always one beside the opening block —
		// `#pragma once`, blank, prose — and a `///` line touching the block is
		// part of the same comment, so the whole thing becomes the file's
		// documentation rather than the first declaration's.
		size_t FileCommandLine(const std::vector<Line> &lines) {
			size_t start = 0;
			while (start < lines.size()) {
				const std::string_view text = TrimLeft(lines[start].Text);
				if (text.empty() || text.starts_with("#pragma once")) {
					start++;
					continue;
				}
				break;
			}
			if (start >= lines.size()) {
				return NONE;
			}

			const std::string_view first = TrimLeft(lines[start].Text);
			if (!first.starts_with("//") || IsAlreadyMarked(first)) {
				return NONE;
			}

			size_t after = start;
			while (after < lines.size() && TrimLeft(lines[after].Text).starts_with("//")) {
				after++;
			}

			// Above the block reads better than below it, and is the case every
			// header in this repository is in.
			if (start > 0 && IsBlank(lines[start - 1].Text)) {
				return start - 1;
			}
			if (after < lines.size() && IsBlank(lines[after].Text)) {
				return after;
			}
			return NONE;
		}
	}

	std::string Promote(std::string_view source) {
		const std::vector<Line> lines = Split(source);
		const size_t fileCommand = FileCommandLine(lines);

		std::string result;
		result.reserve(source.size() + source.size() / 8);

		Scanner scanner;
		for (size_t i = 0; i < lines.size(); ++i) {
			const std::string_view text = lines[i].Text;

			// Still scanned, so that a blank line inside a raw string is not
			// mistaken for the one beside a comment block.
			const size_t comment = scanner.CommentAt(text);

			if (i == fileCommand) {
				result += "/// @file";
			} else if (comment == NONE || IsAlreadyMarked(text.substr(comment))) {
				result += text;
			} else {
				const bool ownsTheLine = IsBlank(text.substr(0, comment));
				result += text.substr(0, comment);
				// `///<` is Doxygen for "documents the thing to my left". A
				// trailing comment promoted to plain `///` would attach to the
				// *next* member instead, which documents the wrong field and
				// looks right while doing it.
				result += ownsTheLine ? "///" : "///<";
				result += EscapePlaceholders(text.substr(comment + 2));
			}

			result += lines[i].Ending;
		}
		return result;
	}
}
