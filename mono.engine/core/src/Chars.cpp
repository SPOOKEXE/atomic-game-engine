#include "PortableChars.hpp"

#include <engine/core/Chars.hpp>

// The probe, and it is the standard's own. `__cpp_lib_to_chars` is defined only
// once a library has *both* halves of `<charconv>`; libc++ ships the integer
// half and leaves this undefined, which is exactly the distinction that matters.
// Testing the compiler or the platform instead would be guessing at which
// libraries a compiler might be paired with.
#if defined(__cpp_lib_to_chars)
#define MONO_HAS_FLOAT_FROM_CHARS 1
#else
#define MONO_HAS_FLOAT_FROM_CHARS 0
#endif

namespace engine::core {

	std::from_chars_result FromChars(const char *first, const char *last, float &value) {
#if MONO_HAS_FLOAT_FROM_CHARS
		return std::from_chars(first, last, value);
#else
		return portable::Read(first, last, value);
#endif
	}

	std::from_chars_result FromChars(const char *first, const char *last, double &value) {
#if MONO_HAS_FLOAT_FROM_CHARS
		return std::from_chars(first, last, value);
#else
		return portable::Read(first, last, value);
#endif
	}
}
