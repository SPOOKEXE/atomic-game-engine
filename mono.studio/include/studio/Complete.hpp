#pragma once

// arch-waiver public-header: forward studio API. Script tooling uses this
// complete completion contract.

// What to offer somebody typing in the script editor.
//
// **Free functions over text, because this is the half that can be silently
// wrong.** A completion list that quietly offers nothing looks exactly like an
// engine with no API, and one that offers a name the VM does not have is worse
// than offering nothing at all - an author picks it out of a list, writes it,
// and finds out at run time in whatever scene reaches that line first. Both
// failures need a window, a device and an imgui frame to see, and neither needs
// one to happen. `mono.studio/AGENTS.md` names `MatchesQuery` and `DiffText`
// for the same reason; this is a third of that kind.
//
// **Nothing here is a parser.** `ScanBackwards` walks left from the caret over
// identifier characters, dots, colons and balanced bracket groups, and stops at
// the first thing that is none of those. That resolves a caret after
// `part.`, `Enum.Material.` or `game:GetService("Players").`, and after the
// opening quote of an `Instance.new` call. A real parse would need the
// language's grammar twice over, and this file covers two languages.
//
// **There is no type inference. There is assignment following, which is a
// smaller thing.** A local is resolved when the buffer *says* what it holds:
// `Instance.new("Part")`, `game:GetService("Lighting")`,
// `FindFirstChildOfClass("Part")`, a `: Part` annotation or a `:: Part` cast
// write the class out; a call's declared return type (`Vector3.new`,
// `World:CreateEntity`) comes from the API dump; `:Clone()` carries its
// receiver's class; and `local same = part` is the same local under another
// name. Only the **last** assignment before the caret counts.
//
// `FindFirstChild` and `WaitForChild` narrow only when the edited world has a
// child of that name, and then to that child's class. Without one they answer
// `Instance`, and an `Instance` of unknown class falls back to the union of
// every scriptable property: a child of a `Model` is not a `Model`, so
// narrowing to the receiver would be a guess.
//
// **Which is why every row says whose property it is.** A narrowed row's detail
// reads `bool on Part` and a union row's reads `bool on some class`: the first
// claims "this class has this" and the second says "one of these classes has
// this", and an author who cannot tell those apart is the failure this feature
// was allowed to have. The marker goes in `Completion::Detail` because that is
// what the popup already draws, so the distinction survives without the drawing
// code having to be told about it.
//
// **Typed resolution comes from the engine API dump.** `mono.tools/bindings`
// writes `api-dump.json` beside the declarations and this editor embeds it, so a
// subject like `game:GetService("Players")`, `Vector3.new(1, 2, 3)` or
// `part:Clone()` resolves to a type whose methods, events and properties are
// offered, and a call's parameters are shown by `SignatureAt`. A subject that
// walks the tree (`script.Parent.Door`, `workspace:WaitForChild("Map")`)
// resolves through `CompletionSources::Tree`, the edited world's instances by
// name. Code above the caret is read for what it does to that tree:
// `Instance.new` parented somewhere adds a node, `:Destroy()` removes one, and a
// local destroyed above the caret is marked rather than offered as live.
//
// Script names are read live from `ecs::Classes`, `ecs::EnumTable` and a VM's
// `script::Runtime::Surface`. Shader names are the exception: GLSL has no VM to
// walk, so its stable language vocabulary is held here and checked directly by
// this module's suite.
//
// @tier client

#include <engine/ecs/Classes.hpp>
#include <engine/script/Language.hpp>
#include <engine/script/Vocabulary.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace studio {

	class ApiDump;

	// What kind of thing an entry is, which decides its tag and its ranking.
	//
	// @since v0.14
	enum class CompletionKind : uint8_t {
		// A class name, offered inside `Instance.new("` and its relatives.
		Class,

		// A property from `ecs::Classes`.
		Property,

		// A method or signal every instance carries.
		Member,

		// A component selection from a GLSL vector.
		Swizzle,

		// An enum set, or one of its members.
		Enum,

		// A global the VM installs, or a member of one.
		Global,

		// A reserved word of the language being written.
		Keyword,

		// A type name built into the language.
		Type,

		// A function built into the language.
		Function,

		// A non-function value built into the language.
		Builtin,

		// An identifier already written in this file.
		Local,

		// The name of an instance in the tree beside this script.
		Child,
	};

	// Which vocabulary and member rules a completion uses.
	//
	// @since v0.21
	enum class CompletionDomain : uint8_t {
		// Luau or JavaScript authored against the engine's script surface.
		Script,

		// GLSL 4.50 fragment source authored in a `ShaderScript`.
		Shader,
	};

	// One row of the popup.
	//
	// @since v0.14
	struct Completion {
		// What replaces the prefix under the caret.
		std::string Text;

		// The dimmed hint on the right: a property's type and whose it is, a
		// class's parent, what a global is. Empty when the name says
		// everything.
		//
		// A property's detail names the class it is being claimed for -
		// `bool on Part` - or says `bool on some class` when the list is the
		// union, which is the only thing distinguishing a narrowed list from a
		// broad one on screen.
		std::string Detail;

		// Which list this row came out of - a class, a property, an enum, a
		// keyword, a global or a local. The popup groups by it, so a row whose
		// kind is wrong is a row filed under the wrong heading rather than a
		// missing one.
		CompletionKind Kind = CompletionKind::Local;

		// From `FuzzyMatch`, so the popup ranks the way the explorer, the
		// properties panel, the command bar and the asset picker already do.
		int Score = 0;
	};

	// What the caret is sitting in, as text alone.
	//
	// **A value rather than three out-parameters**, because every field is read
	// together and a caller that got two of them from one scan and the third
	// from another would be describing two different carets.
	//
	// @since v0.14
	struct CompletionQuery {
		// The identifier characters immediately before the caret. Empty right
		// after a dot, which is the common case and not an error.
		std::string_view Prefix;

		// The expression the separator hangs off - `part` in `part.Anch`,
		// `Enum.Material` in `Enum.Material.Pl`, `game:GetService("Players")`
		// in `game:GetService("Players").Lo`. Calls and indexing are kept whole.
		// Empty when there is no separator, or when the chain ends in one.
		std::string_view Subject;

		// A dot, a colon, or `\0` when the prefix stands alone.
		//
		// The two are kept apart rather than folded together because Luau uses
		// them for different things: `part:Destroy()` passes the instance and
		// `part.Name` does not, so offering methods after a dot would be
		// offering a call that is missing its first argument.
		char Separator = '\0';

		// Whether the caret is inside a string literal on this line.
		bool InString = false;

		// The dotted name being called, when the caret is inside its first
		// string argument - `Instance.new`, `game:GetService`, `IsA`. This is
		// what turns a quote into a list of class names.
		std::string_view Call;
	};

	// Reads the caret's surroundings.
	//
	// @param text  The whole buffer.
	// @param caret A byte offset into it. Past the end is clamped.
	// @return What is under and before the caret.
	// @since v0.14
	CompletionQuery ScanBackwards(std::string_view text, size_t caret);

	// One instance in the tree a script is completed against.
	//
	// @since v0.26
	struct CompletionNode {
		std::string Name;
		std::string ClassName;

		// Index of the parent in the same list, or -1 for a world root.
		int Parent = -1;
	};

	// Everything a completion needs that is not the text itself.
	//
	// @since v0.14
	struct CompletionSources {
		// Whether this document is an engine script or a shader.
		CompletionDomain Domain = CompletionDomain::Script;

		// Which language's keywords and globals to offer.
		//
		// **The script's own, not the editor's.** A `.luau` and a `.js` in one
		// game are completed against different vocabularies, and
		// `CodeSourceContainerSelector` is what decides which this is.
		engine::script::Language Language = engine::script::Language::Luau;

		// The globals and instance members of a VM of that language, or null
		// when none could be made. Null degrades the list rather than emptying
		// it: classes, properties, enums and keywords still come through.
		const engine::script::ScriptSurface *Surface = nullptr;

		// The names of instances beside this script, for a caret after
		// `script.Parent` or `workspace`. Gathered by the panel, because reading
		// them means being inside `Universe::Enter` and this function is not.
		// Offered when `Tree` cannot resolve the subject.
		std::span<const std::string> Children;

		// The edited world's instances, for resolving `script.Parent.Door` by
		// name. Empty when there is no world.
		std::span<const CompletionNode> Tree;

		// Which node of `Tree` is the script being edited, or -1.
		int ScriptNode = -1;

		// The API dump to type against. Null means the one embedded in this
		// build.
		const ApiDump *Api = nullptr;
	};

	// A call's signature, for the hint above the caret while its arguments
	// are being typed.
	//
	// @since v0.26
	struct SignatureHelp {
		// `Vector3.new(x: number?, y: number?, z: number?): Vector3`.
		std::string Label;

		// Each parameter's byte range inside `Label`, for highlighting.
		std::vector<std::pair<size_t, size_t>> Parameters;

		// Which parameter the caret is in, clamped to the last one.
		size_t Active = 0;

		// How many overloads the callee has; `Label` is the first.
		size_t Overloads = 1;
	};

	// The call whose argument list the caret sits in, when the callee resolves
	// to something the API dump can describe.
	//
	// @param text    The whole buffer.
	// @param caret   A byte offset into it.
	// @param sources Where the names come from.
	// @return The signature, or nothing when the caret is not in a known call.
	// @since v0.26
	std::optional<SignatureHelp>
	SignatureAt(std::string_view text, size_t caret, const CompletionSources &sources);

	// What to offer, best first.
	//
	// @param text    The whole buffer.
	// @param caret   A byte offset into it.
	// @param sources Where the names come from.
	// @param limit   How many rows to return at most.
	// @return The ranked entries, empty when nothing matches.
	// @since v0.14
	std::vector<Completion>
	CompleteAt(std::string_view text, size_t caret, const CompletionSources &sources, size_t limit = 40);

	// What kind of thing a completion row is, as one lowercase word or phrase.
	//
	// The popup's footer and nothing else, so a row's kind is visible without
	// each drawing site keeping its own copy of the mapping.
	//
	// @param kind The row's kind.
	// @return A static string, never empty.
	// @since v0.17
	std::string_view Describe(CompletionKind kind);

	// One sentence about a reserved word, for a hover and for the popup's
	// footer.
	//
	// **Written down rather than derived, and that is fine here.** Everything
	// else the editor offers is read live from a VM or the class table because
	// those change; a language's keywords change when the language does, which
	// is a vendor bump somebody reads the notes of. `tests/Complete.cpp` holds
	// the two lists together: every word `script::Keywords` answers has a line
	// here, so a keyword added upstream fails a test rather than hovering
	// silent.
	//
	// @param language Whose reserved words.
	// @param word     The keyword.
	// @return The sentence, or empty when `word` is not one of that
	//         language's keywords.
	// @since v0.17
	std::string_view KeywordDoc(engine::script::Language language, std::string_view word);

	// One sentence about a GLSL reserved word.
	//
	// @param word The keyword.
	// @return The sentence, or empty when `word` is not a GLSL keyword.
	// @since v0.21
	std::string_view ShaderKeywordDoc(std::string_view word);

	// The identifier under a byte offset.
	//
	// @param text   The whole buffer.
	// @param offset A byte offset into it.
	// @return The word the byte sits inside, or empty when it sits on
	//         whitespace, punctuation, or past the end.
	// @since v0.17
	std::string_view WordAt(std::string_view text, size_t offset);

	// What the editor knows about the word under a byte offset, for a hover
	// tooltip.
	//
	// **Empty means no tooltip, and empty is the common answer.** A hover that
	// says "no information" over most of a file is a hover somebody turns off;
	// one that appears only when it has a sentence worth reading is the
	// convention every editor follows. Inside a string or a line comment it is
	// always empty - a keyword quoted in prose is prose.
	//
	// What it can say, first match wins: a keyword's doc line, what a global
	// is, that a name is a method or signal every instance carries, an enum
	// set, a class and its parent, an instance beside this script, a member
	// after a dot or colon read off the subject's type (a method's signature,
	// a property's type), a local whose type the buffer states (the same
	// assignment following `CompleteAt` narrows with, and whether it was
	// destroyed above), or a property's type and the class that declares it.
	//
	// @param text    The whole buffer.
	// @param offset  A byte offset into it.
	// @param sources The same sources a completion at that spot would use.
	// @return One or two lines for a tooltip, or empty for no tooltip.
	// @since v0.17
	std::string HoverText(std::string_view text, size_t offset, const CompletionSources &sources);

	// The classes an author may write where a class name is wanted.
	//
	// **One function, because the class picker and the completion popup are two
	// lists of the same thing.** `mono.studio/AGENTS.md` says the picker never
	// hard-codes a class and that the abstract bases it does name live in one
	// place; `mono.tools/bindings` says copying that list into a second place is
	// how the two would disagree later. So the explorer and this call here.
	//
	// Everything under `Instance`, minus the services - a world has exactly one
	// of each and `scene::InstallServices` puts it there, so offering one is
	// offering a second that nothing resolves - and minus the abstract bases,
	// which the run time would happily mint into rows nothing knows how to draw.
	//
	// @return The class ids, in registration order.
	// @since v0.14
	std::vector<engine::ecs::ClassId> InsertableClasses();

}
