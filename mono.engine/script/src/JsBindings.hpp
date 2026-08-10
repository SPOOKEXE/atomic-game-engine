#pragma once

// What a JavaScript script can hold and what it can touch.
//
// The JavaScript twin of `Bindings.hpp`, and deliberately the same shape: value
// types, an `Instance` constructor, and property access that switches on
// `PropertyType` and never on a property's name.
//
// **The two files share a rule rather than code.** They marshal into different
// VMs with different object models — Luau has metatables, JavaScript has
// prototypes and accessors — so a common implementation would be an abstraction
// over two runtimes' internals. What is actually shared is the thing that
// matters: one class table, one descriptor list, one set of conversions, and —
// since v0.6 — one `SignalTable`, one `ChangeQueue`, one `TaskQueue` and one
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
//   primitive — and `JS_ExecutePendingJob` means the *host* decides when a
//   reaction runs, which is what makes it legal under rule 5 at all.
//
// None of those is one language pretending to be the other.

#include "Codec.hpp"
#include "JsContext.hpp"

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
	// are reviewed differently — one is the property surface and the other is
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
	// `IsA`, no `GetChildren` and no signals at all — silently, because a
	// missing method in JavaScript is `undefined` until something calls it.
	//
	// @param context The VM to install into.
	void InstallJsInstanceMethods(JSContext *context);

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

	// Calls everything connected to one signal.
	//
	// @return An error message when a handler threw, or empty.
	std::string FireJsSignal(
		JSContext *context, SignalKind kind, ecs::Entity subject, int count, JSValueConst *arguments
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
	// `PropertyType::String` is refused by both, and the refusal is the design —
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

	// The datatype vocabulary and the bus services, installed on the global.
	//
	// Their own file for the reason `OpenJsSurface` is separate from
	// `OpenJsBindings`: these are value types and a wire, reviewed against
	// `core/types/` and `world::Postbox` rather than against the class table.
	void InstallJsDatatypes(JSContext *context, JSValueConst global);
	void InstallJsServices(JSContext *context, JSValueConst global);

	// `RaycastParams` and `workspace.Raycast`.
	//
	// Takes the world object because that is where the method goes, and it has
	// to be installed before `OpenJsBindings` seals it.
	void InstallJsQueries(JSContext *context, JSValueConst global, JSValueConst workspace);

	// An `EnumItem`, and reading one back — the JavaScript spellings.
	bool ReadJsEnumValue(JSContext *context, JSValueConst value, core::Name enumName, core::Name &out);

	// The easing enums, converted between their C++ form and their member name.
	//
	// Declared here as well as in `Bindings.hpp` because both bindings need
	// them and neither header may include the other's VM.
	core::EasingStyle EasingStyleOf(core::Name member);
	core::Name NameOf(core::EasingStyle style);
	core::EasingDirection EasingDirectionOf(core::Name member);
	core::Name NameOf(core::EasingDirection direction);

	// A JavaScript value to the shared tree, and back.
	//
	// **The same tree the Luau side builds**, which is what makes "identical
	// bytes from both VMs" a property of the format rather than of either
	// binding.
	bool
	ToScriptValue(JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why);
	JSValue FromScriptValue(JSContext *context, const ScriptValue &value);
}
