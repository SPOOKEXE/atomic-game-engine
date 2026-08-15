#include <linecount/Counter.hpp>
#include <string>

namespace linecount {

	namespace {

		constexpr size_t NONE = std::string_view::npos;

		bool IsSpace(char c) {
			return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
		}

		bool HasContent(std::string_view text) {
			for (const char c : text) {
				if (!IsSpace(c)) {
					return true;
				}
			}
			return false;
		}

		bool IsIdentifierChar(char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		}

		// A `"` preceded by `R`, or by an encoding prefix and an `R`, opens a raw
		// string. Worth the six lines for the same reason docgen carries them:
		// the shader tests hold GLSL in one, and GLSL comments start with `//`.
		bool OpensRawString(std::string_view line, size_t quote) {
			if (quote == 0 || line[quote - 1] != 'R') {
				return false;
			}
			size_t start = quote - 1;
			while (start > 0 && (line[start - 1] == 'L' || line[start - 1] == 'u' || line[start - 1] == 'U' ||
								 line[start - 1] == '8')) {
				start--;
			}
			return start == 0 || !IsIdentifierChar(line[start - 1]);
		}

		// What one line contained, before the whitespace-only rule is applied.
		struct Content {
			bool Code = false;
			bool Comment = false;
		};

		// Walks a file a line at a time, carrying the state that spans lines.
		//
		// Only two things do span a line - an open `/* */` and an unterminated
		// raw string - and both of them make the *next* line mean something
		// other than what it looks like on its own. That is the whole reason
		// this is an object rather than a function.
		class Scanner {
		  public:
			Content Scan(std::string_view line);

		  private:
			bool InBlockComment = false;
			bool InRawString = false;
			std::string RawTerminator;
		};

		Content Scanner::Scan(std::string_view line) {
			Content content;

			size_t i = 0;
			while (i < line.size()) {
				if (InRawString) {
					const size_t end = line.find(RawTerminator, i);
					if (end == NONE) {
						// The rest of the line is string body. Text, not
						// comment text, whatever it happens to look like.
						content.Code = content.Code || HasContent(line.substr(i));
						return content;
					}
					content.Code = true;
					InRawString = false;
					i = end + RawTerminator.size();
					continue;
				}

				if (InBlockComment) {
					const size_t end = line.find("*/", i);
					if (end == NONE) {
						content.Comment = content.Comment || HasContent(line.substr(i));
						return content;
					}
					content.Comment = true;
					InBlockComment = false;
					i = end + 2;
					continue;
				}

				const char c = line[i];
				const char next = i + 1 < line.size() ? line[i + 1] : '\0';

				if (c == '/' && next == '/') {
					content.Comment = true;
					return content;
				}
				if (c == '/' && next == '*') {
					content.Comment = true;
					InBlockComment = true;
					i += 2;
					continue;
				}

				if (c == '"' && OpensRawString(line, i)) {
					content.Code = true;
					const size_t open = line.find('(', i + 1);
					if (open == NONE) {
						// A raw string's delimiter cannot contain a newline, so
						// this line is not something we understand. Calling the
						// remainder code is the safe way to be wrong: it never
						// swallows the rest of the file into a string state
						// that has no terminator to leave by.
						return content;
					}
					RawTerminator = ")" + std::string(line.substr(i + 1, open - i - 1)) + "\"";
					const size_t end = line.find(RawTerminator, open + 1);
					if (end == NONE) {
						InRawString = true;
						return content;
					}
					i = end + RawTerminator.size();
					continue;
				}

				if (c == '"' || c == '\'') {
					content.Code = true;
					size_t j = i + 1;
					while (j < line.size() && line[j] != c) {
						j += line[j] == '\\' ? 2 : 1;
					}
					if (j >= line.size()) {
						return content;
					}
					i = j + 1;
					continue;
				}

				if (!IsSpace(c)) {
					content.Code = true;
				}
				i++;
			}

			return content;
		}
	}

	std::vector<LineKind> Classify(std::string_view source) {
		std::vector<LineKind> kinds;

		Scanner scanner;
		size_t start = 0;
		while (start < source.size()) {
			const size_t newline = source.find('\n', start);
			const std::string_view line =
				newline == NONE ? source.substr(start) : source.substr(start, newline - start);

			// Scanned before the blank test rather than instead of it, because
			// the state a line leaves behind matters even when the line itself
			// is nothing - and a whitespace-only line cannot hold `*/`, `)"` or
			// a quote, so scanning one never changes that state anyway.
			const Content content = scanner.Scan(line);

			if (!HasContent(line)) {
				kinds.push_back(LineKind::Empty);
			} else if (content.Code) {
				kinds.push_back(LineKind::Code);
			} else if (content.Comment) {
				kinds.push_back(LineKind::Comment);
			} else {
				// Content with neither flag set is unreachable: anything that
				// is not whitespace is one or the other. Kept rather than
				// asserted so a future rule that misses a case counts the line
				// somewhere instead of dropping it out of the total.
				kinds.push_back(LineKind::Code);
			}

			if (newline == NONE) {
				break;
			}
			start = newline + 1;
		}

		return kinds;
	}

	Counts Count(std::string_view source) {
		Counts counts;
		for (const LineKind kind : Classify(source)) {
			switch (kind) {
			case LineKind::Empty:
				counts.Empty++;
				break;
			case LineKind::Comment:
				counts.Comment++;
				break;
			case LineKind::Code:
				counts.Code++;
				break;
			}
		}
		return counts;
	}
}
