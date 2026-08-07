#pragma once

// What `:Connect` returns, and the ordering rules both VMs obey.
//
// **The list is shared and the callables are not.** A Luau connection holds a
// registry ref and a JavaScript one holds a `JSValue`; neither type can appear
// here, because `script/AGENTS.md` keeps every VM type inside its own source
// file. What *can* be shared — and has to be — is everything about ordering:
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
#include <unordered_map>
#include <vector>

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
		// at the next barrier — `Changes.hpp` sets out why a signal must not
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

		// --- the 2D tree's input, from `gui::Router` ------------------------
		//
		// **The six below are one mechanism and it is not the tree's.** Every
		// signal above is recorded by the store and fanned out at the barrier;
		// these arrive from outside the world entirely — a host polls a pointer,
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

		// `guiObject.Activated` — pressed and released on the same element.
		// The one nearly every script connects to.
		GuiActivated,

		// `guiObject.InputBegan` — the button went down over it.
		GuiInputBegan,

		// `guiObject.InputEnded` — the button came up. Fired on the element the
		// press *began* on, which is `gui::Router`'s rule and is what makes a
		// drag off a button and back one interaction rather than two.
		GuiInputEnded,

		// `guiObject.MouseEnter` — the pointer entered its rectangle.
		GuiMouseEnter,

		// `guiObject.MouseLeave` — it left. Fired before the matching
		// `MouseEnter` on whatever it moved onto, so a handler that puts
		// something back on leave runs before the one reacting to the arrival.
		GuiMouseLeave,

		// `guiObject.MouseMoved` — it moved while over the element.
		GuiMouseMoved,
	};

	// A callable, as the VM that owns it names one.
	//
	// Opaque here. Luau puts a registry ref in it and JavaScript an index into
	// its own vector of values; nothing in this file may interpret one.
	using CallbackRef = int32_t;

	// What `:Connect` handed back, and what `:Disconnect` names.
	//
	// Monotonic and never reused, so a stale handle disconnects nothing rather
	// than disconnecting whatever took its slot. Reuse would make a
	// double-`:Disconnect` — which is ordinary in real code, because a cleanup
	// path runs whether or not something else already ran — silently kill an
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

		// Whether it retires after one call — `:Once`.
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
		// change — and a hash walk would make that depend on pointer values.
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
