#pragma once

// `std::from_chars` for floating point, on a standard library that has not
// implemented one.
//
// **This exists for one compiler and is not a style preference.** Every caller
// in this engine reaches for `std::from_chars` deliberately: `bake/Obj.cpp` and
// `bake/RobloxModelXml.cpp` both say so in a comment, and the reason is that
// `strtof` and `stod` read the decimal separator from the global locale. A
// machine set to a locale that writes `3,14` parses `3.14` as `3` and puts a
// vertex, a rotation or a transparency somewhere else - silently, on somebody
// else's computer, in a file that looks right on the machine that wrote it.
//
// `<charconv>` is the standard's locale-independent answer, and the integer half
// of it is everywhere. The floating-point half is not: libc++ as Apple ships it
// declares the overloads and defines none, so a call is `error: call to deleted
// function 'from_chars'` - which is what stopped the macOS release build.
//
// So this is the one seam. Where the library has floating-point `from_chars`,
// this *is* `std::from_chars` and costs nothing. Where it does not, it scans the
// number itself and reads it through a stream imbued with `std::locale::classic()`,
// which is verbose and slower and, crucially, still locale-independent.
//
// **Known differences on the fallback path**, all of them narrowing:
//
// - No hex significands. `std::chars_format::general` is what this implements,
//   which is what every caller here passes by taking the default.
// - No leading `+`, and no leading whitespace. `from_chars` rejects both, and
//   the scan below rejects both, so the two agree - a stream on its own would
//   have accepted them.
//
// Integers are deliberately absent. `std::from_chars` handles those everywhere,
// and an overload here would be a second spelling of a working thing.

#include <charconv>

namespace engine::core {

	// Reads a `float` from `[first, last)`, without consulting the locale.
	//
	// @param first Start of the text.
	// @param last  One past its end.
	// @param value Set only on success.
	// @return `ptr` at the first character not consumed and `ec` empty on
	//         success; `ec` of `invalid_argument` with `ptr` at `first` when
	//         nothing there is a number, and `result_out_of_range` when one is
	//         and it does not fit.
	std::from_chars_result FromChars(const char *first, const char *last, float &value);

	// Reads a `double`. As above.
	std::from_chars_result FromChars(const char *first, const char *last, double &value);
}
