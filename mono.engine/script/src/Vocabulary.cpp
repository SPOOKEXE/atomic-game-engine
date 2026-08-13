#include "Bindings.hpp"

#include <engine/script/Vocabulary.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {

		// Luau's reserved words, from the lexer rather than from memory.
		//
		// Sorted, because the completion list is ranked by match quality and
		// ties break by name — an unsorted source would put two runs in a
		// different order for no reason anybody could see.
		constexpr std::string_view LUAU_KEYWORDS[] = {
			"and",	 "break",  "continue", "do",   "else", "elseif", "end",	  "export",
			"false", "for",	   "function", "if",   "in",   "local",	 "nil",	  "not",
			"or",	 "repeat", "return",   "then", "true", "type",	 "until", "while",
		};

		// JavaScript's, likewise. `let`, `const` and `class` are the ones an
		// author reaches for first and the reason this list is not simply
		// Luau's with the names changed.
		constexpr std::string_view JAVASCRIPT_KEYWORDS[] = {
			"async",	"await",   "break",	   "case",	 "catch",  "class",	 "const",	   "continue",
			"debugger", "default", "delete",   "do",	 "else",   "export", "extends",	   "false",
			"finally",	"for",	   "function", "if",	 "import", "in",	 "instanceof", "let",
			"new",		"null",	   "of",	   "return", "static", "super",	 "switch",	   "this",
			"throw",	"true",	   "try",	   "typeof", "var",	   "void",	 "while",	   "yield",
		};

		// Luau names a walk finds that must not be offered.
		//
		// The first six are the refusal stubs `LuauTask.cpp` and `LuauHost.cpp`
		// install so that a script written for Roblox fails with a sentence
		// naming its replacement. Offering one would be offering a name whose
		// only behaviour is to throw.
		//
		// `_G` and `_VERSION` are real and work; they are withheld because
		// neither is something anybody should be writing in a game script, and
		// `_G` in particular is the one name that defeats the per-chunk
		// sandbox.
		constexpr std::string_view LUAU_WITHHELD[] = {
			"_G",
			"_VERSION",
			"delay",
			"getfenv",
			"loadstring",
			"setfenv",
			"spawn",
			"wait",
		};

		// `__instanceMethods` is a global only because that is where the shared
		// method object had to live for `PrototypeFor` to reach it — see
		// `JsSurface.cpp`. It is not part of the surface anybody writes against.
		//
		// `globalThis`, `undefined` and `nil` are real and reachable; the first
		// two are noise at the top of every completion list and the third is
		// offered as a keyword instead, because that is what it is standing in
		// for.
		constexpr std::string_view JAVASCRIPT_WITHHELD[] = {
			"__instanceMethods",
			"globalThis",
			"undefined",
		};

	}

	std::span<const std::string_view> InstanceSignals(const Language language) {
		// **Luau's alone.** JavaScript installs the same fourteen as accessors
		// on `__instanceMethods`, so a walk of that object already reports
		// them; answering here as well would offer every one of them twice.
		if (language != Language::Luau) {
			return {};
		}

		// Built once from the array that sits beside the branch chain in
		// `Instances.cpp`, so this file names no signal of its own.
		static const std::vector<std::string_view> signals = LuauInstanceSignalNames();
		return signals;
	}

	std::span<const std::string_view> Keywords(const Language language) {
		return language == Language::Luau ? std::span<const std::string_view>(LUAU_KEYWORDS)
										  : std::span<const std::string_view>(JAVASCRIPT_KEYWORDS);
	}

	std::span<const std::string_view> Withheld(const Language language) {
		return language == Language::Luau ? std::span<const std::string_view>(LUAU_WITHHELD)
										  : std::span<const std::string_view>(JAVASCRIPT_WITHHELD);
	}

}
