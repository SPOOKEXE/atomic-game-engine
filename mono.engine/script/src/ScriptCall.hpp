#pragma once

// One instance method, written once, called from either language.
//
// **A property was already neutral and a method was not.**
// `ecs::PropertyDescriptor` is data — a `PropertyType`, a size, a getter, a
// setter — so a property declared in `scene` is readable from Luau, from
// JavaScript and in the properties panel with none of the three changing,
// because every binding switches on the type and never on the name.
//
// A method had no such shape. It was a `lua_CFunction` in `Instances.cpp` and a
// `JSCFunction` in `JsSurface.cpp`, written twice, and the two drifted exactly as
// two lists do: Luau's instance method table held thirty entries and
// JavaScript's held twenty-one, and nothing in the build named the nine that
// were missing. `JsSurface.cpp` already carried a scar from the same class of
// bug — a hard-coded count of `10` on a list of sixteen, so six methods
// including `IsDescendantOf` and `Changed` were simply never installed, and a
// method that is not there is `undefined` until something calls it.
//
// So a method becomes data too: a name and a function taking a `ScriptCall`.
// The VM-shaped half — reading an argument, pushing a result, raising an error —
// is an adapter implemented once per language, and `LuauCall.cpp` and
// `JsCall.cpp` are the only files that have met a VM.
//
// ## Why `ScriptValue` cannot be the currency
//
// `script::ScriptValue` is already the shared value type and both languages
// already convert to and from it, so it looks like the answer. It is not, and
// the reason is a rule rather than an omission: `ValueTag` has no instance, and
// `CodecStatus::Unsupported` is what a script gets for offering one. A
// `ScriptValue` crosses a **world** — a bus envelope, a data store, a save file —
// and rule 3 says nothing crossing a world boundary is a pointer, so a handle
// naming a row in one store must not arrive in another.
//
// A method call crosses nothing. It happens inside one process, against one
// store, and returns before the tick moves — which is why `ecs::Entity` is
// exactly the right currency here and exactly the wrong one there. Two
// interfaces, because they answer two questions.
//
// ## A service method is the same thing with no subject
//
// `ServiceSurface` described a service in `lua_CFunction`s, so it could only
// build a Luau one and every JavaScript service was hand-written — which is how
// `ContentService`, `CollectionService`, `HttpService`, `CrossWorldService` and
// `ContextActionService` came to be reachable from one language and not the
// other, with the catalogue naming the gap and nothing closing it. A service
// method takes the service table as its receiver and does nothing with it, so
// it is an instance method whose `Subject()` is `NULL_ENTITY`: one adapter, one
// trampoline, one table. See `ServiceMethod` at the foot of this file.
//
// ## The interface carries what its callers ask for and nothing else
//
// There is no `AsBoolean`, no `OptionalNumber` and no `ReturnEnum`, because
// nothing written against this takes or returns one. A pure virtual with no
// caller is a line every adapter has to implement to satisfy the compiler and
// nobody has to get right, which is the shape a mistake hides in — and adding
// one when the first method needs it is a three-line change the build refuses
// to let anybody forget.
//
// ## `ScriptValue` is a *payload* here, and that is not a contradiction
//
// The section above says `ScriptValue` cannot be this interface's currency and
// `ReadValue`/`ReturnValue` are on it anyway. Both are true, and the difference
// is what the value *is*. An argument that names a thing in this world is an
// `ecs::Entity`, because a handle is meaningful inside one process. An argument
// that is an arbitrary tree a script built — the message `CrossWorldService`
// puts on a bus, the document `HttpService` writes as JSON — is a `ScriptValue`,
// because those are exactly the values that leave the world and rule 3 already
// decided their shape. `ValueTag` still has no instance, so the two doors stay
// separate: no method may take a tree where it means a handle.
//
// @tier L9 · shared
// @since v0.16

#include "Actions.hpp"
#include "Changes.hpp"
#include "Codec.hpp"
#include "Signals.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::script {

	// One method call, in whichever language made it.
	//
	// **`self` is not an argument.** Luau passes the receiver as stack slot one
	// and JavaScript passes it as `this`, so an index-based reader that counted
	// it would be off by one in exactly one language. `Subject()` is the
	// instance the method was called on and `index` zero is the first thing the
	// author actually wrote.
	//
	// **Every reader raises rather than returning a failure.** A wrong argument
	// type is a script bug and each language already has a way to say so — a Luau
	// error and a thrown `TypeError` — so a method body reads its arguments
	// straight through and never checks. See `Raise`.
	class ScriptCall {
	  public:
		ScriptCall() = default;
		virtual ~ScriptCall() = default;

		ScriptCall(const ScriptCall &) = delete;
		ScriptCall &operator=(const ScriptCall &) = delete;
		ScriptCall(ScriptCall &&) = delete;
		ScriptCall &operator=(ScriptCall &&) = delete;

		// The world this VM builds into.
		virtual ecs::Store &World() = 0;

		// Where a change is written down for the next barrier to deliver.
		//
		// **An attribute has no component behind it**, so nothing fans out to a
		// property name on its behalf and the writer says what changed itself —
		// see `ChangeQueue::Record`. Both contexts hold one of these and neither
		// one is a VM type.
		virtual ChangeQueue &Changes() = 0;

		// What `ContextActionService` has claimed in this VM.
		//
		// **Beside `Changes` because it is the same kind of thing**: shared
		// state whose ordering rules a recording depends on, with the callables
		// left opaque. See `Actions.hpp`.
		virtual ActionStack &Actions() = 0;

		// The next draw number for `HttpService:GenerateGUID`.
		//
		// **A counter and never a clock or an entropy source, which is what
		// makes a GUID replayable** — `HttpService.cpp` carries the whole
		// argument. It is on the *context* rather than a file-static for this
		// module's own reason: two runtimes over two worlds must not share one
		// stream. Advances by one per call.
		virtual uint64_t NextGuid() = 0;

		// The instance the method was called on.
		//
		// **`NULL_ENTITY` for a service method**, whose receiver is the service
		// table and carries nothing. A service method never asks; the
		// trampolines are what make the distinction, by checking the receiver
		// for an instance method and not for a service one.
		virtual ecs::Entity Subject() const = 0;

		// Whether an argument is absent or nil.
		//
		// **One question rather than two**, because that is what a caller asks:
		// `SetAttribute(name)` and `SetAttribute(name, nil)` both mean remove,
		// which is `lua_isnoneornil` on one side and `argc <= index ||
		// null || undefined` on the other.
		//
		// @param index Zero-based, `self` excluded.
		virtual bool IsNil(size_t index) const = 0;

		// An argument as a string. Raises when it is not one.
		virtual std::string AsString(size_t index) = 0;

		// An argument as a `CFrame`. Raises when it is not one.
		virtual core::CFrame AsCFrame(size_t index) = 0;

		// An argument as an instance. Raises when it is not one.
		//
		// **The whole reason `ScriptValue` cannot be this interface's currency**,
		// which the header above argues at length: an entity is exactly right
		// here and exactly wrong on a bus.
		//
		// @param index Zero-based, `self` excluded.
		virtual ecs::Entity AsInstance(size_t index) = 0;

		// An argument as whatever attribute it can be.
		//
		// **By the value's own type rather than by a descriptor**, which is the
		// one way an attribute differs from a property: nothing declared it, so
		// what a script passed is what decides. `out.Type` is left `Opaque` for a
		// value with no attribute form, and the caller turns that into a refusal
		// naming the attribute — this does not raise, because `Opaque` is also
		// what an omitted value means and the two are told apart by `IsNil`.
		//
		// @param index Zero-based.
		// @param out   Filled in.
		virtual void ReadAttribute(size_t index, ecs::AttributeValue &out) = 0;

		// Returning. A method calls exactly one of these, or none for a method
		// that answers nothing.
		//@{
		virtual void ReturnNil() = 0;
		virtual void ReturnBoolean(bool value) = 0;
		virtual void ReturnCFrame(const core::CFrame &value) = 0;

		// One instance, or nil for a null entity.
		//
		// **A null lands as nil rather than as a handle to nothing**, which is
		// what every Roblox lookup answers with and what `if player then` reads.
		// The two languages spell nil differently and that is the adapter's
		// business, not a caller's.
		virtual void ReturnInstance(ecs::Entity value) = 0;

		// One attribute, as the value type it holds.
		//
		// An `Opaque` value has no script form and lands as nil, which is what an
		// attribute nobody set reads as.
		virtual void ReturnAttribute(const ecs::AttributeValue &value) = 0;

		// A map from name to value, which is what `GetAttributes` answers.
		//
		// **A map and not a list**, because that is what a caller iterates —
		// `pairs` in one language and `Object.keys` in the other.
		virtual void ReturnAttributes(std::span<const std::pair<core::Name, ecs::AttributeValue>> values) = 0;

		// A handle onto one of `Subject()`'s signals.
		//
		// **The one return whose object is built differently in the two VMs** —
		// `PushSignal` mints a tagged userdata and `MakeJsSignal` an object of a
		// registered class — and it is here rather than left double-bound because
		// that difference is the whole of what an adapter is for. What the two
		// hand back is the same `SignalTable` entry either way, so the *signal* is
		// one thing and only its wrapper is two.
		//
		// @param kind     Which signal.
		// @param property What a `PropertyChanged` connection filters on.
		virtual void ReturnSignal(SignalKind kind, core::Name property) = 0;
		//@}

		// Refuses the call, in the language's own idiom. Never returns.
		//
		// Luau raises a script error and JavaScript throws a `TypeError`; both
		// unwind out of the method body, so there is no error path for a caller
		// to forget. The adapter is what makes that true, which is why this is
		// pure rather than a helper.
		//
		// @param message What the author sees.
		[[noreturn]] virtual void Raise(const char *message) = 0;
	};

	// A method written once.
	using ScriptMethod = void (*)(ScriptCall &call);

	// One entry in the shared instance method table.
	struct InstanceMethod {
		// What a script calls it, in both languages.
		const char *Name;

		ScriptMethod Function;
	};

	// Every instance method that is written once.
	//
	// **Both VMs install every row**, which is what makes the parity a property
	// of the build rather than of somebody checking two files. A method added
	// here is reachable from Luau and from JavaScript in the same commit, and
	// `engine.script.scriptcall` runs the same script in both and compares the
	// answers.
	//
	// @return The table, valid for the life of the program.
	std::span<const InstanceMethod> NeutralInstanceMethods();
}
