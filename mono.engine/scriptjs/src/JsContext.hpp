#pragma once

// What one JavaScript runtime holds, shared by the two files that build it.
//
// **Its own header rather than a struct inside `JsBindings.cpp`**, because
// v0.6's surface is a second file: the property surface and everything an
// author reaches for *after* it are reviewed differently, and one four-thousand
// line translation unit would have made that impossible. Both files need the
// same state, so the state moved out and neither owns it.
//
// This is the JavaScript twin of `LuauBindings.hpp`'s `LuauContext`, and the
// shared members are deliberately the same types. `SignalTable`, `ChangeQueue`
// and `TaskQueue` decide **ordering**, and ordering is what a recording depends
// on - so the two languages cannot disagree about it, because there is one
// implementation and each binding only supplies the callables.
//
// @tier L9 · shared

#include <engine/ecs/Store.hpp>
#include <engine/script/Actions.hpp>
#include <engine/script/Bus.hpp>
#include <engine/script/Changes.hpp>
#include <engine/script/ChildWaiters.hpp>
#include <engine/script/ComputeJobs.hpp>
#include <engine/script/Debris.hpp>
#include <engine/script/EditableMeshJobs.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/Signals.hpp>
#include <engine/script/Tasks.hpp>
#include <engine/script/Tweens.hpp>

#include <cstdint>
#include <quickjs.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// One neutral service property installed in this VM, and the service it is
	// on.
	//
	// **The service's name rides beside the row because a refusal needs it.**
	// `SoundService.AmbientReverb = 1` has to say which service is refusing, and
	// a `ServiceProperty` carries only its own name - the same sentence the Luau
	// half builds from `ServiceSurface::Name`.
	//
	// @since v0.16
	struct JsServiceProperty {
		const ServiceProperty *Row;
		const char *Service;
	};

	// Everything one JS runtime needs to reach the world, hung off the context
	// rather than a static - two runtimes over two worlds must not be able to
	// reach each other's storage.
	//
	// @since v0.5
	struct JsContext {
		ecs::Store *World = nullptr;

		// What the host is, for `RunService.IsServer()` and friends.
		HostRole Role;

		// The services this runtime may reach.
		ScriptCapabilities Access = ScriptCapabilities::None;

		// The program surface installed for plugin runtimes.
		HostSurface *Host = nullptr;

		// Stable host callback ids over the callable table below. The callable
		// table owns the QuickJS values and recycles its slots; these ids never
		// expose those implementation details to the host.
		uint64_t NextHostCallback = 0;
		std::unordered_map<uint64_t, CallbackRef> HostCallbacks;

		// What the last host installation added to the global. Kept so replacing
		// or removing a host does not leave stale service objects behind.
		std::string HostGlobal;
		std::vector<std::string> HostServices;

		// The shared machinery.
		SignalTable Signals;
		ChangeQueue Changes;
		TaskQueue Tasks;

		// What `ContextActionService` has bound, highest priority first.
		//
		// **Shared with the Luau side and for the three above's reason.** Which
		// handler a press reaches is an ordering rule, and this language could
		// not bind the service at all until the rule had one implementation -
		// see `Actions.hpp`.
		ActionStack Actions;

		// How many GUIDs `HttpService.GenerateGUID` has handed out.
		//
		// **A counter rather than a clock or an entropy source, which is what
		// makes a GUID replayable** - `HttpService.cpp` carries the argument.
		// On the context rather than a file-static for this struct's own reason:
		// two runtimes over two worlds must not share one stream.
		uint64_t NextGuid = 0;

		// Every neutral service method installed in this VM, flattened.
		//
		// **What a magic number indexes**, because `JS_NewCFunctionMagic` takes
		// one integer where `lua_pushcclosure` takes the row's address on an
		// upvalue. Appended to as each service is installed and never reordered:
		// a function already built holds its index.
		std::vector<ScriptMethod> ServiceMethods;

		// Every neutral service property installed in this VM, flattened.
		//
		// **A second list beside `ServiceMethods` rather than one**, because a
		// getter and a setter are reached through a different trampoline than a
		// call and merging the two would make a magic number mean two things.
		// Appended to as each service is installed and never reordered.
		std::vector<JsServiceProperty> ServiceProperties;

		// The two queues that step on the fixed tick delta, which are shared for
		// the reason the three above are: what a tween or a deadline *is* names
		// no VM, and the order two of them are drained in is a thing a recording
		// depends on. See `Tweens.hpp` and `Debris.hpp`.
		//@{
		TweenTable Tweens;
		DebrisQueue Debris;
		//@}

		// Every `WaitForChild` this VM has outstanding.
		//
		// **The same type the Luau context holds and for the three above's
		// reason**: a wait's deadline is a tick number and the order two of them
		// are answered in is a thing a recording depends on, so there is one
		// implementation and this binding supplies only the resume. See
		// `ChildWaiters.hpp`.
		ChildWaiters Waiters;

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
		// `core::Ray`, exactly as on the Luau side - the engine had both, and a
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

		// What a bound action's handler is handed as its third argument.
		//
		// **A class rather than a plain object**, which is what makes
		// `typeOf(input)` answer `"InputObject"` and what stops a handler being
		// passed something that merely looks like one. Roblox offers no
		// constructor and neither does this: an input report is produced by the
		// pump or not at all.
		JSClassID InputObjectClass = 0;

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

		// Who is listening to which bus topic.
		//
		// **The same type the Luau context holds since v0.16**, which is what let
		// `MessagingService` be described once: this was an `unordered_map` here
		// and a registry table there, and neither half did anything a language
		// decides. See `Bus.hpp`.
		TopicSubscriptions Subscriptions;

		// Which promise resolver is waiting on which `world::Ticket`.
		//
		// **§1's first legal resume source, made concrete.** A `GetAsync`
		// returns a ticket, the reply lands at a later barrier applied in
		// sorted order, and this joins the reply to the promise waiting on it.
		std::unordered_map<uint64_t, CallbackRef> AwaitedTickets;

		// Which promise resolver is waiting on which `ChildWaiters` entry.
		//
		// **The second resume source, and a second map rather than a widened
		// first one** - the Luau twin carries the argument: a ticket and a waiter
		// id come from two counters that know nothing about each other, and what
		// each resolves *with* is different.
		std::unordered_map<uint64_t, CallbackRef> AwaitedChildren;

		EditableMeshJobs EditableMeshes;
		std::unordered_map<uint64_t, CallbackRef> AwaitedEditableMeshes;

		ComputeJobs Computations;
		std::unordered_map<uint64_t, CallbackRef> AwaitedComputations;

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
