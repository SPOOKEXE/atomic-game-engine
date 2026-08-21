#pragma once

// The locale-independent floating-point parse, on its own so that it is tested
// on every platform rather than only on the one that needs it.
//
// **Otherwise this is dead code wherever it works.** `Chars.cpp` uses
// `std::from_chars` where the library has it, which is everywhere the engine is
// developed - so the branch that Apple's libc++ actually takes would compile
// here, run nowhere here, and be discovered wrong by the one machine that has
// no local toolchain to debug it on. A module's own tests may reach its `src/`
// directory (`mono_add_tests` adds it to the include path, and
// `MonoLibrary.cmake` says why), so `tests/Chars.cpp` includes this and checks
// both paths against each other.
//
// Header-only and `inline` because there is one caller in the library and one in
// the suite, and a second translation unit for four functions is not worth the
// build file.

#include <charconv>
#include <cstddef>
#include <ios>
#include <locale>
#include <sstream>
#include <string>

namespace engine::core::portable {

	inline bool Digit(char c) {
		return c >= '0' && c <= '9';
	}

	// The longest prefix of `[first, last)` that `std::from_chars` would have
	// consumed under `chars_format::general`.
	//
	// **Scanned here rather than left to the stream**, because the whole
	// contract of `from_chars_result::ptr` is where parsing stopped, and a
	// stream's `tellg` after a partial extraction is not that. Callers test
	// `result.ptr != last` to mean "there was trailing rubbish", so getting this
	// wrong would accept `1.5kg` as `1.5`.
	//
	// Deliberately no leading `+` and no leading whitespace: `from_chars`
	// accepts neither, and the two paths have to agree or the fallback is a
	// second set of rules nobody wrote down.
	inline const char *Extent(const char *first, const char *last) {
		const char *at = first;
		if (at != last && *at == '-') {
			at++;
		}

		const char *digits = at;
		while (at != last && Digit(*at)) {
			at++;
		}
		bool any = at != digits;

		if (at != last && *at == '.') {
			at++;
			const char *fraction = at;
			while (at != last && Digit(*at)) {
				at++;
			}
			any = any || at != fraction;
		}

		// No mantissa means no number, whatever follows it.
		if (!any) {
			return first;
		}

		// An exponent counts only with at least one digit behind it. `1e` is the
		// number one followed by a letter, and `ptr` has to say so.
		if (at != last && (*at == 'e' || *at == 'E')) {
			const char *mark = at;
			at++;
			if (at != last && (*at == '+' || *at == '-')) {
				at++;
			}
			const char *power = at;
			while (at != last && Digit(*at)) {
				at++;
			}
			if (at == power) {
				at = mark;
			}
		}
		return at;
	}

	// Reads one number out of the prefix `Extent` found.
	template <class T> std::from_chars_result Read(const char *first, const char *last, T &value) {
		const char *end = Extent(first, last);
		if (end == first) {
			return {first, std::errc::invalid_argument};
		}

		// **Imbued with the classic locale, which is the entire point.** A
		// default-constructed stream carries the global locale, and that is the
		// thing every caller reached for `from_chars` to escape.
		std::istringstream stream(std::string(first, static_cast<size_t>(end - first)));
		stream.imbue(std::locale::classic());

		T parsed{};
		stream >> parsed;

		// Overflow saturates and sets failbit; reported the way `from_chars`
		// reports it, with `ptr` still past the number that was there.
		if (stream.fail() && !stream.eof()) {
			return {end, std::errc::result_out_of_range};
		}

		value = parsed;
		return {end, std::errc{}};
	}
}
