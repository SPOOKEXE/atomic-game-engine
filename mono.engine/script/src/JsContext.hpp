#pragma once

// What one JavaScript runtime holds, shared by the two files that build it.
//
// **Its own header rather than a struct inside `JsBindings.cpp`**, because
// v0.6's surface is a second file: the property surface and everything an
// author reaches for *after* it are reviewed differently, and one four-thousand
// line translation unit would have made that impossible. Both files need the
// same state, so the state moved out and neither owns it.
//
// This is the JavaScript twin of `Bindings.hpp`'s `LuauContext`, and the
// shared members are deliberately the same types. `SignalTable`, `ChangeQueue`
// and `TaskQueue` decide **ordering**, and ordering is what a recording depends
// on — so the two languages cannot disagree about it, because there is one
// implementation and each binding only supplies the callables.
//
// @tier L9 · shared

#include "Changes.hpp"
#include "Debris.hpp"
#include "Signals.hpp"
#include "Tasks.hpp"
#include "Tweens.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/script/Runtime.hpp>

#include <quickjs.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// Everything one JS runtime needs to reach the world, hung off the context
	// rather than a static — two runtimes over two worlds must not be able to
	// reach each other's storage.
	//
	// @since v0.5
	struct JsContext {
		ecs::Store *World = nullptr;

		// What the host is, for `RunService.IsServer()` and friends.
		HostRole Role;

		// The shared machinery.
		SignalTable Signals;
		ChangeQueue Changes;
		TaskQueue Tasks;

		// The two queues that step on the fixed tick delta, which are shared for
		// the reason the three above are: what a tween or a deadline *is* names
		// no VM, and the order two of them are drained in is a thing a recording
		// depends on. See `Tweens.hpp` and `Debris.hpp`.
		//@{
		TweenTable Tweens;
		DebrisQueue Debris;
		//@}

		// The context itself, so a callback holding only this can reach the VM.
		JSContext *Js = nullptr;

		// Whether `Store::OnDescendantRemoving` already holds this VM's hook.
		//
		// The store keeps one listener, so this is about not reinstalling an
		// identical one per connection rather than about correctness.
		bool RemovingHooked = false;

		JSClassID InstanceClass = 0;
		JSClassID EnumItemClass = 0;
		JSClassID Vector3Class = 0;
		JSClassID Color3Class = 0;
		JSClassID CFrameClass = 0;

		// v0.6's datatype vocabulary. `Region3` is a `core::AABB` and `Ray` a
		// `core::Ray`, exactly as on the Luau side — the engine had both, and a
		// second spelling of either would be a duplicate.
		JSClassID Vector2Class = 0;
		JSClassID UDimClass = 0;
		JSClassID UDim2Class = 0;
		JSClassID RectClass = 0;
		JSClassID Region3Class = 0;
		JSClassID NumberRangeClass = 0;
		JSClassID NumberSequenceClass = 0;
		JSClassID ColorSequenceClass = 0;
		JSClassID TweenInfoClass = 0;
		JSClassID RayClass = 0;
		JSClassID RandomClass = 0;
		JSClassID SignalClass = 0;
		JSClassID ConnectionClass = 0;
		JSClassID RaycastParamsClass = 0;

		// What `TweenService.Create` hands back. A class of its own rather than
		// an instance object, for the reason `TAG_TWEEN` gives on the Luau side:
		// the shared instance methods are installed on every instance, and
		// `Play` is not a name a tween may take from every part in the world.
		JSClassID TweenClass = 0;

		// **What a `CallbackRef` means on this side.** The Luau binding puts a
		// registry ref in that integer; this one puts an index into this
		// vector. Nothing shared may interpret either.
		//
		// Slots are recycled through `FreeRefs` rather than the vector growing
		// without bound: a game that connects and disconnects every frame would
		// otherwise leak an index per frame for the life of the world.
		std::vector<JSValue> Callables;
		std::vector<CallbackRef> FreeRefs;

		// One prototype per ECS class, built the first time an instance of it
		// is made. **Accessors live on the prototype, not the object**: a scene
		// of five hundred parts would otherwise define five thousand
		// properties, and every one of them would be the same closure over the
		// same name.
		std::unordered_map<uint32_t, JSValue> Prototypes;

		// Kept so the prototypes can be freed with the context.
		std::vector<JSValue> Owned;

		// The world object, kept so `Parent` hands back the same value a script
		// assigned rather than a second object that behaves alike.
		JSValue Workspace = JS_UNDEFINED;

		// Topic to callbacks, one list per topic.
		std::unordered_map<std::string, std::vector<CallbackRef>> Subscriptions;

		// Which promise resolver is waiting on which `world::Ticket`.
		//
		// **§1's first legal resume source, made concrete.** A `GetAsync`
		// returns a ticket, the reply lands at a later barrier applied in
		// sorted order, and this joins the reply to the promise waiting on it.
		std::unordered_map<uint64_t, CallbackRef> AwaitedTickets;

		// How many ticks a `task.wait` asked for, so its resolution can report
		// how long it actually waited. Keyed by the resolver's ref.
		std::unordered_map<CallbackRef, uint64_t> WaitTicks;
	};

	// The context bound to a JS context.
	JsContext &JsOf(JSContext *context);

	// Retains a value and hands back the integer the shared machinery uses.
	//
	// @param context The JS context.
	// @param value   The value to keep. Duplicated; the caller keeps its own.
	// @return The reference.
	CallbackRef Retain(JSContext *context, JSValueConst value);

	// Releases a reference and frees what it held.
	void Release(JSContext *context, CallbackRef reference);

	// The value a reference names, without transferring ownership.
	JSValueConst Held(JSContext *context, CallbackRef reference);
}
