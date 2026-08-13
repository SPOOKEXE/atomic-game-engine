#pragma once

// One method, written once, called from either language.
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
// method takes the service table as its receiver and does nothing with it, so it
// is an instance method whose `Subject()` is `NULL_ENTITY` — one adapter per
// language, a second trampoline on each, and no third interface. See
// `ServiceMethod` at the foot of this file and `ServiceSurface.hpp` for what a
// service became.
//
// **A service *property* is the same thing again, and it needed one more row
// type rather than one more interface.** `ServiceProperty` is a name and two
// `ScriptMethod`s — a getter that answers and a setter that reads argument zero
// — so the property surface of `UserInputService` and `SoundService` is written
// against this same interface and installed by the same two adapters. What
// differs is only how each VM hangs an accessor off an object, which is the
// business of `InstallService` and `InstallJsServiceProperties`.
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
#include <engine/core/types/Vector2.hpp>
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

		// How many arguments the author actually wrote.
		//
		// **For a variadic tail and nothing else**, which is
		// `ContextActionService:BindAction(name, handler, touch, ...keys)` — the
		// one method on either surface whose argument list has no end. Every
		// other reader takes a fixed index, and `IsNil` is what an *optional*
		// argument asks: a loop that stopped at the first nil would truncate a
		// key list at a hole where Luau's `lua_gettop` walk skipped it.
		virtual size_t Arguments() const = 0;

		// An argument as a string. Raises when it is not one.
		virtual std::string AsString(size_t index) = 0;

		// An argument as a number. Raises when it is not one.
		virtual double AsNumber(size_t index) = 0;

		// An argument as a truth value, with a fallback for an absent one.
		//
		// **A fallback rather than a plain reader, because the only caller wants
		// one**: `GenerateGUID` wraps in braces unless told otherwise, which is
		// Roblox's surprising default and the shape every optional flag on this
		// surface has.
		//
		// **Each language's own truthiness, which is a divergence worth stating
		// rather than papering over.** Only `nil` and `false` are falsy in Luau,
		// where `0`, `""` and `NaN` are falsy in JavaScript — so
		// `GenerateGUID(0)` wraps in one language and does not in the other. A
		// strict reader that raised for a non-boolean would agree in both and
		// would refuse `if x then`-shaped code every Lua author writes; the
		// engine takes the language over the parity here, and this comment is
		// the record of that.
		//
		// @param index    Zero-based.
		// @param fallback What an absent or nil argument means.
		virtual bool OptionalBoolean(size_t index, bool fallback) = 0;

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

		// An argument as one member of one enum.
		//
		// **Answers rather than raising**, unlike every other reader here, and
		// the exception is what the caller is doing: a variadic key list walks
		// past whatever it cannot read, because `BindAction`'s tail is the one
		// place a script may legitimately pass something that is not a key —
		// Roblox's takes `Enum.UserInputType` members there too and this engine
		// binds keys only.
		//
		// **An `EnumItem` or a bare string**, which is the same latitude a
		// property with `PropertyType::Enum` gives: `part.AlphaMode = "Clip"` is
		// what a migrating script already contains, and refusing it here would
		// make input the one surface that is stricter than the rest.
		//
		// @param index    Zero-based.
		// @param enumName The set the value must belong to.
		// @param member   Filled in with the member's name.
		// @return `false` when the value is not a member of that set.
		virtual bool ReadEnum(size_t index, core::Name enumName, core::Name &member) = 0;

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

		// An argument as the shared value tree.
		//
		// **The one reader that answers a failure rather than raising**, and the
		// exception is the message: a table holding a function or an instance is
		// something a caller has to *name* — "the value holds something with no
		// JSON form" and "the data cannot cross a world boundary" are two
		// different sentences about one refusal, and only the method knows which
		// one it is saying. See `Describe` and `HttpService.cpp`'s
		// `DescribeForJson`.
		//
		// @param index Zero-based.
		// @param out   Filled in.
		// @param why   Set when the answer is `false`.
		// @return `false` when the value has no representation.
		virtual bool ReadValue(size_t index, ScriptValue &out, CodecStatus &why) = 0;

		// Keeps an argument that is a function, and hands back the VM's name for
		// it. Raises when it is not one.
		//
		// **The opaque handle `SignalTable` already proves works.** A Luau
		// callable is a registry ref and a JavaScript one an index into
		// `JsContext::Callables`; nothing shared may interpret either, so a
		// `ContextActionService` handler crosses as the same `CallbackRef` a
		// connection does — which is what lets `ActionStack` hold the rules and
		// neither VM hold a second copy of them.
		//
		// The caller owns what comes back and must hand it to `ReleaseCallback`
		// when it lets go, exactly as `SignalTable::Disconnect`'s caller does.
		//
		// @param index Zero-based.
		// @return The VM's name for the callable.
		virtual CallbackRef RetainCallback(size_t index) = 0;

		// Lets go of one, so its closure can be collected.
		//
		// @param callback What `RetainCallback` handed back.
		virtual void ReleaseCallback(CallbackRef callback) = 0;

		// Returning. A method calls one of these, none for a method that answers
		// nothing, or **several for a method that answers several things**.
		//
		// **More than one answer is a Luau shape and JavaScript packs it into an
		// array**, which is the one place this interface lets the two spellings
		// differ — `SoundService:GetListener()` is `(Enum.ListenerType,
		// Instance?)` in Roblox's own documentation, and the alternatives were to
		// invent a record type for one service's shape or to leave the method
		// Luau's alone. It is the same class of difference as a Luau array being
		// one-based: the *answer* is one thing and only its spelling is two.
		//
		//     local mode, ear = SoundService:GetListener()
		//     const [mode, ear] = SoundService.GetListener()
		//@{
		virtual void ReturnNil() = 0;
		virtual void ReturnBoolean(bool value) = 0;
		virtual void ReturnNumber(double value) = 0;
		virtual void ReturnCFrame(const core::CFrame &value) = 0;

		// One `Vector2`, which is what a pointer position is.
		//
		// **Its own return rather than two numbers**, because `Vector2` is a
		// datatype both VMs already build and `GetMouseLocation` answers with one
		// in Roblox. The pair that wanted it is `UserInputService`'s.
		virtual void ReturnVector2(const core::Vector2 &value) = 0;

		// One member of one enum, as an `EnumItem`.
		//
		// **The counterpart of `ReadEnum`, and a surface whose getter and setter
		// disagree about a type is a round trip that does not close** —
		// `UserInputService.MouseBehavior` is written with an `Enum.MouseBehavior`
		// and has to read back as one.
		//
		// **Not a `ScriptValue`, and that distinction is the whole reason this is
		// a separate return.** `ValueTag` has no tag for an `EnumItem` and must
		// not gain one, because a `ScriptValue` crosses a world; an `EnumItem`
		// handed back from a method crosses nothing, so each VM simply builds its
		// own. That is why this exists and `GetBoundActionInfo` is still written
		// twice: a *record* holding enum members has no neutral form, and one
		// member does.
		virtual void ReturnEnum(core::Name enumName, core::Name member) = 0;

		// A list of members of one enum, as the array each language means by one.
		// `UserInputService:GetKeysPressed` is what wanted it.
		virtual void ReturnEnums(core::Name enumName, std::span<const core::Name> members) = 0;

		// A list of input reports, as the language's own `InputObject`s.
		//
		// **The wrapper is per language and the report is not** — a tagged
		// userdata on one side and an object of a registered class on the other,
		// over one `InputReport` — which is the split `ReturnSignal` is on.
		// `UserInputService:GetMouseButtonsPressed` answers with these rather than
		// with `EnumItem`s, which is Roblox's shape: the object carries where the
		// pointer was as well as which button it is.
		virtual void ReturnInputObjects(std::span<const InputReport> reports) = 0;

		// One string, bytes and all.
		//
		// **A `string_view` and not a `const char *`**, because a JSON document
		// and a URL escape are both built with embedded zeroes possible in them
		// — a Luau string is bytes and nothing in this module decodes an
		// encoding, so a length-carrying view is the only form that does not
		// truncate at the first one.
		virtual void ReturnString(std::string_view value) = 0;

		// A list of strings, as the one-based array each language means by one.
		//
		// **Views rather than owned strings**, because every caller is handing
		// back `core::Name::Text()` on interned names, which outlive the call by
		// construction. A list that had to allocate a `std::string` per entry
		// would be an allocation per mesh in a catalogue read every frame by a
		// scene laying itself out.
		virtual void ReturnStrings(std::span<const std::string_view> values) = 0;

		// A list of instances, as the one-based array each language means by
		// one. `CollectionService:GetTagged` is what wanted it.
		virtual void ReturnInstances(std::span<const ecs::Entity> values) = 0;

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

		// The shared value tree, as the language's own value.
		//
		// **What a record with no datatype in it is returned as**, and
		// `ContentService:GetFlipbook` is the case that decided it: three
		// numbers under three names is exactly a `ValueTag::Map`, both VMs
		// already push one, and a `ReturnRecord` invented for it would be a
		// return type per service shape on an interface that is supposed to
		// carry what its callers ask for. A record that needed an `EnumItem` or
		// an `Instance` in it could not use this — see `ScriptValue` — and that
		// refusal is the useful half: it is why `GetBoundActionInfo` is still
		// written twice.
		//
		// Arrays land **one-based** in Luau, because `#` and `ipairs` mean
		// one-based and a zero-based array is a list whose first element is
		// invisible.
		virtual void ReturnValue(const ScriptValue &value) = 0;
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

	// One method on a service, written once.
	//
	// **The same row as `InstanceMethod` and deliberately a different type.**
	// They are installed by different loops onto different objects and the two
	// tables must not be interchangeable — a service method reads `Subject()`
	// and finds nothing, and an instance method installed on a service table
	// would be reachable on a service and on nothing else. One name apiece is
	// what makes a wrong list a compile error.
	//
	// @since v0.16
	struct ServiceMethod {
		const char *Name;

		ScriptMethod Function;
	};

	// One live property on a service, written once.
	//
	// **A property is not a method, and the two mechanisms stay apart because
	// the VMs disagree about which is hard.** A `ScriptMethod` is a call; a
	// property is an accessor, and a language reaches an accessor by a route of
	// its own — Luau through a userdata's `__index`, because `luaL_sandbox`
	// enables `safeenv` and a field read off a constant global *table* compiles
	// to a `GETIMPORT` resolved once, so a live value reads as a frozen one;
	// JavaScript through `JS_DefinePropertyGetSet`, which runs on every read and
	// needs no such trick.
	//
	// **A list rather than a catch-all `__index`, and that is what let the two
	// property-bearing services cross.** The Luau half could be one
	// `lua_CFunction` string-comparing a field name, and was; a JavaScript
	// accessor is registered *per name*, so the names have to be data. Both sides
	// walk this list now, which is also strictly better on the Luau side than the
	// chain of `if (field == ...)` it replaced.
	//
	// @since v0.16
	struct ServiceProperty {
		// What a script reads and writes.
		const char *Name;

		// Answers the value. Takes no arguments and returns exactly one.
		ScriptMethod Get;

		// Takes the new value as argument **zero**. Null for a read-only
		// property, which is most of them — and a write to one is refused by
		// name in both languages rather than being dropped.
		ScriptMethod Set;
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
