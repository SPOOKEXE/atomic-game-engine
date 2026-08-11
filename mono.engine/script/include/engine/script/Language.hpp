#pragma once

// Which language a program is written in, and how a file name says so.
//
// **Its own header because two headers need it and one of them is heavy.**
// `Runtime.hpp` pulls in the debugger and the host surface; `Instances.hpp` is
// included by everything that touches a script instance and needs the enum to
// hold a default in `CodeSourceContainerSelector`. A forward declaration is not
// enough for that — an enumerator needs the definition — and including the
// runtime from the instance header would drag the VM into every consumer.
//
// @tier L9 - shared

#include <cstdint>
#include <string_view>

namespace engine::script {

	// Which VM runs a script.
	//
	// @since v0.5
	enum class Language : uint8_t {
		// Luau, from `mono.vendor/luau`.
		Luau,

		// JavaScript, from `mono.vendor/quickjs`. TypeScript is the typed
		// authoring surface over this one — it erases its types by design, so
		// there is nothing else for a "TypeScript VM" to have been.
		JavaScript,
	};

	// The language a file's extension names.
	//
	// `.luau` and `.lua` are Luau; `.js`, `.mjs` and `.ts` are JavaScript. A
	// `.ts` file is expected to have been type-stripped already — nothing in
	// the C++ build compiles TypeScript, and nothing should: the engine loads
	// what a toolchain emitted.
	//
	// @param path The file name or path.
	// @return The language, defaulting to Luau when the extension says nothing.
	Language LanguageOf(std::string_view path);

}
