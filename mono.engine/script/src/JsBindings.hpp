#pragma once

// What a JavaScript script can hold and what it can touch.
//
// The JavaScript twin of `LuauBindings.hpp`, and deliberately the same shape: value
// types, an `Instance` constructor, and property access that switches on
// `PropertyType` and never on a property's name.
//
// **The two files share a rule rather than code.** They marshal into different
// VMs with different object models - Luau has metatables, JavaScript has
// prototypes and accessors - so a common implementation would be an abstraction
// over two runtimes' internals. What is actually shared is the thing that
// matters: one class table, one descriptor list, one set of conversions, and -
// since v0.6 - one `SignalTable`, one `ChangeQueue`, one `TaskQueue` and one
// codec. Everything about *ordering* is shared, because ordering is what a
// recording depends on.
//
// ## Where the two surfaces differ, and why each difference is the language
//
// - **`a.mul(b)` rather than `a * b`**, and `a.Equals(b)` rather than `a == b`.
//   JavaScript has no operator overloading, and `===` on two objects is
//   identity.
// - **`game.GetService` rather than `game:GetService`.** No colon call.
// - **`typeOf(v)` rather than `typeof v`.** `typeof` is a *keyword* in
//   JavaScript and cannot be rebound; in Luau it is an ordinary global that
//   reads a `__type` metafield.
// - **A `Promise` rather than a coroutine.** `task.wait` suspends by returning
//   a promise an `await` consumes, because that is JavaScript's own suspension
//   primitive - and `JS_ExecutePendingJob` means the *host* decides when a
//   reaction runs, which is what makes it legal under rule 5 at all.
//
// None of those is one language pretending to be the other.

#include "Codec.hpp"
#include "JsContext.hpp"
#include "ServiceCatalogue.hpp"
#include "ServiceSurface.hpp"
#include "Tweens.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>

#include <quickjs.h>
#include <span>
#include <string>

namespace engine::script {

	// Installs the property surface: `Instance`, the value types, `Enum`,
	// `game`, `workspace`, `MessagingService` and `print`.
	//
	// @param context The JS context.
	// @param store   The world instances are created in.
	// @param role    Where scripts under this runtime are standing.
	void OpenJsBindings(JSContext *context, ecs::Store &store, const HostRole &role);

	// Installs v0.6's surface: signals, the instance methods, `task`, the
	// datatype vocabulary, the clock, `typeOf`/`warn` and the store services.
	//
	// A second function rather than more of the first, because the two halves
	// are reviewed differently - one is the property surface and the other is
	// everything an author reaches for after it.
	void OpenJsSurface(JSContext *context);

	// Releases what the two `Open` calls attached. Before the context is freed.
	void CloseJsBindings(JSContext *context);

	// Calls every connected Heartbeat function with `delta`.
	std::string PumpJsHeartbeat(JSContext *context, float delta);

	// Dispatches this tick's deliveries, and resolves the promises waiting on
	// replies.
	std::string PumpJsDeliveries(JSContext *context, ecs::Store &store);

	// Fires `.Changed` for everything the last barrier recorded.
	// Puts the shared instance methods on the global, as `__instanceMethods`.
	//
	// **Called before the first prototype is built, and that ordering is the
	// whole reason it is its own function.** `PrototypeFor` chains every class
	// prototype behind this object and *caches the result*; a prototype built
	// before it existed fell back to a plain one and was then kept forever. The
	// `workspace` global is built during `OpenJsBindings`, so it had no
	// `IsA`, no `GetChildren` and no signals at all - silently, because a
	// missing method in JavaScript is `undefined` until something calls it.
	//
	// @param context The VM to install into.
	void InstallJsInstanceMethods(JSContext *context);

	// Adds every neutral instance method to the shared method object.
	//
	// **The JavaScript half of `ScriptCall.hpp`.** A method that is written once
	// is installed by both VMs from one table, which is what makes parity a
	// property of the build rather than of somebody comparing two files - see
	// `InstallLuauNeutralMethods` for the drift that motivated it.
	//
	// Called by `InstallJsInstanceMethods`, on the object it is building.
	//
	// @param context The VM.
	// @param methods The shared method object every class prototype sits behind.
	// @since v0.16
	void InstallJsNeutralMethods(JSContext *context, JSValueConst methods);

	// Adds one service's neutral methods to the object standing for it.
	//
	// **The JavaScript half of what made five services reachable here.** A
	// `ServiceSurface` is data, so `ServiceCatalogue.cpp` reads one description
	// twice - `InstallService` builds the Luau table and this fills in the
	// JavaScript object - and a method added to that table is a member in both
	// languages in the same commit.
	//
	// @param context The VM.
	// @param service The object the service is being built on.
	// @param methods The service's rows.
	// @since v0.16
	void
	InstallJsServiceMethods(JSContext *context, JSValueConst service, std::span<const ServiceMethod> methods);

	// Adds one service's neutral properties to the object standing for it.
	//
	// **The JavaScript half of what made the last two reachable here**, and it
	// is native accessors rather than anything resembling the Luau side.
	// `JS_DefinePropertyGetSet` runs the getter on every read, so the service
	// stays a plain object - no userdata, no tag, no registry key. What it needs
	// in exchange is the property *names* at install time, which is why
	// `ServiceProperty` is a list and not a catch-all.
	//
	// @param context    The VM.
	// @param service    The object the service is being built on.
	// @param name       The service's name, for a refusal to quote.
	// @param properties The service's rows.
	// @since v0.16
	void InstallJsServiceProperties(
		JSContext *context,
		JSValueConst service,
		const char *name,
		std::span<const ServiceProperty> properties
	);

	// One whole service, as a JavaScript object on the global.
	//
	// **`InstallService`'s twin, and `JsServiceSurface.cpp` is where it lives.**
	// Signals, then methods, then properties - the order the Luau installer fixes
	// for the same reason, so a name claimed twice resolves the same way in both
	// languages. `ServiceCatalogue.cpp` is the only caller.
	//
	// @param context The VM.
	// @param global  The global object the service is named on.
	// @param surface The description both languages read.
	// @since v0.18
	void InstallJsService(JSContext *context, JSValueConst global, const ServiceSurface &surface);

	// One member of one enum, as an `EnumItem`.
	//
	// **Exported because the input pump needs one and is not this file's.** A
	// bound action's handler is called with `Enum.UserInputState.Begin` as its
	// second argument, which is Roblox's signature; the alternative was a second
	// way to build an `EnumItem` in a second translation unit.
	//
	// @since v0.16
	JSValue MakeJsEnumItem(JSContext *context, core::Name enumName, core::Name member);

	// Registers the `InputObject` class a bound action's handler is handed one
	// of. Before the first service is installed.
	//
	// @since v0.16
	void InstallJsInputObject(JSContext *context);

	// One input report as an `InputObject`.
	//
	// The JavaScript twin of `PushInputObject`. Read-only and sealed, for the
	// Luau half's reason: an input report is a fact about a frame rather than a
	// document a handler edits and passes on.
	//
	// @since v0.16
	JSValue MakeJsInputObject(JSContext *context, const InputReport &report);

	// Turns this frame's input edges into bound actions and input signals.
	//
	// **The twin of `PumpInput`, and it exists because binding an action or a
	// signal that never fires is worse than not binding one at all.**
	// `ContextActionService` crossed at v0.16 with no input pump in this
	// language, so a JavaScript `BindAction` would have taken a handler and
	// forgotten it; `UserInputService` crossed with the property mechanism, and
	// its six signals would have been connectable and silent - which is the
	// state this module names twice as reading like a broken engine.
	//
	// **Six things, in `PumpInput`'s order**: the focus edges, the device change,
	// key edges, mouse button edges, pointer motion and the wheel - so a listener
	// sees `WindowFocusReleased` before the releases losing focus caused, in
	// either language.
	//
	// Called at the same place in the barrier the Luau pump is - before the
	// changes, so a handler's writes reach their listeners on this beat rather
	// than the next.
	//
	// @param context   The VM.
	// @param interface What the router produced for this beat, which is what
	//        `gameProcessedEvent` is decided from. See `InterfaceHasPointer`.
	// @return An error message when a handler threw, or empty.
	// @since v0.16
	std::string PumpJsInput(JSContext *context, std::span<const gui::GuiEvent> interface);

	std::string PumpJsChanges(JSContext *context);

	// Delivers everything the tree recorded since the last barrier.
	//
	// The JavaScript half of `PumpTree`. `DescendantRemoving` is not here and
	// never will be: it is dispatched from inside the store before the removal
	// happens, because that is the only place its contract can hold.
	//
	// @param context The VM to deliver into.
	// @return The first error a handler raised, or empty.
	std::string PumpJsTree(JSContext *context);

	// The JavaScript half of `PumpChildWaiters`: resolves every `WaitForChild`
	// whose child has arrived or whose wait has run out.
	//
	// **After `PumpJsTree`, exactly as the Luau side orders it** - and for its
	// reason: a `ChildAdded` handler and a resumed `WaitForChild` are two scripts
	// told about one arrival, and the signal every listener shares goes first.
	//
	// What a waiting script actually continues on is the microtask queue, so the
	// continuation runs in `DrainJobs` at the end of the beat rather than inside
	// this call. That is the same one-step-later shape every `await` in this
	// engine has, and it is the language's rather than the engine's.
	//
	// @param context The VM to resolve in.
	// @return The first error a resumed script raised, or empty.
	// @since v0.15
	std::string PumpJsChildWaiters(JSContext *context);

	// The JavaScript half of `PumpCharacters`.
	//
	// @param context The VM to deliver into.
	// @return The first error a handler raised, or empty.
	// @since v0.17
	std::string PumpJsCharacters(JSContext *context);

	// The JavaScript half of `PumpGuiEvents`, with the same argument rule:
	// `MouseEnter`, `MouseLeave` and `MouseMoved` get `(x, y)`, and the three
	// input signals get nothing until there is an `InputObject` to hand them.
	//
	// @param context The VM to deliver into.
	// @param events  What the host collected since the last beat.
	// @return The first error a handler raised, or empty.
	std::string PumpJsGuiEvents(JSContext *context, std::span<const gui::GuiEvent> events);

	// Resolves every task due at the world's current tick.
	std::string PumpJsTasks(JSContext *context);

	// Advances every tween by one tick and fires what completed.
	//
	// **The twin of `PumpTweens`, at the same place in the barrier**, which is
	// the head of it - see `LuauRuntime::Heartbeat` for the whole order and why
	// the world's own timed work goes before the pumps that deliver to scripts.
	// `delta` is the fixed tick delta and never wall time.
	//
	// **There is no `PumpJsDebris` beside it, and that is the difference between
	// the two queues.** Draining debris fires nothing, so `PumpDebris` takes a
	// store and a queue and both runtimes call the one function; a tween's
	// `Completed` is a signal, and calling a callable is the half no shared
	// function can do.
	std::string PumpJsTweens(JSContext *context, float delta);

	// Calls everything connected to one signal.
	//
	// `property` is `FireSignal`'s filter, for its reason: one `SignalKind` serving
	// a set of names told apart at fire time. Invalid fires every connection.
	//
	// @return An error message when a handler threw, or empty.
	std::string FireJsSignal(
		JSContext *context,
		SignalKind kind,
		ecs::Entity subject,
		int count,
		JSValueConst *arguments,
		core::Name property = {}
	);

	// A signal handle, and a connection handle.
	JSValue MakeJsSignal(JSContext *context, SignalKind kind, ecs::Entity subject, core::Name property = {});
	JSValue MakeJsConnection(JSContext *context, ConnectionId id);

	// Wrapping and unwrapping the value types.
	JSValue MakeVector3(JSContext *context, const core::Vector3 &value);
	JSValue MakeColor3(JSContext *context, const core::Color3 &value);
	JSValue MakeCFrame(JSContext *context, const core::CFrame &value);

	core::Vector3 *AsVector3(JSContext *context, JSValueConst value);
	core::Color3 *AsColor3(JSContext *context, JSValueConst value);
	core::CFrame *AsCFrame(JSContext *context, JSValueConst value);

	// The four `gui` is authored in, added at v0.8 with the property types.
	JSValue MakeVector2(JSContext *context, const core::Vector2 &value);
	JSValue MakeUDim(JSContext *context, const core::UDim &value);
	JSValue MakeUDim2(JSContext *context, const core::UDim2 &value);
	JSValue MakeRect(JSContext *context, const core::Rect &value);

	core::Vector2 *AsVector2(JSContext *context, JSValueConst value);
	core::UDim *AsUDim(JSContext *context, JSValueConst value);
	core::UDim2 *AsUDim2(JSContext *context, JSValueConst value);
	core::Rect *AsRect(JSContext *context, JSValueConst value);

	// The curve a tween is authored with, or null. `JsCall::AsTweenInfo` is what
	// asked for it; `TweenService` used to reach the class id directly.
	core::TweenInfo *AsTweenInfo(JSContext *context, JSValueConst value);

	// The three `effects` is authored in, added at v0.10 with the property types.
	JSValue MakeNumberRange(JSContext *context, const core::NumberRange &value);
	JSValue MakeNumberSequence(JSContext *context, const core::NumberSequence &value);
	JSValue MakeColorSequence(JSContext *context, const core::ColorSequence &value);

	core::NumberRange *AsNumberRange(JSContext *context, JSValueConst value);
	core::NumberSequence *AsNumberSequence(JSContext *context, JSValueConst value);
	core::ColorSequence *AsColorSequence(JSContext *context, JSValueConst value);

	// Marshalling one value of one `PropertyType`, both ways.
	//
	// **The type and the enum name, never a descriptor**, because the second
	// caller has none: an ECS component field carries exactly these values and is
	// not a property. One switch per language rather than two per language that
	// agree until somebody edits one.
	//
	// `PropertyType::String` is refused by both, and the refusal is the design -
	// `bytes` is uninitialised storage and a `std::string` cannot be assigned into
	// it, so every caller takes strings down a path of its own.
	//
	// @since v0.12
	JSValue ToJsValue(JSContext *context, ecs::PropertyType type, core::Name enumName, const void *bytes);
	bool FromJsValue(
		JSContext *context, JSValueConst value, ecs::PropertyType type, core::Name enumName, void *out
	);

	// Installs `World`, and the component methods every instance object gains.
	//
	// The JavaScript twin of `OpenEcs`. Called after `InstallJsInstanceMethods`,
	// whose `__instanceMethods` object this adds to.
	//
	// @since v0.12
	void InstallJsEcs(JSContext *context, JSValueConst global);

	// One instance object for an entity, prototype and all.
	JSValue MakeJsInstance(JSContext *context, ecs::Entity instance);

	// The entity an instance object stands for, or `NULL_ENTITY`.
	ecs::Entity JsEntityOf(JSContext *context, JSValueConst object);

	// The datatype vocabulary, installed on the global.
	//
	// Its own file for the reason `OpenJsSurface` is separate from
	// `OpenJsBindings`: these are value types, reviewed against `core/types/`
	// rather than against the class table.
	void InstallJsDatatypes(JSContext *context, JSValueConst global);

	// Every service this language binds, from the catalogue.
	//
	// **The list is `ServiceCatalogue.cpp`'s and not this file's**, which is what
	// makes "which services exist" one fact rather than one per VM. A service
	// this language has no installer for installs nothing, and `GetService`
	// refuses it by name saying which language does bind it - see
	// `ServiceCatalogue.hpp`.
	//
	// @param context The VM.
	// @param global  The global object to hang each service on.
	// @param phase   Which set to install. This language has no studio services
	//        today, so the second phase is a walk over an empty selection - kept
	//        because the *catalogue* has one and a second signature would be a
	//        second place that fact lives.
	// @since v0.15
	void InstallJsServices(JSContext *context, JSValueConst global, ServiceAvailability phase);

	// --- what is left of this language's own service code ---------------------
	//
	// **Nothing, and the list that used to be here is the point.** There were
	// seven `OpenJs*Service` functions at v0.15 and none now: every service is a
	// `ServiceSurface` that `ServiceCatalogue.cpp` builds twice, so a service
	// added to this engine cannot be a service this language does not have. The
	// linker argument that made them named in the first place still applies to
	// the surfaces, and `ServiceCatalogue.cpp` is still the file that names every
	// one.
	//
	// What remains per language is *apparatus* rather than service: the `Tween`
	// class below, which is a value type a method hands back.

	// Installs the `Tween` class and the three methods on a tween handle.
	//
	// **The handle and not the service.** `TweenServiceSurface` describes
	// `GetValue` and `Create` for both languages; `Play`, `Pause` and `Cancel`
	// belong to the object `Create` answers with, and that object is a registered
	// class here and a tagged userdata there. See `Tweens.hpp`.
	//
	// @since v0.16
	void OpenJsTweenHandle(JSContext *context);

	// One `Tween` object for a tween's entity.
	//
	// @param context The VM.
	// @param tween   The tween's entity, from `TweenTable::Create`.
	// @since v0.16
	JSValue MakeJsTween(JSContext *context, ecs::Entity tween);

	// `RaycastParams` and `workspace.Raycast`.
	//
	// Takes the world object because that is where the method goes, and it has
	// to be installed before `OpenJsBindings` seals it.
	void InstallJsQueries(JSContext *context, JSValueConst global, JSValueConst workspace);

	// An `EnumItem`, and reading one back - the JavaScript spellings.
	bool ReadJsEnumValue(JSContext *context, JSValueConst value, core::Name enumName, core::Name &out);

	// A JavaScript value to the shared tree, and back.
	//
	// **The same tree the Luau side builds**, which is what makes "identical
	// bytes from both VMs" a property of the format rather than of either
	// binding.
	bool
	ToScriptValue(JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why);
	JSValue FromScriptValue(JSContext *context, const ScriptValue &value);
}
