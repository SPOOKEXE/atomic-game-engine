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
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <cstdint>
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

		// How many times this table has been written, which is what makes
		// noticing a change cost nothing.
		//
		// **A counter rather than a hash, because the alternative is paid every
		// tick and this is paid per edit.** `MirrorSourcePrograms` has to know
		// whether the text moved since it last looked, and the only other way to
		// know is to hash every program in the world on every tick - a kilobyte
		// of Luau per script per tick to learn what almost never changes.
		//
		// Deliberately **not serialised**: it counts writes to *this* table in
		// *this* process, so a world that is loaded reads back at zero and the
		// mirror rebuilds itself once. Writing it would put a session's edit
		// count into a save file and make two loads of one game file disagree
		// about a number nothing outside this process may compare.
		//
		// @since v0.15
		uint64_t Generation = 0;

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

	// The program a script instance runs, on the instance itself.
	//
	// **A copy of what the cache holds, and the copy is what a client can be
	// sent.** `SourceCache` is a world resource: one table for the whole world,
	// which is right for storage - many instances share a path - and impossible
	// for the wire, because `replication::Authority::SetInterest` filters
	// *entities*. A resource crossing whole would put `ServerScriptService`'s
	// programs on every client, which is the leak `scene::VisibleToClients`
	// exists to have closed. A row on the instance is hidden by exactly the rule
	// that hides the instance.
	//
	// **Derived, with one writer, which is what keeps it from being a second
	// source of truth.** `MirrorSourcePrograms` fills it from the cache and
	// nothing else may write one; `ReadProgram` reads the cache first wherever
	// there is one, so an editor's unsaved edit cannot lose to a mirror taken a
	// tick ago. That is the arrangement `scene::PreviousTransform` and
	// `gui::Resolved` already have.
	//
	// **Only what a client could run.** A `Script` is the server's and is never
	// mirrored, so its text is not a component and cannot cross however interest
	// is configured - defence that does not depend on a predicate being right.
	//
	// **No property declares it**, so no script can read another script's
	// source. `Instances.hpp` carries why that is the security boundary rather
	// than a preference.
	//
	// @since v0.15
	struct Program {
		// The active source path this text was read for.
		//
		// The freshness key in both directions: the mirror refills a row whose
		// path no longer matches the instance's, and `ReadProgram` ignores a row
		// that names a different path rather than running text the instance has
		// stopped pointing at.
		core::Name Path;

		// The program.
		std::string Text;
	};

	// What `MirrorSourcePrograms` remembers between ticks.
	//
	// One per runtime rather than per world, for the reason
	// `Runtime::RunNewScripts`' record is a member: two VMs over one world would
	// each answer "have I done this" separately, and the second one finds every
	// row already correct and writes nothing.
	//
	// @since v0.15
	struct SourceMirror {
		// The cache generation the rows were last built from.
		uint64_t Generation = 0;
	};

	// Brings every client-runnable script's `Program` row in line with the
	// cache.
	//
	// **The steady-state cost is one 64-bit compare for the world plus a name
	// compare per script**, because nothing here reads a program's bytes unless
	// `SourceCache::Generation` moved or an instance's active path changed. A
	// tick on which no script was edited touches no text at all.
	//
	// A path that cannot be read leaves the instance without a row and is tried
	// again next tick. Remembering the failure would be a second record of which
	// reads failed, and what it would save is one failed `open` per tick per
	// script whose file is missing - which is a broken world either way.
	//
	// **Does nothing in a replica**, where the rows arrived from the authority
	// and the cache is empty: refilling them from whatever this machine happens
	// to have on disk is how a client would run a program the server never sent.
	// `ecs::Store::AdoptOnly` is the test, as it is everywhere else in this
	// module.
	//
	// @param store  The world.
	// @param mirror What the caller remembered from last time.
	// @since v0.15
	void MirrorSourcePrograms(ecs::Store &store, SourceMirror &mirror);

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

	// Reads what one instance runs: cache first, then what arrived on it, then
	// the filesystem.
	//
	// **`ReadSource` answers for a path and this answers for an instance**,
	// which is the difference a replica makes. A client holds no cache and no
	// game file; what it holds is the `Program` row the authority sent, and the
	// only thing that can find one is a lookup that starts from the instance.
	//
	// The order is the load-bearing part. The cache is this world's own record
	// and wins wherever it has an answer, so an unsaved edit in the studio still
	// runs; the row is what a replica has instead of a cache; the filesystem is
	// last, so a client cannot quietly run a file of its own in place of the
	// program it was sent.
	//
	// @param store    The world.
	// @param instance The script instance.
	// @param path     Filled with the active source path, which is also the name
	//                 the program is compiled under.
	// @param out      Filled with the program on success.
	// @param error    Filled with why, on failure.
	// @return `false` when the instance names no path, or names one that is in
	//         neither the cache, nor a row, nor a readable file.
	// @since v0.15
	bool ReadProgram(
		const ecs::Store &store, ecs::Entity instance, core::Name &path, std::string &out, std::string &error
	);
}
