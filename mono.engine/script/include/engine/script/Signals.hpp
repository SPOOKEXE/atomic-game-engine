#pragma once

// What `:Connect` returns, and the ordering rules both VMs obey.
//
// **The list is shared and the callables are not.** A Luau connection holds a
// registry ref and a JavaScript one holds a `JSValue`; neither type can appear
// here, because `script/AGENTS.md` keeps every VM type inside its own source
// file. What *can* be shared - and has to be - is everything about ordering:
// which connection fires first, what a `:Disconnect` during a fire does, and
// what a connection id is. Those are the rules a recording depends on, and two
// hand-written copies of them would agree until the first time one was fixed.
//
// So a callable crosses this interface as an opaque `CallbackRef`. Each VM
// decides what the integer means and hands it back untouched.
//
// **Ordering is insertion order and nothing else.** Not a priority, not a
// per-instance bucket walked in hash order: a hash map's iteration order is
// exactly the trap `docs/retired/SCRIPT_CONCURRENCY.md` §3 names for the codec, and it
// is the same trap here. Two runs of one script must call the same functions in
// the same sequence.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// Which signal a connection sits on.
	//
	// @since v0.6
	enum class SignalKind : uint8_t {
		// `RunService.Heartbeat`. One per world, no subject.
		Heartbeat,

		// `instance.Changed`. The subject is the instance being watched.
		Changed,

		// `instance:GetPropertyChangedSignal(name)`. The subject is the
		// instance; which property is carried by the connection rather than by
		// the key, because a per-property key would mean a map entry per
		// property per instance for a surface most scripts never touch.
		PropertyChanged,

		// `UserInputService` events. One per world and filtered by their public
		// signal name. Separate from `PropertyChanged` because controller state is
		// a resource change, not an instance property event, and the generic change
		// pump supplies a different argument shape.
		Input,

		// `instance.ChildAdded` and `instance.ChildRemoved`. The subject is the
		// parent, and the handler is called with the child.
		ChildAdded,
		ChildRemoved,

		// `instance.DescendantAdded`. The subject is any ancestor of what
		// arrived, and the handler is called with it.
		//
		DescendantAdded,

		// `instance.DescendantRemoving`. The subject is the ancestor about to
		// lose it, and the handler is called with what is leaving.
		//
		// **The one signal here that is not queued, and the only one that
		// cannot be.** Everything else on this list is recorded and delivered
		// at the next barrier - `Changes.hpp` sets out why a signal must not
		// fire from inside a write. This one's whole contract is that the
		// handler is called *while the thing is still there*, which a queue
		// drained a tick later cannot offer, because by then it has gone.
		//
		// So it is dispatched synchronously from `Store::OnDescendantRemoving`,
		// at the top of the operation and before a single link moves. That is a
		// different position from the one `Changes.hpp` rules out: nothing is
		// half-written, because nothing has been written.
		DescendantRemoving,

		// `instance.AncestryChanged`. The subject is the instance whose chain
		// of parents changed, which is what moved *and everything under it*,
		// and the handler is called with the instance and its new parent.
		AncestryChanged,

		// `Players.PlayerAdded` and `Players.PlayerRemoving`. The subject is
		// the `Players` service, and the handler is called with the player.
		//
		// **These are the tree's signals wearing the name a game expects.** A
		// `Player` is an instance parented under `Players`, so arriving and
		// leaving are already a `ChildAdded` and a removal - what these add is
		// the filter, which is not cosmetic: `Players` may hold something that
		// is not a player, and a handler that had to test the class itself is a
		// handler every game writes and one of them forgets.
		//
		// **They are two different mechanisms, matching the pair they wrap.**
		// `PlayerAdded` is recorded and delivered at the barrier like every
		// other tree change. `PlayerRemoving` is dispatched synchronously from
		// `Store::OnDescendantRemoving`, before anything is unlinked, for the
		// reason `DescendantRemoving` is: the whole point of the signal is that
		// the player is still there when the handler runs - a game saving
		// somebody's progress on the way out has nothing to save otherwise.
		PlayerAdded,
		PlayerRemoving,

		// `player.CharacterAdded` and `player.CharacterRemoving`. The subject is
		// the `Player`, and the handler is called with the character model.
		//
		// **Not the tree's, unlike the pair above them.** A player arriving *is*
		// a reparent, so `PlayerAdded` is `ChildAdded` with a filter; a character
		// is a `Model` under `Workspace` and the link to its player is a
		// component, so nothing in the tree changes shape when somebody
		// respawns. What records it is `scene::SetPlayerCharacter` - the one
		// door every assignment goes through - and `scene::TakeCharacterChanges`
		// is what both pumps drain.
		//
		// **Two different mechanisms, matching the pair above them.**
		// `CharacterAdded` is recorded by `scene::SetPlayerCharacter` and
		// delivered at the barrier like every other arrival. `CharacterRemoving`
		// rides `Store::OnDescendantRemoving` instead, for the reason
		// `PlayerRemoving` does: dying in this engine *is* the model being
		// destroyed, so a queue drained a tick later would hand a handler an
		// instance it cannot read a single property off.
		//
		// The queue still records removals, and the pump fires only the ones
		// whose model is **still alive** - which is exactly the case the hook did
		// not cover: `player.Character = nil` releases a body without destroying
		// it. The two are disjoint, so nothing fires twice.
		//
		// @since v0.17
		CharacterAdded,
		CharacterRemoving,

		// --- the 2D tree's input, from `gui::Router` ------------------------
		//
		// **The six below are one mechanism and it is not the tree's.** Every
		// signal above is recorded by the store and fanned out at the barrier;
		// these arrive from outside the world entirely - a host polls a pointer,
		// `gui::Router` decides what that means, and the events are handed to
		// `Runtime::DeliverGuiEvents`. Nothing in `ecs` knows they happened.
		//
		// They are still *queued* rather than fired on arrival, and that is the
		// same decision `Changes.hpp` argues for one door along: a handler may
		// destroy the instance it was called about, and the router is mid-walk
		// of a compiled draw list that names it. Delivering at the next
		// heartbeat puts them where every other resume happens, which is what
		// `docs/retired/SCRIPT_CONCURRENCY.md` §1 permits and what keeps two runs of one
		// recording in step.
		//
		// The subject is the element, for all six.

		// `guiObject.Activated` - pressed and released on the same element.
		// The one nearly every script connects to.
		GuiActivated,

		// `guiObject.InputBegan` - the button went down over it.
		GuiInputBegan,

		// `guiObject.InputEnded` - the button came up. Fired on the element the
		// press *began* on, which is `gui::Router`'s rule and is what makes a
		// drag off a button and back one interaction rather than two.
		GuiInputEnded,

		// `guiObject.MouseEnter` - the pointer entered its rectangle.
		GuiMouseEnter,

		// `guiObject.MouseLeave` - it left. Fired before the matching
		// `MouseEnter` on whatever it moved onto, so a handler that puts
		// something back on leave runs before the one reacting to the arrival.
		GuiMouseLeave,

		// `guiObject.MouseMoved` - it moved while over the element.
		GuiMouseMoved,

		// `uiDragDetector.DragStart`, `.DragContinue` and `.DragEnd`.
		//
		// **The subject is the *detector* and not the element it moves**, which
		// is Roblox's arrangement and is what lets two detectors on one panel
		// mean two gestures. A script connects to the modifier it authored, and
		// the element it drags is that modifier's parent.
		//
		// @since v0.18
		GuiDragBegan,
		GuiDragContinue,
		GuiDragEnded,

		// `textBox.Focused` - a press landed on it and the keyboard is now its.
		//
		// **The only pair here that is about the keyboard rather than the
		// pointer, and it still arrives from the router**, because a press is
		// what decides where typing goes. `gui::EventKind::Focused` is the
		// event; `GuiServiceState::FocusedTextBox` is where the fact rests, and
		// `UserInputService:GetFocusedTextBox` reads it there rather than
		// counting these.
		//
		// @since v0.15
		GuiFocused,

		// `textBox.FocusLost` - a press landed somewhere else.
		//
		// **The handler is called with `enterPressed` and nothing after it.**
		// Roblox passes a second argument, the `InputObject` that took the focus
		// away, and this engine has no such object to pass: the router deals in
		// `gui::GuiEvent` and `script::InputReport` is built by the *input* pump
		// from a different frame's state, so anything handed over here would be
		// a value made up at the call site.
		//
		// **`enterPressed` is `GuiEvent::Entered`, and the two producers are what
		// make it worth an argument.** A press landing elsewhere releases a box
		// through `gui::Router` and answers false; Return releases one through
		// `gui::Type` and answers true, which is how a script tells a submitted
		// field from an abandoned one.
		//
		// @since v0.15
		GuiFocusLost,

		// `CrossWorldService:OpenChannel(name)` - a payload another world in this
		// universe addressed to that channel on this one.
		//
		// **No subject, like `Heartbeat` and unlike everything between them.** A
		// channel message arrives at the world rather than at any instance in it,
		// so the subject is `NULL_ENTITY`.
		//
		// **The channel is the connection's `Property`**, which is what makes one
		// kind serve every channel a world opens. It is
		// `GetPropertyChangedSignal`'s mechanism used for a name the engine never
		// declared, exactly as `GetAttributeChangedSignal` uses it - and it is why
		// there is no signal *field* on the service: a field is one list, and two
		// subsystems in one world listening on two channels need two.
		//
		// **Fired at the deliveries barrier**, which is the first of the four
		// stages `LuauRuntime::Heartbeat` runs - a message the barrier applied
		// belongs to the tick that is starting, so a handler sees it before
		// anything that beat moves.
		//
		// @since v0.15
		CrossWorldMessage,

		// `SettingsService:SetMenuAction(name, label)` - the named row was
		// activated in the client's ESC menu. The action name is the connection's
		// `Property`, so independently-authored rows share one signal kind without
		// hearing each other.
		//
		// No subject, because the menu belongs to the client presentation rather
		// than to an instance in the world.
		//
		// @since v0.21
		SettingsMenuAction,

		// `tween.Completed` - a tween reached the end of its last pass.
		//
		// **The subject is the tween's own entity, which is what a tween is.**
		// `Tweens.hpp` argues that at length; what this list needs from it is
		// that the subject is not the instance being animated - two tweens
		// driving one part are two signals, and a subject shared between them
		// would be one.
		//
		// **Fired at the tween barrier rather than inside the step.** A handler
		// may cancel the tween it was called about or start another, and
		// `TweenTable::Advance` is mid-walk of the list that names it - the same
		// argument `Changes.hpp` makes for not firing from inside a write.
		//
		// **The handler is called with nothing.** Roblox passes a
		// `PlaybackState`, this engine has no such enum, and an argument
		// invented here would have to change the day one arrives - the same
		// trade `GuiActivated` is on a few lines up.
		//
		// @since v0.16
		TweenCompleted,
	};

	// Whether a tree change is a player arriving in or leaving the `Players`
	// service.
	//
	// **The filter behind `PlayerAdded` and `PlayerRemoving`, shared by both
	// languages and by both mechanisms.** `Players` is an ordinary container
	// and may hold something that is not a player, so the test is on both ends:
	// the container is that service, and the thing moving is a `Player`. A
	// handler that had to check the class itself is a check every game writes
	// and one of them forgets.
	//
	// By class rather than by `scene::PlayersOf`, which is a scan of every root
	// in the world - this runs once per tree change.
	//
	// @param store    The world.
	// @param container The parent gaining or losing the instance.
	// @param instance The instance that moved.
	// @return `true` when a player joined or left.
	// @since v0.13
	bool IsPlayerOfService(const ecs::Store &store, ecs::Entity container, ecs::Entity instance);

	// Which player is about to lose the body being removed, or a null entity.
	//
	// **`IsPlayerOfService`'s shape for the other synchronous signal**, and the
	// gate on `container` is what makes it fire once. `OnDescendantRemoving`
	// announces a leaving instance to *every* ancestor above it, so a test that
	// only asked "is this somebody's character" would fire `CharacterRemoving`
	// once per level of the tree above `Workspace`.
	//
	// **Null for an NPC and for a body already released**, both through
	// `scene::PlayerOf`, which reads `Character::Owner` and checks it is alive.
	// The second is what keeps this disjoint from the queued half: a release
	// clears the owner, so a model destroyed after being released is nobody's
	// here and its removal was already reported at the barrier.
	//
	// @param store     The world.
	// @param container The ancestor being told.
	// @param instance  The instance leaving.
	// @return The `Player`, or `ecs::NULL_ENTITY`.
	// @since v0.17
	ecs::Entity PlayerLosingCharacter(const ecs::Store &store, ecs::Entity container, ecs::Entity instance);

	// A callable, as the VM that owns it names one.
	//
	// Opaque here. Luau puts a registry ref in it and JavaScript an index into
	// its own vector of values; nothing in this file may interpret one.
	using CallbackRef = int32_t;

	// What `:Connect` handed back, and what `:Disconnect` names.
	//
	// Monotonic and never reused, so a stale handle disconnects nothing rather
	// than disconnecting whatever took its slot. Reuse would make a
	// double-`:Disconnect` - which is ordinary in real code, because a cleanup
	// path runs whether or not something else already ran - silently kill an
	// unrelated connection.
	using ConnectionId = uint64_t;

	// A connection that never existed.
	inline constexpr ConnectionId NULL_CONNECTION = 0;

	// One live connection.
	//
	// @since v0.6
	struct Connection {
		// The handle `:Connect` returned.
		ConnectionId Id = NULL_CONNECTION;

		// The VM's name for the callable.
		CallbackRef Callback = 0;

		// The property this connection filters on, for `PropertyChanged`.
		// Invalid for every other kind.
		core::Name Property;

		// Whether it is still connected.
		//
		// A flag rather than an erase, because a `:Disconnect` from inside a
		// fire would otherwise move the vector under the loop walking it. The
		// dead entries are compacted once the fire finishes.
		bool Live = true;

		// Whether it retires after one call - `:Once`.
		//
		// A flag here rather than a wrapper closure in each VM. A wrapper would
		// have to disconnect itself from inside its own call, which is the one
		// shape a Luau author reliably gets wrong and the reason `:Once` is
		// worth binding at all.
		bool Once = false;
	};

	// Every connection in one VM, grouped by what it listens to.
	//
	// @since v0.6
	class SignalTable {
	  public:
		// Adds a connection.
		//
		// @param kind     Which signal.
		// @param subject  The instance, or `NULL_ENTITY` for a world signal.
		// @param callback The VM's name for the callable.
		// @param property The property to filter on, for `PropertyChanged`.
		// @return The handle a script disconnects with.
		ConnectionId
		Connect(SignalKind kind, ecs::Entity subject, CallbackRef callback, core::Name property = {});

		// Marks a connection dead and reports what to release.
		//
		// The callable is **not** released here: only the VM knows how, and a
		// disconnect can arrive from inside a fire, where the value may still be
		// on the stack. The caller releases what this hands back.
		//
		// @param id       The handle `:Connect` returned.
		// @param released Filled in with the callable to release.
		// @return `false` when the handle names nothing live.
		bool Disconnect(ConnectionId id, CallbackRef &released);

		// Reports whether a handle names a live connection.
		//
		// @param id The handle to test.
		// @return `true` when it is still connected.
		bool Connected(ConnectionId id) const;

		// Marks a connection as retiring after its first call.
		//
		// Separate from `Connect` rather than a parameter on it, so the common
		// case reads as one argument list and `:Once` is visibly the exception.
		//
		// @param id The handle to mark.
		// @return `false` when the handle names nothing live.
		bool MarkOnce(ConnectionId id);

		// Calls `body` for every live connection on one signal, in order.
		//
		// **Connections made during a fire do not fire this pass**, because the
		// count is snapshotted on entry. A callback that connects another one
		// would otherwise be able to extend its own iteration without bound, and
		// how far it got would depend on where the vector happened to reallocate.
		//
		// Disconnections during a fire take effect immediately: a connection
		// already marked dead is skipped even if the fire has not reached it.
		//
		// @param kind    Which signal.
		// @param subject The instance, or `NULL_ENTITY` for a world signal.
		// @param body    Called with each live callable.
		void Fire(SignalKind kind, ecs::Entity subject, const std::function<void(const Connection &)> &body);

		// Every instance with at least one connection of a kind, in the order
		// their first connection was made.
		//
		// **Insertion order, not hash order**, and that is the whole reason this
		// is not a bare walk of the map. `.Changed` fires per instance, so the
		// order instances are visited in is the order a script sees its world
		// change - and a hash walk would make that depend on pointer values.
		//
		// @param kind Which signal.
		// @param body Called with each subject.
		void EachSubject(SignalKind kind, const std::function<void(ecs::Entity)> &body) const;

		// Drops every connection on one instance and reports what to release.
		//
		// Called when an instance is destroyed: a `.Changed` connection on a row
		// that no longer exists would fire against a dead handle forever.
		//
		// @param subject  The instance being dropped.
		// @param released Appended with every callable to release.
		void DropSubject(ecs::Entity subject, std::vector<CallbackRef> &released);

		// Empties the table and reports every callable to release.
		//
		// @param released Appended with every callable the table held.
		void Clear(std::vector<CallbackRef> &released);

		// How many live connections one signal has.
		//
		// @param kind    Which signal.
		// @param subject The instance, or `NULL_ENTITY` for a world signal.
		// @return The count of live connections.
		size_t Count(SignalKind kind, ecs::Entity subject) const;

	  private:
		// One signal's identity, packed so it can key a map.
		//
		// The kind in the top bits and the entity underneath. An entity id is
		// an index and a generation and never uses the top eight bits, so this
		// is a shift rather than a hash of a pair.
		static uint64_t KeyOf(SignalKind kind, ecs::Entity subject) {
			return (static_cast<uint64_t>(kind) << 56) | (subject.Id & 0x00FFFFFFFFFFFFFFull);
		}

		// Removes the dead entries from one list, once nothing is walking it.
		static void Compact(std::vector<Connection> &connections);

		std::unordered_map<uint64_t, std::vector<Connection>> Lists;

		// Which list a handle is in, so `Disconnect` needs no scan of every
		// signal in the world.
		std::unordered_map<ConnectionId, uint64_t> Owners;

		// Subjects in first-connection order, per kind. What `EachSubject`
		// walks, and the reason a hash map alone is not enough.
		std::unordered_map<uint8_t, std::vector<ecs::Entity>> SubjectOrder;

		// How deep a fire is, so a nested one does not compact a list an outer
		// fire is still walking.
		int Firing = 0;

		ConnectionId NextId = 1;
	};
}
