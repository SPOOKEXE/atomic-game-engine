#pragma once

// Where a script's program text lives when it does not live on disk.
//
// **`Instances.hpp` predicted this field and named the version that would need
// it.** `Source::Path` is a `core::Name` because a component must be trivially
// copyable to be a column, so it cannot hold an unbounded string - and until
// v0.7 the path was resolved straight against the filesystem. That was correct
// for `--script Rings.luau` and wrong for everything a studio does: an editor
// that saved to a file per keystroke would be an editor whose undo history is
// the filesystem's, and a game file that carried paths instead of programs
// would be a game file that does not contain the game.
//
// So the text is a **world resource** rather than a component: one table per
// world, keyed by the same path the `Source` component names, consulted before
// the filesystem. That is the arrangement `scene::SurfaceTable` already uses
// for the same reason - a fact shared by many rows belongs once per world, not
// once per row - and it is why this is serialisable with an explicit writer
// rather than by its object representation.
//
// **The path is still the identity, and the cache is an override rather than a
// replacement.** A world with no cache reads files exactly as v0.6 did, which
// is what keeps `--script` working and what lets a game reference an asset it
// did not author. A path present in the cache never touches the disk, which is
// what makes an unsaved edit run.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// One cached program.
	//
	// @since v0.7
	struct SourceRow {
		// The asset-relative path this text stands in for, exactly as a
		// `Source` component spells it.
		core::Name Path;

		// The program.
		std::string Text;
	};

	// A world's script text, keyed by path.
	//
	// A flat vector searched linearly, for `SurfaceTable`'s reasons rather than
	// out of imitation: a world holds tens of scripts, a `core::Name` compare
	// is an integer compare, and insertion order is program order - so two
	// loads of one game file hold an identical table and a snapshot of it is
	// byte-identical. A hash map would give the second property up to improve a
	// lookup nobody has measured.
	//
	// @since v0.7
	struct SourceCache {
		// The programs, in the order they were first set.
		//
		// Public for the serialiser this module registers, which walks it.
		std::vector<SourceRow> Rows;

		// Records a program, replacing any existing text for that path.
		//
		// Replacing in place rather than appending, so a script saved twice
		// holds one row rather than a history of them.
		//
		// @param path The asset-relative path.
		// @param text The program.
		void Set(core::Name path, std::string_view text);

		// The program cached for a path, or nothing.
		//
		// **No get-or-default, for `SurfaceTable`'s reason.** A miss means "read
		// the file", and a miss silently returning an empty program would be a
		// script that runs, does nothing and reports success.
		//
		// @param path The path to resolve.
		// @return A pointer into `Rows`, invalidated by the next `Set`, or
		//         `nullptr` when nothing is cached for that path.
		const std::string *Find(core::Name path) const;

		// Forgets a path, if it is cached.
		//
		// @param path The path to drop.
		// @return `true` when a row was removed.
		bool Erase(core::Name path);

		// The number of cached programs.
		//
		// @return The current row count.
		size_t Count() const {
			return Rows.size();
		}
	};

	// Registers `SourceCache` and the `Source` component under stable names.
	//
	// Idempotent and process-wide, like every other registration. Called by
	// `ScriptClass()` so a caller that registers the class tree cannot end up
	// with a cache the snapshot writer refuses.
	void RegisterScriptComponents();

	// Reads a script's program, cache first and filesystem second.
	//
	// **One function, so there is one answer.** A second place that resolved a
	// path would be a second place to forget the cache, and the symptom would
	// be an editor whose unsaved changes run in one code path and not another.
	//
	// @param store The world, whose `SourceCache` resource is consulted first.
	// @param path  The asset-relative or absolute path.
	// @param out   Filled with the program on success.
	// @param error Filled with why, on failure.
	// @return `false` when the path is not cached and the file could not be
	//         read.
	bool ReadSource(const ecs::Store &store, core::Name path, std::string &out, std::string &error);
}
