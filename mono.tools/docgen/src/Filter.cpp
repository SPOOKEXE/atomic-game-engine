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
					lines.push_back({source.substr(start), {}});
					break;
				}
				size_t end = newline;
				std::string_view ending = "\n";
				if (end > start && source[end - 1] == '\r') {
					end--;
					ending = "\r\n";
				}
				lines.push_back({source.substr(start, end - start), ending});
				start = newline + 1;
			}
			return lines;
		}

		std::string_view TrimLeft(std::string_view text) {
			const size_t first = text.find_first_not_of(" \t");
			return first == NONE ? std::string_view{} : text.substr(first);
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
			while (i > 0 &&
				   (line[i - 1] == 'L' || line[i - 1] == 'u' || line[i - 1] == 'U' || line[i - 1] == '8')) {
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
					while (end < comment.size() && (IsIdentifierChar(comment[end]) || comment[end] == '-')) {
						end++;
					}
				}

				const bool placeholder = end > i + 1 && end < comment.size() && comment[end] == '>';
				result += placeholder ? "&lt;" : "<";
			}
			return result;
		}

		// Where the `**` emphasis markers are in one comment body.
		//
		// **Skipping the ones inside a code span is the whole reason this is a
		// scan rather than a `find`.** `` `a ** b` `` is code, and turning it
		// into a tag would rewrite the thing the backticks exist to quote.
		// Backtick state is per line: a code span in this repository's prose
		// never crosses one, and assuming otherwise would let a single stray
		// backtick swallow the rest of a block.
		std::vector<size_t> BoldMarkers(std::string_view body) {
			std::vector<size_t> found;
			bool code = false;
			size_t i = 0;
			while (i < body.size()) {
				if (body[i] == '`') {
					code = !code;
					i++;
					continue;
				}
				if (!code && body[i] == '*' && i + 1 < body.size() && body[i + 1] == '*') {
					found.push_back(i);
					i += 2;
					continue;
				}
				i++;
			}
			return found;
		}

		bool IsSentenceStop(char c) {
			return c == '.' || c == '?' || c == '!';
		}

		// Moves a sentence's full stop out of the emphasis that ends with it.
		//
		// `**Sentence.**` becomes `**Sentence**.`, and that one character is the
		// whole of it.
		//
		// **`JAVADOC_AUTOBRIEF` splits the brief at the first sentence-ending
		// stop, and it does not care that the stop is inside emphasis.** This
		// repository's house style is a bold *sentence* — `**Twenty-eight and
		// not thirty-two.**` — so the split lands between the `**` and its
		// partner: the brief ends holding an unclosed emphasis and the detailed
		// description starts with a stranded closing one. Doxygen reports `end of
		// comment block while expecting </strong>` **against the following
		// comment block**, which is why the warning never points at the comment
		// that caused it.
		//
		// **Emphasis spanning a line break is fine and was the wrong suspect.**
		// A whole afternoon went on it: the first minimal reproduction happened
		// to keep the stop inside the bold while dashes, quotes and wrapping were
		// varied around it, so every variant failed and the wrapping took the
		// blame. Moving the stop out fixes a bold spanning three lines; keeping
		// it in breaks one that fits on half of one.
		//
		// **A character moved rather than lines joined**, because the line count
		// is this filter's load-bearing invariant — Doxygen numbers the source
		// listing from the filtered text and the browser links from the original.
		// This edits within a line and changes no line's existence.
		//
		// **An odd marker leaves the block alone.** A comment with an unpaired
		// `**` — prose about a pointer-to-pointer, a literal asterisk pair —
		// would otherwise have its punctuation shuffled on a boundary that is not
		// emphasis at all.
		void MoveSentenceStops(std::vector<std::string> &bodies, size_t first, size_t last) {
			size_t total = 0;
			for (size_t i = first; i <= last; ++i) {
				total += BoldMarkers(bodies[i]).size();
			}
			if (total == 0 || total % 2 != 0) {
				return;
			}

			size_t seen = 0;
			for (size_t i = first; i <= last; ++i) {
				const std::vector<size_t> markers = BoldMarkers(bodies[i]);
				if (markers.empty()) {
					seen += markers.size();
					continue;
				}

				std::string &body = bodies[i];

				// Right to left, so an edit cannot move a marker this loop has
				// not reached yet.
				for (size_t index = markers.size(); index-- > 0;) {
					// Whether this marker closes depends on how many came before
					// it in the *block*, which is what makes the pairing a block
					// question rather than a line one.
					size_t before = seen;
					for (size_t earlier = 0; earlier < index; ++earlier) {
						before++;
					}
					if (before % 2 == 0) {
						continue;
					}

					const size_t marker = markers[index];
					size_t stop = marker;
					while (stop > 0 && IsSentenceStop(body[stop - 1])) {
						stop--;
					}
					if (stop == marker) {
						continue;
					}

					const std::string moved = body.substr(stop, marker - stop);
					body.erase(stop, marker - stop);
					body.insert(stop + 2, moved);
				}

				seen += markers.size();
			}
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

		// **Where every comment starts, worked out first and once.** `Scanner`
		// carries state across lines — a block comment and a raw string both
		// outlive one — so it must see every line exactly once and in order.
		// That is also why the emphasis rewrite below cannot simply ask again.
		std::vector<size_t> comments(lines.size(), NONE);
		{
			Scanner scanner;
			for (size_t i = 0; i < lines.size(); ++i) {
				comments[i] = scanner.CommentAt(lines[i].Text);
			}
		}

		// Whether each line is a comment this filter promotes, and what its body
		// becomes. Escaping happens here so that `MoveSentenceStops` measures
		// offsets in the text that is actually emitted: `EscapePlaceholders`
		// lengthens `<word>` to `&lt;word>`, so a marker position taken before it
		// ran would name the wrong character afterwards.
		std::vector<std::string> bodies(lines.size());
		std::vector<bool> owned(lines.size(), false);
		std::vector<bool> promoted(lines.size(), false);

		for (size_t i = 0; i < lines.size(); ++i) {
			const std::string_view text = lines[i].Text;
			if (i == fileCommand || comments[i] == NONE || IsAlreadyMarked(text.substr(comments[i]))) {
				continue;
			}
			promoted[i] = true;
			owned[i] = IsBlank(text.substr(0, comments[i]));
			bodies[i] = EscapePlaceholders(text.substr(comments[i] + 2));
		}

		// **Emphasis is a property of a block, not of a line**, so the runs are
		// found before anything is rewritten. Only own-the-line comments group:
		// a trailing `///<` documents the thing to its left and is a comment of
		// one line, so pairing across two of them would join two members' prose.
		for (size_t i = 0; i < lines.size();) {
			if (!promoted[i] || !owned[i]) {
				i++;
				continue;
			}
			size_t last = i;
			while (last + 1 < lines.size() && promoted[last + 1] && owned[last + 1]) {
				last++;
			}
			MoveSentenceStops(bodies, i, last);
			i = last + 1;
		}

		std::string result;
		result.reserve(source.size() + source.size() / 8);

		for (size_t i = 0; i < lines.size(); ++i) {
			const std::string_view text = lines[i].Text;

			if (i == fileCommand) {
				result += "/// @file";
			} else if (!promoted[i]) {
				result += text;
			} else {
				result += text.substr(0, comments[i]);
				// `///<` is Doxygen for "documents the thing to my left". A
				// trailing comment promoted to plain `///` would attach to the
				// *next* member instead, which documents the wrong field and
				// looks right while doing it.
				result += owned[i] ? "///" : "///<";
				result += bodies[i];
			}

			result += lines[i].Ending;
		}
		return result;
	}
}
