#pragma once

// What a script may name, as data an editor can offer.
//
// **Read out of a live VM rather than written down beside it.** Every global
// this engine installs is one of about thirty `lua_setglobal` calls and twenty
// five `JS_SetPropertyStr` calls spread across fifteen source files, and there
// is no table of them anywhere — so a list here would be a fourth copy after
// the two VMs and `mono.tools/bindings`' prelude strings, and the one nobody
// would regenerate.
//
// This module has already paid for that mistake twice, and both are recorded
// where they happened. `LuauValues.cpp` carries `Magnitude` and `Unit`, which
// `engine.d.luau` promised for two versions while the run time answered
// "Vector3 has no member 'Unit'" — a script that typechecked clean and failed
// anyway, invisible to `bindings-check` because it compares the declarations
// against the *class table* and a value type's members are in neither.
// `JsSurface.cpp` carries a hand-written `10` on a list of sixteen methods,
// which simply did not install the last six.
//
// So `Runtime::Surface` walks the global table of a runtime that has just been
// constructed and reports what is actually there. A global added anywhere in
// this module is offered by the editor with nothing here changing, and a global
// removed stops being offered in the same commit.
//
// **What cannot be walked is said rather than guessed, and there is one of
// those.** A member reached through an `__index` *function* — `Vector3.Unit`,
// `Rect.Width`, `game.Workspace`, `part.Changed` — is a string comparison
// rather than an entry in a table, and nothing can enumerate a branch. Instance
// *methods* are fine: they live in a real table, Luau's in the registry under
// `engine.instance.methods` and JavaScript's in `__instanceMethods`, so a walk
// finds them. Instance *signals* on the Luau side are the branch chain, so
// `LuauInstances.cpp` keeps a list beside it and the suite checks every entry
// against a live VM.
//
// Classes, properties and enums are not here at all, because `ecs::Classes` and
// `ecs::EnumTable` already answer for them and a copy would be the drift this
// file exists to avoid.
//
// @tier L9 - shared

#include <engine/script/Language.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// What sort of thing a name is, so an editor can rank and icon it.
	//
	// **Three cases and not a type system.** A walk of a global table can tell
	// a function from a container from a plain value, and that is genuinely
	// everything it can tell — anything finer would be a guess dressed as a
	// fact.
	//
	// @since v0.14
	enum class NameKind : uint8_t {
		// Callable. `print`, `require`, `task.wait`.
		Function,

		// Holds members worth offering after a dot. `task`, `math`, `Enum`.
		Container,

		// Anything else — a number, a string, an instance, a userdata.
		Value,
	};

	// One name a script may write, and what is under it.
	//
	// @since v0.14
	struct VocabularyEntry {
		// The name as a script spells it.
		std::string Name;

		NameKind Kind = NameKind::Value;

		// The members a dot after this name could reach, empty when there are
		// none *or* when they cannot be enumerated.
		//
		// **The two are not distinguished, deliberately.** A table whose members
		// come from an `__index` function and a table with nothing in it both
		// answer nothing to a walk, and inventing a third state would mean
		// claiming to know which — the guess this file exists to avoid.
		std::vector<std::string> Members;
	};

	// Everything one VM offers, as an editor needs it.
	//
	// @since v0.14
	struct ScriptSurface {
		// Every global, in no particular order.
		std::vector<VocabularyEntry> Globals;

		// The methods and signals every instance carries, so that a `part` followed
		// by `:` or by a dot can be answered without a class.
		//
		// Properties are deliberately absent: those come from `ecs::Classes`,
		// which knows the class an instance actually is and can therefore answer
		// better than a flat list.
		std::vector<std::string> InstanceMembers;
	};

	// The language's own reserved words.
	//
	// Offered before any engine name has been typed, which is what keeps a
	// completion list useful on the first line of an empty script.
	//
	// @param language Which language's keywords.
	// @return The keywords, sorted.
	// @since v0.14
	std::span<const std::string_view> Keywords(Language language);

	// Names a walk finds that an editor should not offer.
	//
	// Two kinds, and both would be worse than an absence. **Refusal stubs** —
	// `wait`, `spawn`, `delay`, `loadstring`, `getfenv`, `setfenv` — exist so
	// that a script written elsewhere fails with a sentence instead of "attempt
	// to call a nil value"; offering one is offering a name that always throws.
	// **Internals** — JavaScript's `__instanceMethods`, Luau's `_G` — are
	// reachable but are not surface anybody should be writing against.
	//
	// @param language Which VM's surface.
	// @return The names to drop, sorted.
	// @since v0.14
	std::span<const std::string_view> Withheld(Language language);

	// Every word a bus reply's status can be, in ordinal order, `"Unknown"` last.
	//
	// **`script::DescribeStatus`'s switch, read rather than transcribed.** The
	// generated declarations carry a `BusStatus` string union, and until v0.18
	// both of them were hand-written text in `mono.tools/bindings/app/main.cpp`
	// with a comment asking whoever added a status to remember. Nothing in the
	// build checked it, which is the third category rule 6 refuses: a status
	// appended to `world::BusStatus` typechecked as an error in correct script
	// code, and the only thing that would have said so was somebody re-reading a
	// comment.
	//
	// `just bindings-check` is what closes it now — the generator builds both
	// unions from this list, so a new status that has not reached the checked-in
	// declarations fails the check by name.
	//
	// **`"Unknown"` is a real member and is last.** It is what `DescribeStatus`
	// answers for a value it does not recognise, which a script can be handed
	// after a version skew between two processes, so a union without it makes
	// correct handling code a typecheck failure.
	//
	// @return The words, valid for the life of the program.
	// @since v0.18
	std::span<const std::string_view> BusStatusWords();

}
