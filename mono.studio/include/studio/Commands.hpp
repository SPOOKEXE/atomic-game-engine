#pragma once

// Undo and redo, as a log of what the author asked for.
//
// **The editor applied every action straight to the store until v0.7**, which
// `mono.studio/AGENTS.md` recorded as deferred with the reason attached: a
// half-undo covering property edits and not deletions would be worse than none,
// because the one thing an undo stack has to be is trustworthy. This is the
// whole of it rather than the easy half.
//
// ## Nothing here is world state
//
// The log lives in the editor, never enters a store, never crosses a bus and
// never reaches a snapshot — the rule at the top of `Editor.hpp`. That is not
// tidiness: Stop throws the universe away and restores a snapshot, so a log that
// travelled with the world would come back describing edits the restore has
// already discarded. `Editor::BeginRun` clears it instead, and the transport
// says so.
//
// ## Why it does not hold an `ecs::Entity`
//
// An entity handle is an index and a generation into **one** store. Undoing a
// delete cannot give the handle back — `ReadInstanceDocument` builds a new row
// and the new row has a new handle — so a log holding entities would be correct
// exactly until the first undone deletion, after which every later command in
// it would name something else. That is the failure mode where undo silently
// edits the wrong instance, which is worse than refusing.
//
// So a command names an `EditId`, and the log keeps the map from that to
// whatever entity currently carries it. Rebuilding rebinds; nothing else has to
// know. `Editor::MoveInstanceToWorld` already had to solve this once — moving
// across worlds changes the handle and the selection is repointed — and this is
// that mechanism generalised rather than a second one.
//
// **Only the root of a rebuilt subtree is rebound, and the limit is stated
// rather than hidden.** A document does not carry the editor's ids, so undoing
// the deletion of a parent gives its children new handles that no id names. A
// command recorded earlier against one of those children therefore resolves to a
// dead handle and is dropped — which is safe, because an entity carries a
// generation and a recycled index does not answer `Alive`. What it is not is
// complete: the honest reading is that undo reaches back past a restored subtree
// for the subtree itself and not for edits *inside* it. Closing that needs the
// document format to carry an identity, which is `StableId` in `MCP.md`'s
// `project` server and does not exist yet.
//
// ## What records, and what deliberately does not
//
// Recording happens inside the `Universe::Enter` where the mutation happens,
// because a delete has to be photographed **before** it is a delete. Undo and
// redo are called from outside one and enter for themselves, which is the same
// split every panel already makes.
//
// **The editor's own writes are not author actions and must never appear here.**
// `EnsureViewerCamera` creates a camera per viewport panel and
// `ReleaseViewerCamera` destroys it; a new game installs an example script.
// None of those is something anybody asked for, and an undo that deleted the
// camera you are looking through would be indistinguishable from a crash. They
// are omitted by not calling the recorder, which is the only mechanism that
// cannot be forgotten in one direction and remembered in the other — there is no
// flag to get wrong.
//
// @tier L12 · client

#include <engine/ecs/Entity.hpp>
#include <engine/game/Values.hpp>
#include <engine/world/Universe.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace studio {

	// An editor-owned name for an instance that outlives the instance.
	//
	// Opaque and monotonic. Zero is the null id and resolves to no entity, which
	// is what a command whose subject has genuinely gone reads as.
	//
	// @since v0.7
	struct EditId {
		// The id itself. Zero is the null id and resolves to no entity.
		uint64_t Value = 0;

		// Reports whether two ids name the same tracked instance.
		//
		// @param other The id to compare.
		// @return `true` when both values are equal.
		constexpr bool operator==(const EditId &other) const {
			return Value == other.Value;
		}

		// Tests only whether this is the null id.
		//
		// @return `false` only for the null id.
		constexpr explicit operator bool() const {
			return Value != 0;
		}
	};

	// What one entry in the log did.
	//
	// @since v0.7
	enum class CommandKind : uint8_t {
		// An instance came into being. Undo destroys it, redo rebuilds it.
		Create,

		// An instance went away. Undo rebuilds it, redo destroys it.
		Destroy,

		// An instance changed parent within one world.
		Reparent,

		// One property of one instance changed.
		Property,
	};

	// What to do with a recording that has finished.
	//
	// Roblox's `Enum.FinishRecordingOperation`, and the three values are the
	// three answers there are: keep it as its own step, throw it away, or fold
	// it into the step before.
	//
	// **A closed list whose ordinal reaches a wire** — a committed waypoint is
	// what team create replicates — so a value may be added at the end and none
	// may be reordered.
	//
	// @since v0.13
	enum class FinishOperation : uint8_t {
		// Keep it. The recording becomes one waypoint that one undo reverses.
		Commit = 0,

		// Throw it away, and put back what it changed.
		//
		// **Reverted rather than merely forgotten.** A plugin that cancels has
		// decided its edit should not have happened, and a cancel that left the
		// changes in place while removing the only way to undo them would be
		// the worst of both.
		Cancel = 1,

		// Fold it into the waypoint before it.
		//
		// For an edit that continues one already recorded — a drag that
		// resumes, a property nudged again — where two steps in the history
		// would be two presses of Ctrl+Z for one action.
		Append = 2,
	};

	// Returns a stable, human-readable name for a finish operation.
	//
	// @param operation The operation to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(FinishOperation operation);

	// One undoable edit.
	//
	// **A document rather than a component list**, for `Create` and `Destroy`.
	// `game::WriteInstanceDocument` already carries a subtree with the script
	// text it names and nothing else, it is the format a save uses, and it is
	// therefore the one representation that cannot fall behind what an instance
	// is made of. A hand-written capture would be a second answer to "what is
	// this instance" and would go stale the first time a module added a
	// component.
	//
	// @since v0.7
	struct Command {
		// Which edit this is, and therefore which of the fields below carry
		// anything.
		CommandKind Kind = CommandKind::Create;

		// Which scene it happened in.
		engine::world::WorldId World;

		// What was edited.
		EditId Subject;

		// Where it hung before. `Reparent` uses this, and `Create` and `Destroy`
		// use it as the place to rebuild under.
		EditId OldParent;

		// Where it hangs now. `Reparent` only.
		EditId NewParent;

		// The subtree, for the kinds that have to rebuild one.
		std::string Document;

		// Which property changed.
		engine::core::Name Property;

		engine::game::PropertyValue Before; // What it read before the edit.
		engine::game::PropertyValue After;	// What it reads after it.

		// What to call this in the Edit menu — "Delete Part", not "Destroy".
		std::string Description;

		// Which waypoint this belongs to.
		//
		// **The unit undo and redo actually move by, and the unit team create
		// replicates.** A command recorded outside a recording gets a waypoint
		// of its own, so a log that nobody groups behaves exactly as it did
		// before this field existed — one command, one press of Ctrl+Z. A
		// recording gives every command inside it one number, and then a
		// plugin's "make these forty parts neon" is one step rather than forty.
		//
		// Monotonic within one log and never reused, so a waypoint that was
		// undone and then superseded cannot be confused with a later one.
		//
		// @since v0.13
		uint64_t Waypoint = 0;
	};

	// A recording that has been started and not yet finished.
	//
	// @since v0.13
	struct Recording {
		// The identifier `TryBeginRecording` handed out. Empty for none.
		//
		// **Opaque text rather than a number**, because it is what a script
		// holds between two calls and a number invites arithmetic on it.
		std::string Identifier;

		// What the action is called, for logging.
		std::string Name;

		// What to show a person. Falls back to `Name`.
		std::string DisplayName;

		// The waypoint every command recorded during it is stamped with.
		uint64_t Waypoint = 0;

		// How many commands the log held when it started, so a cancel knows
		// how far to reverse and an append knows what it is folding into.
		size_t DepthAtStart = 0;

		// Whether this names a recording in progress.
		//
		// @return `true` when an identifier was issued.
		bool InProgress() const {
			return !Identifier.empty();
		}
	};

	// The undo and redo stacks, and the id table that makes them survive a
	// rebuild.
	//
	// @since v0.7
	class CommandLog {
	  public:
		// How many commands are kept.
		//
		// **Bounded rather than unbounded, and the bound is depth rather than
		// bytes.** A `Destroy` of a large subtree carries a document, so an
		// unbounded log is unbounded memory held by an editor that is otherwise
		// careful about it. Two hundred is far past what anybody reaches back
		// through by hand and small enough that the worst case is a rounding
		// error against one world.
		static constexpr size_t DEPTH = 200;

		// Builds a log over a universe.
		//
		// @param universe The universe whose worlds it will edit. Must outlive
		//                 the log.
		explicit CommandLog(engine::world::Universe &universe) : Universe(&universe) {}

		// Gives an instance an id, or returns the one it already has.
		//
		// @param world    The scene it lives in.
		// @param instance The instance.
		// @return Its id, or the null id when the entity is null.
		EditId Track(engine::world::WorldId world, engine::ecs::Entity instance);

		// Issues an id that names nothing yet.
		//
		// **What an arriving `Create` needs.** A foreign create rebuilds a
		// subtree that does not exist here, so there is no entity to `Track` —
		// the id has to exist first and `ApplyForeign` binds it to whatever the
		// rebuild produced. Minting one for a command that is then dropped
		// costs a number, and the numbers are 64 bits wide.
		//
		// @return A fresh id, bound to nothing.
		// @since v0.13
		EditId Mint();

		// Finds what an id currently names.
		//
		// **May return a handle that is no longer live**, and callers ask
		// `Store::Alive` rather than trusting it. The log unbinds an id only
		// when it performs the destroy itself; a caller that records a delete
		// and then performs it leaves the id pointing at a dead handle. That is
		// safe because an entity carries a generation, so a recycled index
		// answers `Alive` with `false` — and it is the reason every apply path
		// here asks before writing.
		//
		// @param id The id to resolve.
		// @return The entity, or `NULL_ENTITY` when nothing carries the id.
		engine::ecs::Entity Resolve(EditId id) const;

		// Records an instance having been created.
		//
		// **Called after the create, from inside its `Enter`.** The document is
		// taken now so that redo has something to rebuild from without the
		// instance needing to still exist at redo time.
		//
		// @param store       The world's store, already entered.
		// @param world       Which scene.
		// @param created     The new instance.
		// @param description What to call it in the Edit menu.
		void RecordCreate(
			engine::ecs::Store &store,
			engine::world::WorldId world,
			engine::ecs::Entity created,
			std::string description
		);

		// Records an instance about to be destroyed.
		//
		// **Called before the destroy, from inside its `Enter`**, because after
		// it there is nothing left to photograph. Getting this order wrong is an
		// undo that restores an empty document, which reads as "undo did
		// nothing" rather than as a fault.
		//
		// @param store       The world's store, already entered.
		// @param world       Which scene.
		// @param doomed      The instance that is about to go.
		// @param description What to call it in the Edit menu.
		void RecordDestroy(
			engine::ecs::Store &store,
			engine::world::WorldId world,
			engine::ecs::Entity doomed,
			std::string description
		);

		// Records a reparent within one world.
		//
		// @param world       Which scene.
		// @param instance    What moved.
		// @param from        The parent it had. May be null.
		// @param to          The parent it has now. May be null.
		// @param description What to call it in the Edit menu.
		void RecordReparent(
			engine::world::WorldId world,
			engine::ecs::Entity instance,
			engine::ecs::Entity from,
			engine::ecs::Entity to,
			std::string description
		);

		// Records one property write.
		//
		// **Refuses a write that changed nothing**, because a property panel
		// submits an edit on every keystroke that parses and a log full of
		// no-op entries is an undo key that has to be pressed forty times to
		// reach the last thing that mattered.
		//
		// @param world       Which scene.
		// @param instance    What was edited.
		// @param property    Which property.
		// @param before      What it read before.
		// @param after       What it reads now.
		// @param description What to call it in the Edit menu.
		void RecordProperty(
			engine::world::WorldId world,
			engine::ecs::Entity instance,
			engine::core::Name property,
			const engine::game::PropertyValue &before,
			const engine::game::PropertyValue &after,
			std::string description
		);

		// --- recordings -------------------------------------------------------
		//
		// Roblox's `ChangeHistoryService` shape, and it is the shape this log
		// wanted anyway. A recording is a **named, atomic group of commands**:
		// one undo reverses all of it, and — once team create exists — one
		// message carries all of it. Without grouping, "make these forty parts
		// neon" is forty presses of Ctrl+Z locally and forty messages on the
		// wire, and the second is worse because a peer could apply half.
		//
		// **One recording at a time, per log.** Roblox allows one per plugin
		// and refuses the second; this log is the editor's single history, so
		// the same rule applies to it as a whole. Nesting would need an
		// answer to what a cancel of the outer one does to a committed inner
		// one, and there is no answer that is not surprising.

		// Starts a recording.
		//
		// @param name        What the action is called, for logging.
		// @param displayName What to show a person. Empty uses `name`.
		// @return The identifier to pass to `FinishRecording`, or nothing when
		//         a recording is already in progress or the log is disabled.
		//         **Nothing is a refusal to record, not a licence to edit
		//         anyway** — a caller that ignores it and edits produces changes
		//         that land in whatever recording is already open.
		// @since v0.13
		std::optional<std::string> TryBeginRecording(std::string name, std::string displayName = {});

		// Finishes the recording `identifier` names.
		//
		// @param identifier What `TryBeginRecording` returned. Ignored for
		//        `Cancel`, so a caller can abandon its own recording without
		//        having kept the identifier — Roblox's rule, and it is the one
		//        case where not knowing is the normal situation.
		// @param operation  What to do with it.
		// @return `false` when no recording is in progress, or when the
		//         identifier does not match one that is.
		// @since v0.13
		bool FinishRecording(std::string_view identifier, FinishOperation operation);

		// Whether a recording is in progress.
		//
		// @param identifier A specific one to ask about, or empty for any.
		// @return Whether a matching recording is open.
		// @since v0.13
		bool IsRecordingInProgress(std::string_view identifier = {}) const;

		// The recording in progress, for a panel that shows one.
		//
		// @return The recording. `InProgress` is false when there is none.
		// @since v0.13
		const Recording &InFlight() const {
			return Open;
		}

		// Closes whatever waypoint is open and starts a new one.
		//
		// Roblox's `SetWaypoint`, which its own documentation says will be
		// deprecated in favour of recordings. It is here because plugins
		// written against it exist, and because it is one line over the
		// machinery recordings already need: the next command recorded gets a
		// fresh waypoint whether or not anything asked for one.
		//
		// **Not a place to record from.** It marks a cut in a stream of edits
		// that have already happened, so calling it before an edit does nothing
		// the next command would not have done anyway.
		//
		// @param name What to call the waypoint that just closed. Applied to
		//        the commands in it, so the Edit menu names the action rather
		//        than the last property that changed inside it.
		// @since v0.13
		void SetWaypoint(std::string name);

		// Collapses the whole history into one base waypoint.
		//
		// Nothing is left to undo or redo and nothing is reverted — this
		// establishes a clean floor rather than restoring one. What it is for
		// is the moment after a load or a save, where reaching back past the
		// current state means reaching into a document that is no longer open.
		//
		// @since v0.13
		void ResetWaypoints();

		// Turns recording on or off.
		//
		// **Off clears both stacks and does not repopulate**, which is Roblox's
		// behaviour and is the safe one: a log that kept recording while
		// disabled would offer, the moment it was re-enabled, to undo across a
		// period it had been told not to watch.
		//
		// @param enabled Whether to record.
		// @since v0.13
		void SetEnabled(bool enabled);

		// Whether anything is being recorded.
		//
		// @return `true` unless `SetEnabled(false)` was called.
		// @since v0.13
		bool Enabled() const {
			return Recording_;
		}

		// --- watching ---------------------------------------------------------

		// What the log tells whoever is listening.
		//
		// **One seam for four listeners**, and they are genuinely different
		// things: the scripting service raises `OnUndo` and friends, team
		// create puts a committed waypoint on the wire, the History panel
		// repaints, and a test asserts. A log that called any one of them by
		// name would be a log that knows what a plugin is.
		//
		// @since v0.13
		struct Watcher {
			// A recording started.
			std::function<void(const Recording &)> RecordingStarted;

			// A recording finished, however it finished.
			std::function<void(const Recording &, FinishOperation)> RecordingFinished;

			// A waypoint was committed, with every command in it.
			//
			// **The replication seam.** What this hands over is exactly what a
			// peer has to apply to arrive at the same document, and it is
			// handed over once per waypoint rather than once per command
			// because a peer that applied half of a group would be showing a
			// state the author never saw.
			std::function<void(uint64_t, std::span<const Command>)> Committed;

			// A waypoint was undone, by name.
			std::function<void(std::string_view)> Undone;

			// A waypoint was redone, by name.
			std::function<void(std::string_view)> Redone;
		};

		// Sets who is listening. One watcher; the editor fans out.
		//
		// @param watcher The callbacks. Any of them may be empty.
		// @since v0.13
		void Watch(Watcher watcher) {
			Listener = std::move(watcher);
		}

		// Points an id at a local instance.
		//
		// **What makes a foreign command applicable at all.** An `EditId` is
		// this log's own name for an instance, so a command that arrived from
		// another editor names ids this log has never issued. Before applying
		// one, whoever carries it resolves each id to a local instance — by
		// path, which is the only identity two editors share — and says so
		// here.
		//
		// A `Create` binds its own subject as a side effect of rebuilding, so
		// only the ids a command *reads* have to be adopted: the parent it
		// hangs under, and the subject of anything that is not a create.
		//
		// @param id       The foreign id.
		// @param world    Which local scene it lives in.
		// @param instance What it names here.
		// @since v0.13
		void Adopt(EditId id, engine::world::WorldId world, engine::ecs::Entity instance);

		// Records commands that arrived from somewhere else.
		//
		// **The other end of `Watcher::Committed`, and it must not enter the
		// undo stack.** Somebody else's edit is not a step in *this* author's
		// history: Ctrl+Z is a promise about what you did, and an editor that
		// reversed a colleague's change because you pressed it once too often
		// would be an editor nobody could work in. Roblox's team create makes
		// the same split and it is the right one.
		//
		// The ids in the commands are the sender's, so they are resolved
		// through this log's own table by the same `Track`/`Rebind` mechanism
		// an undo uses — a peer that has never seen the instance creates it
		// from the document and binds the id.
		//
		// @param commands One waypoint's worth, in the order they were made.
		// @return How many landed. A command whose subject does not resolve is
		//         dropped and counted out rather than applied to whatever now
		//         occupies the row.
		// @since v0.13
		size_t ApplyForeign(std::span<const Command> commands);

		// Reports whether there is anything to undo.
		//
		// @return `true` when the undo stack is not empty.
		bool CanUndo() const {
			return !Done.empty();
		}

		// Reports whether there is anything to redo.
		//
		// @return `true` when the redo stack is not empty.
		bool CanRedo() const {
			return !Undone.empty();
		}

		// What undo would reverse, for the menu item's label.
		//
		// @return The description, or empty when there is nothing to undo.
		std::string_view NextUndo() const {
			return Done.empty() ? std::string_view() : std::string_view(Done.back().Description);
		}

		// What redo would reapply, for the menu item's label.
		//
		// @return The description, or empty when there is nothing to redo.
		std::string_view NextRedo() const {
			return Undone.empty() ? std::string_view() : std::string_view(Undone.back().Description);
		}

		// Reverses the most recent waypoint.
		//
		// **Called from outside `Universe::Enter`**, and enters for itself.
		//
		// **A waypoint rather than a command, since v0.13.** A command recorded
		// outside a recording is a waypoint of its own, so a log nobody groups
		// behaves exactly as it did before — and a recording is one press
		// rather than one per property it touched, which is the whole reason
		// recordings exist.
		//
		// Every command in the waypoint is reversed, newest first, because the
		// commands inside one are ordered and reversing them forwards would
		// re-create a parent after restoring the child that hangs from it.
		//
		// @return `true` when anything was reversed. `false` when there was
		//         nothing to undo, or when every command's subject no longer
		//         resolves — which is dropped rather than applied to whatever
		//         now occupies the row.
		bool Undo();

		// Reapplies the most recently undone waypoint.
		//
		// @return `true` when anything was reapplied.
		bool Redo();

		// Throws both stacks away.
		//
		// **What `BeginRun` calls.** Stop restores a snapshot taken before the
		// run, so a stack spanning that boundary would offer to reverse edits
		// the restore has already discarded.
		void Clear();

		// Drops every command naming one scene, and forgets its ids.
		//
		// **What `EndRun` calls, and it is the other half of `Clear`.** A scene
		// can be manipulated *while it runs* — the gizmo and the explorer both
		// work during a play test, deliberately — so edits are recorded during
		// the run. Stop then destroys that world and rebuilds it from the
		// snapshot, which gives every instance in it a new handle.
		//
		// Those commands are not merely stale, they are unreversible: their
		// subjects resolve to dead handles and every one of them would be
		// dropped on the way past. Leaving them in would make the Edit menu
		// offer "Undo Move" and then refuse it, which reads as undo being
		// broken rather than as the restore having already undone it.
		//
		// **One scene, not the whole log.** Another world may have been edited
		// throughout and never run; its history has nothing to do with this
		// restore. That is the same argument `EndRun` already makes for leaving
		// other scenes' script tabs alone.
		//
		// @param world The scene whose commands to forget.
		void Forget(engine::world::WorldId world);

		// How deep the undo stack is, for tests and the statistics panel.
		//
		// @return The number of commands that can be undone.
		size_t Depth() const {
			return Done.size();
		}

		// The commands that can be reversed, oldest first.
		//
		// **For the History panel, which is why this is a span and not a
		// copy.** The panel reads it every frame — `AGENTS.md`'s rule that a
		// panel caches nothing the model owns — and the last element is what
		// `Undo` would reverse next.
		//
		// @return The undo stack, valid until the next command.
		std::span<const Command> Undoable() const {
			return Done;
		}

		// The commands that have been reversed and can be reapplied.
		//
		// **Ordered so the last element is what `Redo` does next**, which is
		// the reverse of how a person reads a history list. The panel presents
		// it accordingly rather than making the reader hold that in their head.
		//
		// @return The redo stack, valid until the next command.
		std::span<const Command> Redoable() const {
			return Undone;
		}

	  private:
		// Applies one command in one direction.
		//
		// @param command The command.
		// @param forward `true` to apply it, `false` to reverse it.
		// @return `true` when it landed.
		bool Apply(Command &command, bool forward);

		// Points an id at a different entity, after a rebuild gave it one.
		//
		// @param id     The id to rebind.
		// @param world  Which scene the new handle lives in.
		// @param entity What now carries it.
		void Rebind(EditId id, engine::world::WorldId world, engine::ecs::Entity entity);

		// Pushes onto the undo stack, dropping the oldest past `DEPTH`.
		//
		// **Recording clears the redo stack**, which is the ordinary rule: once
		// you have done something new, the branch you had undone is not
		// reachable any more and offering to redo into it would apply a command
		// against a world that has moved on.
		//
		// @param command The command to record.
		void Push(Command &&command);

		// Reverses and drops every command back to `depth`.
		//
		// What a cancelled recording does. Separate from `Undo` because it
		// crosses waypoint boundaries deliberately and must not put anything on
		// the redo stack — a cancel is "this never happened", not "step back".
		//
		// @param depth How deep the undo stack was before the recording.
		void RollBackTo(size_t depth);

		// The next waypoint number, and it never repeats.
		uint64_t NextWaypoint();

		engine::world::Universe *Universe = nullptr;

		std::vector<Command> Done;
		std::vector<Command> Undone;

		// The recording in progress, if any.
		Recording Open;

		// Where waypoint numbers come from. Monotonic, so a waypoint that was
		// undone and superseded cannot be confused with a later one.
		uint64_t Waypoints = 0;

		// How deep the stack was at the last cut.
		//
		// **What `SetWaypoint` merges back to.** Roblox's rule is that the
		// changes between two waypoints are one undo, and this is where "since
		// the previous one" is kept. Advanced by an undo, a redo, a recording
		// and a reset, so a merge can never reach across a boundary into
		// commands the stack no longer holds in the order they were made.
		size_t Cut = 0;

		// Whether anything is recorded at all. See `SetEnabled`.
		bool Recording_ = true;

		// Set while `Undo`, `Redo` or `ApplyForeign` is writing, so the writes
		// they make are not themselves recorded.
		//
		// **The one flag in this class, and it earns its place.** Undo applies
		// a command by performing the opposite edit, and every path that
		// performs an edit is a path that records one — without this, undoing
		// a delete records a create, which the next undo then reverses, and
		// Ctrl+Z toggles one instance in and out of existence for ever.
		bool Replaying = false;

		Watcher Listener;

		// What an id currently names: a world and a handle within it.
		//
		// **The world is part of the answer, not context.** An `ecs::Entity` is
		// an index and a generation into *one* store, so the same numeric handle
		// exists in every world at once — the first instance in one scene and
		// the first in another are the same number. A table keyed on the handle
		// alone therefore hands one id to two different instances, and undo
		// applies an edit recorded in one scene to whatever happens to share its
		// handle in another.
		//
		// That is the failure this header's opening note describes and the
		// reverse map walked straight into. Found by a test that recorded in two
		// worlds — with one world it is invisible, which is why it is worth the
		// pair.
		struct Bound {
			engine::world::WorldId World;
			engine::ecs::Entity Instance;

			bool operator==(const Bound &other) const {
				return World == other.World && Instance == other.Instance;
			}
		};

		// Hashes a world and handle together.
		struct BoundHash {
			size_t operator()(const Bound &bound) const {
				const size_t world = std::hash<uint32_t>{}(bound.World.Index);
				const size_t entity = std::hash<engine::ecs::Entity>{}(bound.Instance);
				return world ^ (entity + 0x9e3779b97f4a7c15ULL + (world << 6) + (world >> 2));
			}
		};

		// The id table, both ways. Forward is what `Resolve` reads; reverse is
		// what stops `Track` minting a second id for an instance that has one.
		std::unordered_map<uint64_t, Bound> Entities;
		std::unordered_map<Bound, uint64_t, BoundHash> Ids;

		uint64_t NextId = 1;
	};
}
