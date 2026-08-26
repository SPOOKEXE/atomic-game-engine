#pragma once

// Reading a first-party tree the way the architecture rules read it.
//
// **Not a C++ parser, and it must not become one.** The four rules in
// `docs/CODE_ARCH.md` §11 are about declarations - a member's type, a header's
// includers, an argument reaching a writer - and every one of them is decidable
// from the shape of the text once comments and literals are out of the way. A
// tool that needed a real front end would need the build's include paths, which
// means it could only run after a configure, which is exactly what the
// architecture check is not.
//
// What that costs is stated rather than hidden: a macro that expands to a
// member declaration is invisible here, and so is a type reached through a
// `using` alias. `Rules.hpp` says per rule what it catches and what it does not.
//
// **Offsets are preserved.** `Strip` writes a space over every character it
// removes and keeps every newline, so an offset into the stripped text is the
// same offset in the original and a line number can be taken from either.
//
// @tier L0 · shared

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sourcecheck {

	// One data member of a record.
	struct Member {
		// The declared type, as written. `std::vector<engine::scene::Camera>`
		// stays whole, because a container of a component is the same fact
		// stored twice.
		std::string Type;

		// The member's own name.
		std::string Name;

		// One-based, in the file the record came from.
		size_t Line = 0;
	};

	// A `struct`, `class` or `union` with a body.
	struct Record {
		// The name as declared. Unqualified.
		std::string Name;

		// The enclosing namespaces, joined with `::`. Empty at global scope.
		// Enclosing *records* are not part of this - `Enclosing` carries that.
		std::string Namespace;

		// The record this one is nested inside, or empty.
		std::string Enclosing;

		// One-based line of the declaration.
		size_t Line = 0;

		// Data members only. Anything with a parameter list is behaviour.
		std::vector<Member> Members;

		// Whether the body declares a function of any kind.
		//
		// **The discriminator the ECS-copy rule turns on**, and it is the one
		// that took the rule from eighteen findings to seven. A record with no
		// behaviour is an argument list somebody wrote down - `render::View`
		// carries a camera because a draw call needs one - and a value passed to
		// a call is not a second authority for it. A record with behaviour is an
		// object that lives across frames, and a component sitting in one is the
		// second copy rule 2 is about.
		bool HasBehaviour = false;
	};

	// An `enum` or `enum class` declaration.
	struct Enumeration {
		// The name as declared. Unqualified.
		std::string Name;

		// The enclosing namespaces, joined with `::`.
		std::string Namespace;

		// One-based line of the declaration.
		size_t Line = 0;
	};

	// A rule switched off at one place, with the reason in the source.
	//
	// Written `// arch-waiver <rule>: <reason>` on the line above the
	// declaration it covers, or anywhere in the file for a file-level rule.
	//
	// **A comment rather than a list in a build file**, because a list drifts
	// away from the code it names and nobody notices; a comment moves when the
	// declaration moves and dies when it dies. Plain text rather than a
	// `@`-command, because `docgen` runs Doxygen with `WARN_IF_DOC_ERROR` and an
	// unknown command is a documentation error.
	//
	// A reason beginning with `known violation` means something different from
	// the rest: see `Rules.hpp`.
	struct Waiver {
		// The rule name - `ecs-copy`, `world-pointer`, `name-id`,
		// `public-header`.
		std::string Rule;

		// Everything after the colon. Empty is refused: a waiver with no reason
		// is the third category rule 6 exists to stop.
		std::string Reason;

		// One-based line of the comment itself.
		size_t Line = 0;

		// The first line of code at or after the comment, which is what the
		// waiver covers. Zero when the comment is the last thing in the file.
		size_t Covers = 0;
	};

	// One first-party source file, read once.
	struct File {
		// Relative to the scanned root, with `/` separators on every platform.
		std::string Path;

		// The module directory's name - `core`, `client`, `studio`. A module is
		// a directory with an `include/` beside its sources.
		std::string Module;

		// The module directory, relative to the root.
		std::string ModuleDir;

		// The path a `#include` would name this file by, or empty when it is not
		// under `include/`. `engine/core/Name.hpp`.
		std::string IncludePath;

		// Whether the file is under the module's `tests/` or `benchmarks/`.
		// Those are read for the include graph and are never reported against:
		// a suite is allowed to build the thing it is testing.
		bool Test = false;

		// The file as it is on disk.
		std::string Raw;

		// The same length, with comments and literal bodies blanked.
		std::string Stripped;

		// Every record with a body, in declaration order.
		std::vector<Record> Records;

		// Every named enumeration, in declaration order.
		std::vector<Enumeration> Enums;

		// Every `#include` target, as written between the delimiters.
		std::vector<std::string> Includes;

		// Every rule this file switches off, and where.
		std::vector<Waiver> Waivers;

		// One-based lines carrying `// arch-crossing`, which marks the record
		// below as something that travels between worlds.
		std::vector<size_t> Crossings;
	};

	// Every first-party source file under one root.
	struct Tree {
		// In directory-walk order, made deterministic by sorting on `Path`.
		std::vector<File> Files;
	};

	// Blanks comments and the insides of string and character literals.
	//
	// Raw string literals are handled by their delimiter, because a `//` inside
	// a block of embedded GLSL is not a comment and blanking from there to the
	// end of the line would eat a brace.
	//
	// @param text The file as read.
	// @return A string of the same length, positions unchanged.
	std::string Strip(std::string_view text);

	// Every record with a body, including nested ones.
	//
	// @param stripped The output of `Strip`.
	// @return Records in declaration order.
	std::vector<Record> Records(std::string_view stripped);

	// Every enumeration with a name.
	//
	// @param stripped The output of `Strip`.
	// @return Enumerations in declaration order.
	std::vector<Enumeration> Enums(std::string_view stripped);

	// Every `#include` target, quoted or angled.
	//
	// Read from the original text rather than the stripped one, because a quoted
	// include is a string literal and `Strip` has blanked it.
	//
	// @param raw The file as read.
	// @return Include targets, as written between the delimiters.
	std::vector<std::string> Includes(std::string_view raw);

	// Every `// arch-waiver <rule>: <reason>` in the file.
	//
	// A comment with no reason after the colon is returned with an empty
	// `Reason`, which the rules report as an error rather than honouring.
	//
	// @param raw The file as read.
	// @return Waivers in file order.
	std::vector<Waiver> Waivers(std::string_view raw);

	// Every `// arch-crossing` marker, as the line of the declaration it covers.
	//
	// @param raw The file as read.
	// @return One-based lines of the marked declarations.
	std::vector<size_t> Crossings(std::string_view raw);

	// Reads one file into a `File`, without deciding which module it is in.
	//
	// @param path The path to record.
	// @param text The file's contents.
	// @return The parsed file, with `Module` and `IncludePath` left empty.
	File Parse(std::string_view path, std::string_view text);

	// Walks `root` and reads every first-party `.hpp`, `.cpp`, `.h` and `.inl`.
	//
	// Skips `mono.vendor`, `.git`, `.cache`, `node_modules`, anything beginning
	// with a dot, and any directory named `fixtures` - the last of those is what
	// keeps this tool's own negative test data out of a scan of the repository
	// that contains it.
	//
	// @param root The tree to read.
	// @return Every file, sorted by path.
	Tree Scan(const std::filesystem::path &root);
}
