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
#include <string>
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
		CommandKind Kind = CommandKind::Create;

		// Which scene it happened in.
		engine::world::WorldId World;

		// What was edited.
		EditId Subject;

		// Where it hung before, and where it hangs now. `Reparent` uses both;
		// `Create` and `Destroy` use `OldParent` as the place to rebuild under.
		EditId OldParent;
		EditId NewParent;

		// The subtree, for the kinds that have to rebuild one.
		std::string Document;

		// Which property, and what it read before and after.
		engine::core::Name Property;
		engine::game::PropertyValue Before;
		engine::game::PropertyValue After;

		// What to call this in the Edit menu — "Delete Part", not "Destroy".
		std::string Description;
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

		// Reverses the most recent command.
		//
		// **Called from outside `Universe::Enter`**, and enters for itself.
		//
		// @return `true` when a command was reversed. `false` when there was
		//         nothing to undo, or when the command's subject no longer
		//         resolves — which is dropped rather than applied to whatever
		//         now occupies the row.
		bool Undo();

		// Reapplies the most recently undone command.
		//
		// @return `true` when a command was reapplied.
		bool Redo();

		// Throws both stacks away.
		//
		// **What `BeginRun` calls.** Stop restores a snapshot taken before the
		// run, so a stack spanning that boundary would offer to reverse edits
		// the restore has already discarded.
		void Clear();

		// How deep the undo stack is, for tests and the statistics panel.
		//
		// @return The number of commands that can be undone.
		size_t Depth() const {
			return Done.size();
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
		// @param entity What now carries it.
		void Rebind(EditId id, engine::ecs::Entity entity);

		// Pushes onto the undo stack, dropping the oldest past `DEPTH`.
		//
		// **Recording clears the redo stack**, which is the ordinary rule: once
		// you have done something new, the branch you had undone is not
		// reachable any more and offering to redo into it would apply a command
		// against a world that has moved on.
		//
		// @param command The command to record.
		void Push(Command &&command);

		engine::world::Universe *Universe = nullptr;

		std::vector<Command> Done;
		std::vector<Command> Undone;

		// The id table, both ways. Forward is what `Resolve` reads; reverse is
		// what stops `Track` minting a second id for an instance that has one.
		std::unordered_map<uint64_t, engine::ecs::Entity> Entities;
		std::unordered_map<engine::ecs::Entity, uint64_t> Ids;

		uint64_t NextId = 1;
	};
}
