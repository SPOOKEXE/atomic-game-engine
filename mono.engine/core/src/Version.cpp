#include <engine/core/Version.hpp>

// Defined by mono.engine/core/CMakeLists.txt, on this one source file rather
// than on the target.
//
// **The narrow scope is the point.** Every module in the engine links `core`,
// so a definition on the target would put the version in every translation unit
// that includes any core header, and bumping it would recompile the engine to
// change one string. Scoped here, a bump rebuilds this file and relinks.
#ifndef MONO_VERSION
#error "MONO_VERSION is not defined. See mono.engine/core/CMakeLists.txt."
#endif

namespace engine::core {

	std::string_view Version() {
		return MONO_VERSION;
	}
}
