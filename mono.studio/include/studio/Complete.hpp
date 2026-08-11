#pragma once

// What to offer somebody typing in the script editor.
//
// **Free functions over text, because this is the half that can be silently
// wrong.** A completion list that quietly offers nothing looks exactly like an
// engine with no API, and one that offers a name the VM does not have is worse
// than offering nothing at all — an author picks it out of a list, writes it,
// and finds out at run time in whatever scene reaches that line first. Both
// failures need a window, a device and an imgui frame to see, and neither needs
// one to happen. `mono.studio/AGENTS.md` names `MatchesQuery` and `DiffText`
// for the same reason; this is a third of that kind.
//
// **Nothing here is a parser.** `ScanBackwards` walks left from the caret over
// identifier characters, dots and colons and stops at the first thing that is
// neither. That resolves `part.`, `Enum.Material.` and `Instance.new("` — which
// is most of what an author is doing when they want a list — and it resolves
// nothing about `f().` on purpose. A real parse would need the language's
// grammar twice over, and this file covers two languages.
//
// **There is no type inference, and the limit is worth stating out loud.** A
// local declared `local p = Instance.new("Part")` is resolved, because the class
// is written on the line. A local from `part:FindFirstChild("X")` is not, and
// after it `p.` offers the union of every scriptable property rather than
// `BasePart`'s. The union is a longer list, never a wrong one.
//
// Where the names come from is the point, and none of it is written down here:
// classes, properties and enums are read live from `ecs::Classes` and
// `ecs::EnumTable`, and the globals and instance members come from
// `script::Runtime::Surface`, which walks a VM. `script/Vocabulary.hpp` carries
// the argument and the two bugs that paid for it.
//
// @tier client

#include <engine/ecs/Classes.hpp>
#include <engine/script/Language.hpp>
#include <engine/script/Vocabulary.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studio {

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

		// An enum set, or one of its members.
		Enum,

		// A global the VM installs, or a member of one.
		Global,

		// A reserved word of the language being written.
		Keyword,

		// An identifier already written in this file.
		Local,

		// The name of an instance in the tree beside this script.
		Child,
	};

	// One row of the popup.
	//
	// @since v0.14
	struct Completion {
		// What replaces the prefix under the caret.
		std::string Text;

		// The dimmed hint on the right: a property's type, a class's parent,
		// what a global is. Empty when the name says everything.
		std::string Detail;

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

		// The expression the separator hangs off — `part` in `part.Anch`,
		// `Enum.Material` in `Enum.Material.Pl`. Empty when there is no
		// separator, or when what precedes it is not a plain dotted chain.
		std::string_view Subject;

		// `.`, `:`, or `\0` when the prefix stands alone.
		//
		// The two are kept apart rather than folded together because Luau uses
		// them for different things: `part:Destroy()` passes the instance and
		// `part.Name` does not, so offering methods after a dot would be
		// offering a call that is missing its first argument.
		char Separator = '\0';

		// Whether the caret is inside a string literal on this line.
		bool InString = false;

		// The dotted name being called, when the caret is inside its first
		// string argument — `Instance.new`, `game:GetService`, `IsA`. This is
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

	// Everything a completion needs that is not the text itself.
	//
	// @since v0.14
	struct CompletionSources {
		engine::script::Language Language = engine::script::Language::Luau;

		// The globals and instance members of a VM of that language, or null
		// when none could be made. Null degrades the list rather than emptying
		// it: classes, properties, enums and keywords still come through.
		const engine::script::ScriptSurface *Surface = nullptr;

		// The names of instances beside this script, for `script.Parent.` and
		// `workspace.`. Gathered by the panel, because reading them means being
		// inside `Universe::Enter` and this function is not.
		std::span<const std::string> Children;
	};

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

	// The classes an author may write where a class name is wanted.
	//
	// **One function, because the class picker and the completion popup are two
	// lists of the same thing.** `mono.studio/AGENTS.md` says the picker never
	// hard-codes a class and that the abstract bases it does name live in one
	// place; `mono.tools/bindings` says copying that list into a second place is
	// how the two would disagree later. So the explorer and this call here.
	//
	// Everything under `Instance`, minus the services — a world has exactly one
	// of each and `scene::InstallServices` puts it there, so offering one is
	// offering a second that nothing resolves — and minus the abstract bases,
	// which the run time would happily mint into rows nothing knows how to draw.
	//
	// @return The class ids, in registration order.
	// @since v0.14
	std::vector<engine::ecs::ClassId> InsertableClasses();

}
