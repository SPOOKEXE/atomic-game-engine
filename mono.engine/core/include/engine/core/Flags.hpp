#pragma once

// Process-wide named settings, resolved once at startup and frozen.
//
// **Declared by a table a module hands over, and never by self-registering
// statics.** `script/ServiceCatalogue` made this argument first and it holds
// harder here: a flag declared in a translation unit nothing else references is
// dropped by the linker out of a static archive, and a flag that silently does
// not exist is worse than one that is missing — the program runs, reads the
// built-in default, and reports nothing. So a module writes one table and one
// `Declare` call, a program names the registrars it wants, and a config key
// naming a flag no registrar declared is an **error** rather than a shrug.
//
// This is also what keeps a derived vocabulary from drifting.
// `assets::ContentFlags` builds its rows from the extension table itself, so a
// format added there arrives here with no second edit and no second list.
//
// **The name is the contract and the index is an optimisation**, which is rule
// 4 one layer down from `core::Name`. A flag reaches a config file, an
// environment variable and a command line; all three are places a string goes.
// `Flag` resolves a name to a row once so the read after that is an array
// index.
//
// **Frozen before the loop starts, and that is rule 5 rather than tidiness.** A
// value that can move between two ticks is a value two machines can disagree
// about, and the disagreement arrives as a desync a long way from the flag that
// caused it. `Freeze` is called by every program once its options are applied;
// a `Set` after it is refused and named in the log rather than being quietly
// dropped.
//
// Precedence is a property of the *source* and not of the call order:
//
//     built-in default < config file < environment < command line
//
// so a program may apply them in whichever order suits it and a config file
// still cannot overwrite what somebody typed.
//
// @tier L0 · shared
// @since v0.15

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

	// What a flag's text is read as.
	enum class FlagKind : uint8_t {
		// `true`/`false`, `on`/`off`, `yes`/`no`, `1`/`0`. A bare flag on a
		// command line means `true`.
		Boolean,

		// A whole number, read as `int64_t`.
		Integer,

		// A real number, read as `double`.
		Number,

		// Anything at all, including empty.
		Text,

		// A sequence of strings, in the order they were given.
		//
		// **Because some settings are genuinely several and a scalar cannot say
		// so.** Content origins are tried in priority order and an origin's
		// upstreams are too, so the setting is a sequence and splitting a scalar
		// on a separator would make any value containing that separator — a path
		// somebody chose — a bug nobody could see.
		//
		// A key repeated *within one source* appends; a source that outranks it
		// replaces the whole list. That is the scalar precedence rule applied to
		// a list rather than a second rule: a config file's three origins are
		// three lines, and one `--flag` on the command line replaces all three
		// rather than being appended to something the person cannot see.
		//
		// An empty value clears it, which is how a command line says "none" to a
		// file that named some.
		List,
	};

	// Where a value came from, in precedence order.
	//
	// **The order of the members is the precedence**, so comparing two sources
	// is a comparison and not a table. Adding one in the middle changes what
	// beats what, which is why they are spelled out rather than derived.
	enum class FlagSource : uint8_t {
		// What `Flags.cpp` declares. Every flag starts here.
		Default = 0,

		// A `[section]` `key = value` file — `core::ConfigFile`.
		ConfigFile = 1,

		// The process environment.
		Environment = 2,

		// What somebody typed.
		CommandLine = 3,
	};

	// A name for a source, for a log line and the `--flags` listing.
	//
	// @param source The source.
	// @return A view valid for the lifetime of the process.
	const char *Describe(FlagSource source);

	// One declared flag.
	//
	// The default is text for every kind, so one table holds all four and a
	// reviewer reads the declared value in the form a config file would write
	// it.
	struct FlagDescription {
		// Dotted and lowercase — `content.gif`, `server.tick-rate`. The prefix
		// is the area rather than the module, because a flag is read by whoever
		// needs it and named after what it is about.
		std::string_view Name;

		FlagKind Kind = FlagKind::Boolean;

		// The built-in value, spelled as a config file would spell it.
		std::string_view Default;

		// One line, in the imperative. What `--flags` prints beside the name.
		std::string_view Description;
	};

	// Why a `Set` did not take.
	enum class FlagStatus : uint8_t {
		// Applied, and the flag now reads this value.
		Applied,

		// No flag by that name is declared. **An error rather than a warning**,
		// for `core::Arguments`' reason: a typo that is silently ignored fails
		// at the behaviour instead of at the command line.
		NoSuchFlag,

		// The text is not a value of that flag's kind.
		NotAValue,

		// Something of at least this precedence has already spoken. Not a
		// failure — it is what makes a config file unable to overwrite a
		// command line — so a caller applying a whole file treats it as
		// ordinary.
		Outranked,

		// `Freeze` has been called. A caller that meant to set a flag before
		// the loop started has a bug in its startup order, so this is logged.
		Frozen,
	};

	// A name for a status, for the message a caller writes.
	//
	// @param status The status.
	// @return A view valid for the lifetime of the process.
	const char *Describe(FlagStatus status);

	// The declared table, the values on it, and the freeze.
	//
	// Values are written during startup on one thread and read from every
	// thread afterwards, which is what `Freeze` turns from a convention into a
	// checked one: after it, a read is a load of a value nothing can be writing.
	//
	// @threadsafe after Freeze
	class Flags {
	  public:
		// Adds `table` to what this process knows about.
		//
		// **The views are borrowed and not copied**, so a table is a
		// `static constexpr` array or something else that outlives the process.
		// A row whose name repeats one already declared is refused and named:
		// two rows for one name is two defaults, and which of them a build gets
		// would depend on link order.
		//
		// @param table The rows to declare.
		// @return `false` when a name repeated, after declaring the rest.
		static bool Declare(std::span<const FlagDescription> table);

		// Every flag this process has declared, in declaration order.
		static std::span<const FlagDescription> Declared();

		// Whether a flag by this name is declared.
		//
		// **The quiet form of the question**, where constructing a `Flag` and
		// asking `IsValid` logs. That is right for a read that was meant to
		// work and wrong for a caller checking whether a whole table was ever
		// registered — which is a legitimate thing to ask, and would otherwise
		// print a warning per row for a program that simply does not use them.
		//
		// @param name The flag.
		// @return `true` when something declared it.
		static bool Has(std::string_view name);

		// Applies `text` to the flag named `name`.
		//
		// @param name   The flag.
		// @param text   Its value, in the form a config file would write it. A
		//               boolean accepts an empty string as `true`, which is what
		//               a bare command-line flag hands over.
		// @param source Where it came from, which decides whether it wins.
		// @return What happened.
		static FlagStatus Set(std::string_view name, std::string_view text, FlagSource source);

		// Refuses every later `Set` and makes the values stable for the run.
		//
		// Called once by each program, after its options are applied and before
		// its loop starts. Calling it twice is harmless.
		static void Freeze();

		// Whether the values are stable.
		static bool Frozen();

		// Forgets every declaration, every `Set` and the freeze.
		//
		// **For a test and nothing else.** A program that needed this would be
		// a program changing a flag mid-run, which is the thing `Freeze` exists
		// to refuse.
		static void Reset();

		// What `--flags` prints: every flag, its value, and where the value came
		// from.
		//
		// @return One line per flag, each newline-terminated.
		static std::string Listing();

		// How many times the table has been emptied.
		//
		// **What makes a cached `Flag` index safe across a `Reset`.** Only a
		// test resets, and only a test would then read a handle resolved
		// against the table that is gone — but a stale index into a
		// re-declared table answers the *wrong flag*, which is a failure that
		// reads as the code under test being wrong.
		static uint32_t Generation();
	};

	// A flag table that owns the strings in it.
	//
	// **Because a table's defaults are usually derived rather than typed.** A
	// program's built-in values live in its own `Options` struct and a content
	// form's flag name is built from the extension table, so a
	// `static constexpr` array of literals cannot express either without
	// writing the fact down twice — which is the drift rule 2 exists for. This
	// holds the text and hands back rows pointing into it.
	//
	// The storage is node-based on purpose: `FlagDescription` borrows its
	// views, so a container that reallocates would dangle every row already
	// added.
	//
	// Build it into a function-local `static const` and hand `Rows()` to
	// `Flags::Declare`. It has to outlive the declaration, because the table
	// borrows from it too.
	class FlagTableBuilder {
	  public:
		FlagTableBuilder &Boolean(std::string_view name, bool value, std::string_view description);
		FlagTableBuilder &Integer(std::string_view name, int64_t value, std::string_view description);
		FlagTableBuilder &Number(std::string_view name, double value, std::string_view description);
		FlagTableBuilder &Text(std::string_view name, std::string_view value, std::string_view description);

		// A list, whose built-in value is the empty one.
		//
		// **No default items, and that is not an omission.** A built-in list
		// would have to be spelled somewhere and every source above it either
		// replaces or appends to it — so a default of one entry is a default a
		// deployment cannot remove without knowing it is there. The programs
		// that want a fallback apply it after reading, where it is visible.
		FlagTableBuilder &List(std::string_view name, std::string_view description);

		// The rows, valid for as long as this object is.
		std::span<const FlagDescription> Rows() const {
			return Descriptions;
		}

	  private:
		FlagTableBuilder &
		Add(std::string_view name, FlagKind kind, std::string value, std::string_view description);

		// Node-based, so adding a row cannot move the text an earlier row is
		// pointing at.
		std::deque<std::string> Storage;
		std::vector<FlagDescription> Descriptions;
	};

	// A read of one flag by name.
	//
	// Construct once — a file-scope constant beside the code that reads it is
	// the intended shape — and read as often as you like. The first read is a
	// linear scan of the table; every read after it is an array index.
	//
	// **Resolved on first read rather than on construction**, because a
	// file-scope `Flag` is built during static initialisation and the table it
	// names is filled by a `Declare` call in `main`. Resolving eagerly would
	// make the intended shape the one that never works, and it would depend on
	// translation-unit order to do it.
	//
	// **An undeclared name is a dead handle rather than a crash**, and it logs
	// once. A flag read through a dead handle answers its type's zero, which is
	// the same answer a caller would have got from a flag that exists and is
	// off — so the log line is the only thing that tells them apart, and it
	// names the flag rather than the symptom.
	class Flag {
	  public:
		// Names a flag. Nothing is looked up until something is read.
		//
		// @param name The flag's name, which must outlive this object. A string
		//        literal is the intended argument.
		explicit constexpr Flag(std::string_view name) : Wanted(name) {}

		// Whether the name is declared. Resolves it if it has not been.
		bool IsValid() const;

		// The flag's name, or empty when it is not declared.
		std::string_view Name() const;

		// The value as a boolean. `false` for a flag of another kind.
		bool Boolean() const;

		// The value as a whole number. Zero for a flag of another kind.
		int64_t Integer() const;

		// The value as a real number. Zero for a flag of another kind.
		//
		// **A `Number` or an `Integer`**, because a caller wanting a double does
		// not care which of the two somebody declared, and the alternative is
		// every call site testing the kind.
		double Number() const;

		// The value as text, whatever the kind — so a listing can print one
		// without switching. A list is joined with `, `, for display only.
		//
		// Valid until the next `Set` of this flag, and for the life of the
		// process once `Flags::Freeze` has been called.
		std::string_view Text() const;

		// The value as a sequence, in the order it was given.
		//
		// Empty for a flag of another kind, and for a list nobody has set.
		//
		// Valid for as long as `Text` is.
		std::span<const std::string> Items() const;

		// Where the current value came from.
		FlagSource Source() const;

	  private:
		// Fills `Index`, once, and returns it.
		int32_t Resolve() const;

		std::string_view Wanted;

		// An index into the declared table; -1 for a name nothing declares and
		// `UNRESOLVED` before the first read.
		//
		// **Mutable so a `const` read can fill it**, which is the whole of what
		// makes a `static const Flag` beside its reader work.
		static constexpr int32_t UNRESOLVED = -2;
		mutable int32_t Index = UNRESOLVED;

		// The table generation `Index` was resolved against.
		mutable uint32_t ResolvedAt = 0;
	};
}
