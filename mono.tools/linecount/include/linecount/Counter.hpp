#pragma once

// How much of a C++ source file is code, and how much is everything else.
//
// Three numbers per file — empty lines, comment lines, code lines — over a
// classification that is a scan of the whole file rather than a test applied to
// each line on its own. Two of the states a line can be in outlive the line: a
// `/* */` that opened earlier, and a raw string literal that has not closed. A
// per-line regex gets both wrong and reports a plausible number while doing it.
//
//     Counts counts = Count(source);
//     counts.Code + counts.Comment + counts.Empty == counts.Total()
//
// The classification is a judgement, and the rules are written down on
// LineKind because a number nobody can reproduce is a number nobody can argue
// with.
//
// @tier L0 · shared

#include <cstddef>
#include <string_view>
#include <vector>

namespace linecount {

	// What one line of a translation unit is.
	//
	// The rules, in the order they are applied:
	//
	// - A line holding nothing but whitespace is `Empty` — including one inside
	//   a block comment or a raw string. A blank line is a blank line wherever
	//   it appears, and counting the ones inside comment blocks as comments
	//   inflates the comment share by the paragraph breaks in it.
	// - A line holding any code outside a comment is `Code`, even when a
	//   comment follows it. `int count = 0;  // why` is a line of code that was
	//   explained, not half a line of each.
	// - Anything else with content on it is `Comment`.
	//
	// Text inside a raw string literal is `Code`, never `Comment`. The shader
	// tests hold GLSL in `R"(...)"` and GLSL comments start with `//`; those are
	// comments in another language, and this counts C++.
	enum class LineKind {
		Empty,	 // Whitespace, and nothing else.
		Comment, // Comment text, and no code.
		Code	 // Code, with or without a comment after it.
	};

	// The three totals, and nothing derived that a caller could get wrong.
	struct Counts {
		// Lines holding nothing but whitespace.
		size_t Empty = 0;
		// Lines holding comment text and no code.
		size_t Comment = 0;
		// Lines holding code, with or without a comment after it.
		size_t Code = 0;

		// Every line in the file. The three fields partition it, so this is
		// their sum and never a separate count that could disagree.
		size_t Total() const {
			return Empty + Comment + Code;
		}

		// Adds another file's totals into this one.
		Counts &operator+=(const Counts &other) {
			Empty += other.Empty;
			Comment += other.Comment;
			Code += other.Code;
			return *this;
		}
	};

	// The totals for one translation unit.
	//
	// A file is as many lines as it has newlines, plus one when it does not end
	// on a newline. An empty file is zero lines rather than one — a file with
	// nothing in it has no blank line in it either.
	//
	// @param source The whole file, exactly as it is on disk.
	// @return The three totals, summing to the file's line count.
	Counts Count(std::string_view source);

	// What each line of a translation unit is, in order.
	//
	// The same walk `Count` does, without the tallying. Here because a
	// disagreement about one line is answered by looking at that line, and a
	// test that can only see three totals cannot say which line moved.
	std::vector<LineKind> Classify(std::string_view source);
}
