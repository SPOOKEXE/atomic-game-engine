#pragma once

// Which first-party functions no program can ever reach.
//
// The graph is built from relocations, not from source. Every relocation in a
// function's bytes is an edge from that function to whatever it names: a call,
// a function pointer, a vtable, a jump table. Edges between objects are joined
// by symbol name, the way the linker would join them. A walk from the roots
// then marks everything live, and what is left over has no path from any
// program's start.
//
// Roots:
// - `main` in a program's own objects
// - every static initialiser (`.init_array`, `.ctors` and friends)
// - any first-party name a vendor object refers to (callbacks found by name)
//
// The answer is only as good as the build it reads. It needs one section per
// function (`-ffunction-sections`), because a call within one section is
// resolved by the assembler and leaves no relocation. And it needs `-O0`,
// because an inlined helper has no call left to find. Unoptimised first-party
// builds get both; see `MONO_COMPILE_OPTIONS` in the root `CMakeLists.txt`.
//
// A virtual function of a class that is constructed anywhere stays live through
// its vtable even if no call ever lands on it. That is a known blind spot, not
// a bug: the vtable really does hold its address.
//
//     std::vector<deadcode::Input> inputs = ...;
//     deadcode::Analysis analysis = deadcode::Analyse(inputs);
//     for (const deadcode::Function &f : analysis.Dead) { ... }
//
// @tier L0 · shared

#include <deadcode/Elf.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace deadcode {

	// What an object is for. `Ship` objects go into programs a player or a
	// developer runs, `Test` objects into Catch2 binaries, and `Vendor` objects
	// are only read for the first-party names they call back into.
	enum class Role { Ship, Test, Vendor };

	struct Input {
		std::string Source;
		Role Kind = Role::Ship;
		ObjectFile Object;
	};

	struct Function {
		std::string Source;
		std::string Name;
		uint64_t Size = 0;
		// Weak, so emitted from a header (inline or template) rather than
		// defined in `Source` itself.
		bool Inline = false;
	};

	struct Analysis {
		// Reached by no program and no test.
		std::vector<Function> Dead;
		// Reached by a test and by no program. Code that exists for its tests.
		std::vector<Function> TestOnly;
		// How many non-inline functions each `Ship` source defines, so a
		// source whose every one is dead can be called out as a whole.
		std::map<std::string, size_t> FunctionsPerSource;
	};

	Analysis Analyse(const std::vector<Input> &inputs);

	// True when two sized functions share one section. A call between them
	// needs no relocation, so the object was built without
	// `-ffunction-sections` and its graph would be missing edges.
	bool SharesCodeSection(const ObjectFile &object);

	// The readable form of a mangled C++ name, or the name itself when it is
	// not one.
	std::string Demangle(const std::string &name);

}
