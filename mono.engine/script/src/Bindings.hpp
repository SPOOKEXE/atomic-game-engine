#pragma once

// What a script can hold and what it can touch, on the Luau side.
//
// Two halves, and they meet at `PropertyType`. The value types are what a
// property's bytes mean on the script side; the instance binding is what turns
// a name and one of those values into `Store::SetProperty`.
//
// **The marshalling is a switch over `PropertyType` and nothing else.** No
// per-property code, no table of special cases: a property added to `scene`
// tomorrow is reachable from Luau today, because the binding never learned any
// property's name. That is the payoff for making a property a conversion.

#include "Changes.hpp"
#include "Codec.hpp"
#include "Debris.hpp"
#include "ServiceCatalogue.hpp"
#include "Signals.hpp"
#include "Tasks.hpp"
#include "Tweens.hpp"

#include <engine/core/Random.hpp>
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
#include <engine/script/Debugger.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <lua.h>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// Userdata tags. Luau checks these on every access, so a `Color3` handed to
	// something expecting a `Vector3` is caught by the VM rather than by a
	// reinterpret_cast that happens to line up — the two are three floats each.
	//
	// **Values are explicit and must not be reordered.** Nothing serialises one,
	// so this is not rule 4 — it is that a tag is compared against a userdata
	// created earlier in the same process, and renumbering mid-edit is the kind
	// of change that produces a type confusion nothing reports.
	enum : int {
		TAG_VECTOR3 = 1,
		TAG_COLOR3 = 2,
		TAG_CFRAME = 3,
		TAG_INSTANCE = 4,

		// **Retired at v0.7, and the number is held down rather than reused.**
		// This tagged `workspace`, back when `workspace` was the world itself
		// rather than an instance in it. It is a `TAG_INSTANCE` now — see
		// `OpenWorkspace` for why the two notions were collapsed.
		//
		// Not deleted, because the paragraph above this enum gives the reason:
		// a tag is compared against userdata created earlier in the same
		// process, and handing 5 to a new type is how a type confusion nothing
		// reports gets introduced.
		TAG_WORLD_RETIRED = 5,

		// v0.6's datatype vocabulary.
		TAG_VECTOR2 = 6,
		TAG_UDIM = 7,
		TAG_UDIM2 = 8,
		TAG_RECT = 9,
		TAG_REGION3 = 10,
		TAG_NUMBER_RANGE = 11,
		TAG_NUMBER_SEQUENCE = 12,
		TAG_COLOR_SEQUENCE = 13,
		TAG_TWEEN_INFO = 14,
		TAG_RAY = 15,
		TAG_RANDOM = 16,

		// The signal surface. A signal is a handle onto `SignalTable`; a
		// connection is a handle onto one entry in it.
		TAG_SIGNAL = 17,
		TAG_CONNECTION = 18,

		// One member of one enum, which is a pair of interned names.
		TAG_ENUM_ITEM = 19,

		// What a raycast is told to ignore.
		TAG_RAYCAST_PARAMS = 20,

		// One stop in a sequence.
		//
		// **Added at v0.10 because a particle emitter made the table form
		// insufficient**, which is a reversal of the note above
		// `ReadNumberKeypoint` and worth recording rather than quietly editing
		// out. That note said two more userdata types for a value an author
		// writes inline once was surface nobody asked for, and while a sequence
		// was only ever constructed from a literal it was right.
		//
		// What changed is that a sequence is now a *property*: an emitter's
		// `Transparency` is read back, and `emitter.Transparency.Keypoints[1]`
		// handed back a bare `{time, value, envelope}` table — three anonymous
		// numbers with no `typeof`, no way to tell one from a `ColorSequence`'s
		// stop, and nothing to compare against. Reading a value back in a shape
		// its own constructor accepts is the round trip a property surface owes,
		// and the table form does not give it a name.
		//
		// The table form still works everywhere it did. Both constructors take
		// either.
		TAG_NUMBER_KEYPOINT = 21,
		TAG_COLOR_KEYPOINT = 22,

		// `UserInputService`, which is a userdata rather than a table.
		//
		// **Because a table cannot hold a property that changes.** `luaL_sandbox`
		// freezes the globals and enables `safeenv`, and Luau then compiles
		// `Service.Field` on a constant global table into a `GETIMPORT` resolved
		// once per closure — so `UserInputService.MouseBehavior` would read
		// whatever it was the first time a script asked, forever. A userdata's
		// field access always goes through `__index`.
		//
		// It carries no payload. What the object is, is its metatable.
		TAG_INPUT_SERVICE = 23,

		// `SoundService`, a userdata for `TAG_INPUT_SERVICE`'s reason: it has a
		// live `Volume`, and a property on a frozen global table is a
		// `GETIMPORT` resolved once. Carries no payload either.
		TAG_SOUND_SERVICE = 24,

		// What an input signal hands its listener.
		//
		// **Roblox's `InputObject`, and the reason it is a userdata rather than
		// a table is not `safeenv` this time** — it is that a table would be a
		// value a handler could write into and hand on, and an input report is
		// a fact about a frame rather than a document. The tag is also what
		// makes `typeof` answer `"InputObject"`.
		TAG_INPUT_OBJECT = 25,

		// What `TweenService:Create` hands back.
		//
		// **A userdata of its own rather than the ordinary instance handle**,
		// even though a tween *is* an entity — see `Tweens.hpp`. The neutral
		// instance methods are installed flat on every instance, so a `Play`
		// there would claim the name for every part and folder in the engine,
		// and `Play` is a name Roblox puts on three classes.
		//
		// Carries the tween's entity, which is the key to `TweenTable` and the
		// subject of its `Completed`.
		TAG_TWEEN = 26,
	};

	// One action bound through `ContextActionService`.
	//
	// **Declared here rather than in `InputServices.cpp` because it lives on the
	// context**, and it lives on the context because it holds a registry
	// reference: a `thread_local` would be shared between two VMs on one thread,
	// so a reference minted by one would be dereferenced against the other's
	// state. That is not a leak — it is a `lua_getref` into a different VM, which
	// is undefined and crashed a test that had nothing to do with input.
	//
	// @since v0.10
	struct BoundAction {
		// What the script called it. Unbinding is by this name.
		std::string Name;

		// The keys it claims, as `scene::KeyCode` ordinals.
		//
		// **Ordinals rather than the enum**, so this header does not have to name
		// `scene` — `Bindings.hpp` is included by every binding file and widening
		// it for one vector is the kind of edge that spreads.
		std::vector<uint16_t> Keys;

		// The registry reference to the handler.
		int Callback = -1;

		// Higher wins. Roblox's default is zero and so is this.
		int Priority = 0;
	};

	// Everything one Luau runtime needs, hung off the state rather than a
	// static.
	//
	// **One runtime, one world.** Two runtimes over two worlds must not be able
	// to reach each other's storage, and a file-static would have made that
	// mistake available — the sort that works until the second world exists.
	// Every bound function reaches this through a light-userdata upvalue.
	//
	// @since v0.6
	struct LuauContext {
		// The world this VM builds into.
		ecs::Store *World = nullptr;

		// What the host is, for `RunService:IsServer()` and friends.
		HostRole Role;

		// Connections, and the ordering rules both VMs share.
		SignalTable Signals;

		// What changed since the last barrier, as property names.
		ChangeQueue Changes;

		// Suspended threads and when they may run again.
		TaskQueue Tasks;

		// What `ContextActionService` has bound, highest priority first.
		//
		// **Sorted at bind time rather than searched at press time**, because
		// binding is rare where pressing is not — and because the order has to be
		// stable, which a sort on every press could not promise.
		std::vector<BoundAction> Actions;

		// The VM this context belongs to, so a `Connection` userdata can reach
		// the table without a second upvalue on every method.
		lua_State *State = nullptr;

		// Whether `Store::OnDescendantRemoving` already holds this VM's hook.
		//
		// The store keeps one listener, so a second install would replace the
		// first with an identical one — harmless, and still worth not doing
		// once per connection.
		bool RemovingHooked = false;

		// Where execution should be reported from, owned by the `Runtime`.
		//
		// A pointer rather than a copy: breakpoints are edited from the editor
		// while the world runs, and a copy taken at construction would be a
		// second answer to what is armed.
		Debugger *Breakpoints = nullptr;

		// What each `ModuleScript` evaluated to, by entity id.
		//
		// **A registry ref per module, and the module runs once.** Roblox's rule:
		// every `require` of one module hands back the same value, so a module
		// with a side effect at its top level has that side effect once — on
		// whichever script required it first. A map that re-ran would make
		// module order something an author had to reason about.
		//
		// Keyed by `Entity::Id` rather than by path, because two instances may
		// name one file and they are two modules. That is what makes a module a
		// thing in the tree rather than a thing on disk.
		std::unordered_map<uint64_t, int> Modules;

		// Modules part way through evaluating, innermost last.
		//
		// **Cycle detection, and it has to be by instance.** `a` requiring `b`
		// requiring `a` would otherwise recurse until the C stack ran out, which
		// surfaces as a crash with no line number rather than as a script error
		// naming the two files.
		std::vector<ecs::Entity> Loading;

		// The script currently being run by `RunInstance`, so a captured hit can
		// say which one it came from.
		//
		// Set around one run and cleared after it. Null during a heartbeat,
		// which is honest — the connection that is running was made by a script
		// and nothing records which, so naming one would be a guess.
		ecs::Entity RunningScript;

		// The script instance the next chunk belongs to.
		//
		// **Set before `Run` and consumed by it**, rather than assigned as a
		// global here — `luaL_sandboxthread` gives each chunk its own global
		// table, so `script` has to be written onto the thread after it is
		// sandboxed and not onto the state before. That per-chunk scoping is the
		// point: one script's `script` is invisible to the next.
		ecs::Entity PendingScript;

		// Which registry ref holds each suspended thread.
		//
		// `TaskQueue` holds a `CallbackRef` and a script holds a thread object;
		// this is what joins the two, and it is what makes `task.cancel`
		// possible at all. Keyed on the thread's `lua_State *`, which is its
		// identity for as long as the ref keeps it alive.
		std::unordered_map<lua_State *, CallbackRef> Threads;

		// How many ticks a `task.wait` asked for, so the resume can report how
		// long it actually waited.
		std::unordered_map<lua_State *, uint64_t> WaitTicks;

		// How many arguments a `task.defer` or `task.delay` left on its
		// thread's stack, so the resume passes them on.
		std::unordered_map<lua_State *, int> PendingArguments;

		// What this program offers beyond the world, or null.
		//
		// **Null for a game script and set for a tool**, which is the whole of
		// the distinction: a game's vocabulary is the scene and an editor tool's
		// is the program running it. See `script/Host.hpp`.
		HostSurface *Host = nullptr;

		// Which registry ref holds each function a host was handed.
		//
		// **Keyed by a counter rather than by an address**, because an address
		// is not stable between runs and this module refuses one in an id
		// anywhere. Starting at one leaves zero meaning "no callback".
		//@{
		std::unordered_map<uint64_t, int> HostCallbacks;
		uint64_t NextHostCallback = 0;
		//@}

		// Which thread is suspended on which `world::Ticket`.
		//
		// **The first of `docs/retired/SCRIPT_CONCURRENCY.md` §1's three legal resume
		// sources**, made concrete. A `GetAsync` returns a ticket, the reply
		// lands at a later barrier applied in sorted order, and this is what
		// joins the reply back to the script waiting for it. Keyed on the
		// ticket's value, which is per world and monotonic.
		std::unordered_map<uint64_t, lua_State *> AwaitedTickets;

		// How many GUIDs `HttpService:GenerateGUID` has handed out.
		//
		// **A counter rather than a clock or an entropy source, and that is what
		// makes a GUID replayable.** `HttpService.cpp` carries the whole
		// argument; what it needs from here is a draw number that advances the
		// same way on every run of the same script. On the context rather than a
		// file-static for this file's own reason — two runtimes over two worlds
		// must not share one.
		uint64_t NextGuid = 0;

		// Every tween this VM has made, and what is waiting to be destroyed.
		//
		// **Beside `Signals`, `Changes` and `Tasks`, and for their reason.**
		// Both step on the fixed tick delta at the barrier and both drain in a
		// stated order, so both are shared machinery the two languages must not
		// have two copies of — see `Tweens.hpp` and `Debris.hpp`.
		//@{
		TweenTable Tweens;
		DebrisQueue Debris;
		//@}
	};

	// Reports whether a thread has a scheduled resume.
	//
	// What tells a legal yield from an illegal one: a thread that suspended
	// through `task` is waiting for a tick boundary, which
	// `docs/retired/SCRIPT_CONCURRENCY.md` §1 permits. One that suspended any other way
	// found a route out of the sandbox and is refused.
	//
	// @param context The VM's context.
	// @param thread  The suspended thread.
	// @return `true` when something will resume it.
	bool ThreadIsScheduled(LuauContext &context, lua_State *thread);

	// The context bound to a state.
	//
	// @param state The VM, or one of its threads.
	// @return The context.
	LuauContext &ContextOf(lua_State *state);

	// The context on a bound function's first upvalue.
	//
	// @param state The VM inside a bound C function.
	// @return The context.
	LuauContext &UpvalueContext(lua_State *state);

	// --- values ---------------------------------------------------------------

	// How wide one marshalled value can be, and therefore how big the stack
	// buffers that carry one are.
	//
	// **This was `sizeof(core::CFrame)` until v0.10 added the sequences.** A
	// `core::ColorSequence` is twenty keypoints and does not fit in twenty-eight
	// bytes, and every guard is `size > sizeof(bytes)` — so an emitter's `Color`
	// failed its read with "could not read 'Color'", naming a property that is
	// declared and readable and whose only problem was a buffer one file away.
	//
	// A named constant rather than a `sizeof` at each buffer, because there are
	// several and a getter's being narrower than the setter's is a bug with no
	// symptom on the setter.
	constexpr size_t WIDEST_PROPERTY = std::max(sizeof(core::ColorSequence), sizeof(core::NumberSequence));

	// Pushes a value of one `PropertyType`, read into a buffer of its size.
	//
	// **The type and the enum name, never a descriptor**, because the second
	// caller has no descriptor to give: an ECS component field carries exactly
	// these values and is not a property. One switch rather than two that agree
	// until somebody edits one.
	//
	// `PropertyType::String` is refused rather than handled, and the refusal is
	// the design: `bytes` is uninitialised storage and a `std::string` cannot be
	// assigned into that, so every caller takes strings down a path of its own.
	// Failing loudly is what catches a future caller that forgot the branch.
	//
	// @param state    The VM.
	// @param type     What the bytes mean.
	// @param enumName Which set an `Enum` value belongs to. Ignored otherwise.
	// @param bytes    The value.
	// @return `false` when the type has no script representation.
	// @since v0.12
	bool PushPropertyValue(lua_State *state, ecs::PropertyType type, core::Name enumName, const void *bytes);

	// Reads a Luau value into a buffer of that type's size.
	//
	// Raises a Luau type error for a value of the wrong shape, exactly as the
	// `Check*` helpers below do — so a caller checks the return for "this type
	// cannot cross" and never for "the script passed the wrong thing".
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param type     What to read it as.
	// @param enumName Which set an `Enum` value must belong to.
	// @param out      Where to write it. At least `Schemas::SizeOf(type)` bytes.
	// @return `false` when the type has no script representation, or when an
	//         `Enum` value belongs to a different set.
	// @since v0.12
	bool
	ReadPropertyValue(lua_State *state, int index, ecs::PropertyType type, core::Name enumName, void *out);

	// Installs `Vector3`, `Color3` and `CFrame` as globals.
	void OpenValues(lua_State *state);

	// Installs v0.6's datatypes: `Vector2`, `UDim`, `UDim2`, `Rect`, `Region3`,
	// `NumberRange`, `NumberSequence`, `ColorSequence`, `TweenInfo`, `Ray` and
	// `Random`.
	//
	// **Each is a shim over what the engine already holds**, exactly as
	// `Vector3` and `CFrame` are, and none of them is a component. `Region3` is
	// `core::AABB` and `Ray` is `core::Ray`: the engine had both, and adding a
	// second spelling of either would have been the duplicate the root
	// `AGENTS.md` calls the most expensive kind of debt.
	//
	// @param state The VM.
	void OpenDatatypes(lua_State *state);

	// The easing enums, converted between their C++ form and their member name.
	//
	// `TweenInfo` holds a `core::EasingStyle` and a script names one, so
	// something has to join the two. Here rather than in `core` because the
	// *names* are userland vocabulary — `core/types/TweenInfo.hpp` is L1 and
	// knows nothing about a script.
	core::EasingStyle EasingStyleOf(core::Name member);
	core::Name NameOf(core::EasingStyle style);
	core::EasingDirection EasingDirectionOf(core::Name member);
	core::Name NameOf(core::EasingDirection direction);

	// Pushes a fresh value of each type and returns it for filling in. The
	// instance binding uses these to hand a property's bytes back to a script.
	core::Vector3 *PushVector3(lua_State *state);
	core::Color3 *PushColor3(lua_State *state);
	core::CFrame *PushCFrame(lua_State *state);

	// Reads a value of each type, raising a Luau type error when the argument
	// is something else. The tag is what makes this safe: `Vector3` and
	// `Color3` are the same three floats, so a check on shape would pass.
	core::Vector3 &CheckVector3(lua_State *state, int index);
	core::Color3 &CheckColor3(lua_State *state, int index);
	core::CFrame &CheckCFrame(lua_State *state, int index);

	// The four `gui` is authored in. Same split as the three above: the
	// metatable is `LuauDatatypes.cpp`'s and these only carry a property's
	// bytes across.
	//
	// `CheckVector2Value` rather than `CheckVector2` because that name is
	// already taken by a file-local in `LuauDatatypes.cpp` doing exactly this;
	// merging the two is a tidy-up worth doing and not one to do inside the
	// change that adds four property types.
	core::Vector2 *PushVector2(lua_State *state);
	core::UDim *PushUDim(lua_State *state);
	core::UDim2 *PushUDim2(lua_State *state);
	core::Rect *PushRect(lua_State *state);

	core::Vector2 &CheckVector2Value(lua_State *state, int index);
	core::UDim &CheckUDim(lua_State *state, int index);
	core::UDim2 &CheckUDim2(lua_State *state, int index);
	core::Rect &CheckRect(lua_State *state, int index);

	// --- the three a curve is authored in -------------------------------------
	//
	// The same split again: the metatables are `LuauDatatypes.cpp`'s and these
	// hand a property's bytes across. See `Values.cpp` for what makes these three
	// different from the seven above — they are hundreds of bytes rather than a
	// handful of floats.

	core::NumberRange *PushNumberRange(lua_State *state);
	core::NumberSequence *PushNumberSequence(lua_State *state);
	core::ColorSequence *PushColorSequence(lua_State *state);

	core::NumberRange &CheckNumberRange(lua_State *state, int index);
	core::NumberSequence &CheckNumberSequence(lua_State *state, int index);
	core::ColorSequence &CheckColorSequence(lua_State *state, int index);

	// --- the codec bridge -----------------------------------------------------
	//
	// **One answer to "what is a Lua table", reached from two places.** These are
	// defined in `Services.cpp` because `MessagingService` was the first caller,
	// and they are declared here because they must not gain a second
	// implementation: `Codec.hpp` decides which tables are arrays, what a cycle
	// is and what a key becomes, and `HttpService:JSONEncode` has to give the
	// same answers or one value crosses a bus in one shape and lands in a JSON
	// document in another.

	// Reads a Luau value at `index` into a `ScriptValue`.
	//
	// **The sort is not here**: this walks a table in whatever order Luau offers
	// and the writer sorts, which is the arrangement `Codec.hpp` argues for — a
	// caller that had to remember to sort would be a second place the
	// determinism guarantee could be lost.
	//
	// @param state The VM.
	// @param index The stack index to read. Absolute, not relative.
	// @param out   Filled in.
	// @param depth How deep this value sits. Zero at the top.
	// @param why   Set when the answer is `false`.
	// @return `false` when the value has no representation.
	bool ReadScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why);

	// Pushes a `ScriptValue` as the Luau value it names.
	//
	// Arrays land as **one-based** tables, because `#` and `ipairs` mean
	// one-based in Luau and a zero-based array handed to a script is a list whose
	// first element is invisible.
	//
	// @param state The VM.
	// @param value What to push.
	void PushScriptValue(lua_State *state, const ScriptValue &value);

	// --- signals --------------------------------------------------------------

	// Pushes a signal object onto the stack.
	//
	// **A handle, not a list.** The connections live in `SignalTable`, so two
	// scripts holding "the same signal" are holding the same thing rather than
	// two objects that behave alike.
	//
	// @param state    The VM.
	// @param kind     Which signal.
	// @param subject  The instance, or `NULL_ENTITY` for a world signal.
	// @param property The property to filter on, for `PropertyChanged`.
	void PushSignal(lua_State *state, SignalKind kind, ecs::Entity subject, core::Name property = {});

	// Installs the `RBXScriptSignal` and `RBXScriptConnection` metatables.
	//
	// @param state The VM.
	void OpenSignals(lua_State *state);

	// Calls everything connected to one signal, with the arguments already on
	// the stack.
	//
	// @param state     The VM.
	// @param kind      Which signal.
	// @param subject   The instance, or `NULL_ENTITY`.
	// @param arguments How many values on top of the stack to pass.
	// @return An error message when a handler raised, or empty.
	std::string FireSignal(lua_State *state, SignalKind kind, ecs::Entity subject, int arguments);

	// --- services -------------------------------------------------------------
	//
	// **A surface service is a global table, and there is one way to build one.**
	// `scene/Services.hpp` states the other kind — a *container* service is an
	// instance in the tree with children, properties and a row in the save file,
	// and `scene::InstallServices` builds those ten from one table. This is the
	// half that had no such table: nine engine surfaces spread over five files,
	// each hand-rolling the same ten lines of `lua_newtable`, push-closure loop
	// and `lua_setglobal`, over two different method-array types.
	//
	// Five copies of ten lines is not a crisis on its own. What it costs is that
	// there was nowhere to state a rule about services and have it hold: every
	// method takes the context as upvalue 1 and every one of the five had to
	// remember, `game:GetService(name)` and the global must be **one** object and
	// nothing said so, and adding a tenth service meant copying a sixth.

	// One method on a service's table.
	//
	// A plain function pointer and a name, because that is all a service method
	// is: `ServiceSurface` is what supplies the context every one of them reads.
	struct ServiceMethod {
		const char *Name;
		lua_CFunction Function;
	};

	// One signal exposed as a field on a service's table.
	//
	// **A field and not a method**, which is Roblox's shape and is why this is
	// its own list: `RunService.Heartbeat:Connect(f)` reads a value and calls a
	// method *on that value*, where `RunService:IsServer()` calls a method on the
	// service. The two cannot share a list because they are not built the same
	// way — a signal is pushed by `PushSignal` and carries no context upvalue.
	struct ServiceSignal {
		const char *Name;
		SignalKind Kind;
	};

	// Everything a surface service is, as data.
	//
	// **Data rather than a base class**, for the reason the container half is a
	// table of rows: what varies between services is a name and three lists, and
	// the code that turns those into a global is the same code every time. A
	// service that needs something genuinely new adds a field here, once, where
	// every service can see it — rather than a tenth private copy of the loop
	// that nothing else can learn from.
	struct ServiceSurface {
		// What the global is called, and therefore what
		// `game:GetService(name)` finds. `RunService::GetService` looks in the
		// globals before it looks at the tree, so naming it here is the whole of
		// making it resolvable — and it is why the global and the service are
		// **one** table rather than two objects a script could tell apart.
		const char *Name = nullptr;

		std::span<const ServiceMethod> Methods;

		// Pushed as fields before the methods, so a service may name a signal
		// and a method the same thing and get the method. Nothing does; the
		// order is stated so that if anything ever does, it is decided here
		// rather than by which loop ran last.
		std::span<const ServiceSignal> Signals;

		// Property access, or null for a service with no properties.
		//
		// **Setting `Index` changes what the service *is*, from a table to a
		// userdata, and that is forced rather than stylistic.** `luaL_sandbox`
		// enables Luau's `safeenv`, which lets the compiler turn a constant
		// global and a constant field into a `GETIMPORT` — resolved **once** and
		// cached in the closure. On a table, the first read of a property wins
		// forever, so a property that changes reads as one that does not. It was
		// found by watching `__index` fire for the first read of
		// `UserInputService.MouseBehavior` and not for the second, with no raw
		// key on the table to explain it.
		//
		// A userdata's field access is never an import, so every read through a
		// local goes to `__index` — which is the form a Roblox script is written
		// in anyway, since `game:GetService` is a method call and cannot be an
		// import. `DEFERRED.md` D00030 records the edge that remains: the same
		// property read off a *bare global* still caches.
		//
		// So a property-bearing service needs `Tag` and `MethodsKey` as well,
		// because a userdata has no fields to hold its methods in.
		//
		// `NewIndex` may be null while `Index` is not, which is a service with
		// read-only properties.
		//@{
		lua_CFunction Index = nullptr;
		lua_CFunction NewIndex = nullptr;
		//@}

		// The userdata tag, from `Bindings.hpp`'s tag block. Required when
		// `Index` is set and ignored otherwise.
		int Tag = 0;

		// Where the method table is stashed for `Index` to find, since a
		// userdata cannot carry one. Required when `Index` is set and ignored
		// otherwise; the service's `Index` reads the same key.
		const char *MethodsKey = nullptr;
	};

	// Builds one surface service and sets it as a global.
	//
	// **Every method gets the runtime's `LuauContext` as upvalue 1**, which is
	// how any of them reach the store, the world and the universe — see
	// `UpvalueContext`. That was the one invariant the five hand-rolled copies
	// each had to remember separately, and it is the one a tenth copy would have
	// been most likely to forget: a method installed with `lua_pushcfunction`
	// instead compiles, links, runs, and reads a garbage pointer.
	//
	// **Must run before `luaL_sandbox`**, which freezes the global table. A
	// service installed after it is silently absent.
	//
	// @param state   The VM.
	// @param surface What to build.
	void InstallService(lua_State *state, const ServiceSurface &surface);

	// Every service this language binds, from the catalogue.
	//
	// **The list is `ServiceCatalogue.cpp`'s and not this file's**, which is what
	// makes "which services exist" one fact rather than one per VM. See
	// `ServiceCatalogue.hpp` for why it is a table naming installers rather than
	// a registrar per service file.
	//
	// **Two phases, because they install at different moments.**
	// `ServiceAvailability::Always` runs early, with the rest of the vocabulary.
	// `Studio` runs late — `BreakpointService` reads `LuauContext::Breakpoints`
	// to decide whether to install at all, so it cannot run before that pointer
	// is set, and it writes a global, so it cannot run once `luaL_sandbox` has
	// frozen the table.
	//
	// A service this language does not bind installs nothing. The refusal is in
	// `GetService`, which is the only place a script can tell the difference.
	//
	// @param state The VM.
	// @param phase Which set to install.
	// @since v0.15
	void InstallLuauServices(lua_State *state, ServiceAvailability phase);

	// Installs `RunService`, whose `Heartbeat` a script connects behaviour to,
	// and whose `IsServer`/`IsClient`/`IsStudio` say where a script is standing.
	void OpenRunService(lua_State *state);

	// Installs `UserInputService` and `ContextActionService`.
	//
	// **Both read `scene::InputState` and neither reads `engine::input`**, which
	// is the tier seam `Input.hpp` exists for: this module is `shared` and the SDL
	// pump is `client`.
	void OpenUserInputService(lua_State *state);
	void OpenContextActionService(lua_State *state);

	// Turns this frame's input edges into bound actions and input signals.
	//
	// Called at a barrier beside `PumpChanges`, for the same reason: a handler
	// that mutated the world would otherwise do it in the middle of a loop over
	// it.
	//
	// **Five things now and one until v0.16**: the focus edges, key edges, mouse
	// button edges, pointer motion and the wheel — in that order, so a listener
	// sees `WindowFocusReleased` before the releases losing focus caused. Every
	// signal but the focus pair is handed an `InputObject`, which is Roblox's
	// shape and which this passed nothing resembling before; `InputServices.cpp`
	// carries what the three previous answers were.
	//
	// @param state The VM.
	// @return An error message when a handler raised, or empty.
	std::string PumpInput(lua_State *state);

	// Installs `game`, whose `GetService` is how a Roblox script reaches one.
	void OpenGame(lua_State *state);

	// Installs the Universe's services — the only route out of a world.
	//
	// `MessagingService`, `MemoryStoreService` and `DataStoreService`. The last
	// two want a reply, and a reply arrives at a later barrier, so a script
	// **yields** on one — which is legal under `docs/retired/SCRIPT_CONCURRENCY.md` §1
	// precisely because the barrier applies replies in a deterministic order.
	void OpenBusSupport(lua_State *state);
	void OpenMessagingService(lua_State *state);
	void OpenTeleportService(lua_State *state);
	void OpenMemoryStoreService(lua_State *state);
	void OpenDataStoreService(lua_State *state);

	// Installs `CrossWorldService`, the addressed route out of a world.
	//
	// **`MessagingService` is a fan-out and this is a channel**, which is the
	// whole distinction: a topic has no destination, so a game saying one thing
	// to one world had to broadcast it to everybody or send a player carrying
	// it. `world::BusKind::Channel` is the kind, appended beside `Teleport`
	// because a channel is a teleport with nobody attached.
	//
	// @since v0.15
	void OpenCrossWorldService(lua_State *state);

	// Installs `ContentService`, which answers what content this world holds.
	//
	// **The other half of rule 4.** A script names an asset and had no way to
	// ask what the names were, so every demo carried string literals for files
	// that only existed if somebody had baked that exact tree. See
	// `ContentService.cpp`.
	//
	// @param state The VM.
	// @since v0.10
	void OpenContentService(lua_State *state);

	// Installs `CollectionService`, which answers what carries a tag.
	//
	// **The other side of `Instance:AddTag`.** The same three methods, plus the
	// one neither the instance surface nor anything else in the engine could
	// answer from a script: `GetTagged`. A scene that wanted every door had to
	// keep its own list beside the tags, which is rule 2's second copy.
	//
	// No `GetInstanceAddedSignal`, and `CollectionService.cpp`'s header says
	// what firing one honestly would take — a change record in `scene::AddTag`
	// and a pump at the barrier. A signal that never fired would read as a
	// broken engine, which is the trade `v0.5` records for `Heartbeat`.
	//
	// @param state The VM.
	// @since v0.15
	void OpenCollectionService(lua_State *state);

	// Installs `HttpService` — **the half of it that observes nothing**.
	//
	// `JSONEncode`, `JSONDecode`, `GenerateGUID` and `UrlEncode`, and no
	// `RequestAsync`, `GetAsync` or `PostAsync`. Arbitrary outbound HTTP from a
	// game script is a security decision nobody has taken, and this engine's one
	// existing route to the network is a signed manifest verified against a
	// publisher key — a different thing, not a smaller one. `HttpService.cpp`
	// carries the argument and the note asking the next reader not to add the
	// three by reflex.
	//
	// @param state The VM.
	// @since v0.15
	void OpenHttpService(lua_State *state);

	// Installs `SoundService` — **the part of it that is not the mixer**.
	//
	// A `Volume` and a listener, over `scene::AudioState`. `engine::audio` is L12
	// `client` and this module is L9 `shared`, so nothing here can name a mixer:
	// the seam is a resource on the world that `client::SoundStage` reads, which
	// is the arrangement `scene::InputState` established. `SoundService.cpp`
	// lists the eleven Roblox members that are absent and what each would need
	// first.
	//
	// @param state The VM.
	// @since v0.16
	void OpenSoundService(lua_State *state);

	// Installs `TweenService`, and the `Tween` metatable it hands back.
	//
	// **`GetValue` is the whole easing surface a script can reach without
	// building anything**, and `Create` is the rest: a target, a `TweenInfo` and
	// a map from property name to where it should end up. See `Tweens.hpp` for
	// what a tween is, why it is an entity, and why its handle is a userdata of
	// its own rather than an ordinary instance.
	//
	// @param state The VM.
	// @since v0.16
	void OpenTweenService(lua_State *state);

	// Pushes a `Tween` userdata for a tween's entity.
	//
	// @param state The VM.
	// @param tween The tween's entity, from `TweenTable::Create`.
	// @since v0.16
	void PushTween(lua_State *state, ecs::Entity tween);

	// Advances every tween by one tick and fires what completed.
	//
	// **Called at the head of the barrier, before the input pump** — see
	// `LuauRuntime::Heartbeat`, which states the whole order and why the world's
	// own timed work goes first.
	//
	// @param state The VM.
	// @param delta The **fixed tick delta**. Never wall time: a tween advanced
	//        by how long the last frame took puts the scene somewhere else on a
	//        busy machine, and the recording stops replaying.
	// @return An error message when a `Completed` handler raised, or empty.
	// @since v0.16
	std::string PumpTweens(lua_State *state, float delta);

	// Installs `Debris`, whose one method destroys an instance later.
	//
	// @param state The VM.
	// @since v0.16
	void OpenDebrisService(lua_State *state);

	// Destroys everything whose deadline the world has reached.
	//
	// Beside `PumpTweens` and immediately after it, for the reason
	// `LuauRuntime::Heartbeat` gives: an instance's last tick of motion happens
	// before it is taken away.
	//
	// @param state The VM.
	// @since v0.16
	void PumpDebris(lua_State *state);

	// Installs `require`, which is the only route to a `ModuleScript`.
	//
	// **Defined beside the compiler rather than with the other globals**, because
	// evaluating a module is loading a chunk — the same compile, the same
	// sandboxed thread and the same `script` global that a `Script` gets. A
	// second loader here would be a second answer to what running a program
	// means.
	void OpenRequire(lua_State *state);

	// Installs `task`, and the yield rule made real.
	void OpenTask(lua_State *state);

	// Installs `RaycastParams` and `workspace:Raycast`.
	//
	// **The one part of the datatype vocabulary that is a query rather than a
	// value.** Against `physics::Raycast` and the exact shapes, not
	// `spatial::Raycast` and the proxy boxes: a ray that reported the bounding
	// box of a rotated part would be plausible and wrong.
	//
	// Called after `OpenWorkspace`, because it adds to the world's method table.
	void OpenQueries(lua_State *state);

	// Installs `World`, and the component methods every instance gains.
	//
	// **The storage named directly, underneath the Roblox vocabulary the rest of
	// this file installs.** A game's own data — a health, a cooldown, an
	// inventory slot — has been an attribute or a C++ component until now, and
	// neither is a component a query can reach without a rebuild.
	//
	// Called after `OpenInstances`, because the instance half of the surface adds
	// to the method table that function creates.
	//
	// @param state The VM.
	// @since v0.12
	void OpenEcs(lua_State *state);

	// Installs `BreakpointService`, when this runtime is a studio's.
	//
	// **Absent outside a studio**, so `game:GetService("BreakpointService")`
	// fails the way it does for any service this engine does not provide. Arming
	// a breakpoint switches Luau's step mode on and costs the whole runtime its
	// speed, which a shipped server has no business letting a game script do.
	//
	// @param state The VM.
	// @since v0.12
	void OpenBreakpointService(lua_State *state);

	// Installs `host`, when the runtime has one.
	//
	// **One closure per `HostSurface::Names` entry**, so a name the host does
	// not list is not a member — which turns a typo into "attempt to call a nil
	// value" at the call site rather than a refusal from inside a program the
	// author cannot see.
	//
	// Does nothing when there is no host, which is every game script.
	//
	// @param state The VM.
	// @since v0.12
	void OpenHost(lua_State *state);

	// Calls a function a script handed the host.
	//
	// @param state     The VM.
	// @param callback  What to call.
	// @param arguments What to pass.
	// @return `false` when the callback is unknown or the handler raised.
	// @since v0.12
	bool CallHostCallback(lua_State *state, HostCallback callback, HostArguments arguments);

	// Lets go of one, so its closure can be collected.
	//
	// @param state    The VM.
	// @param callback What to release.
	// @since v0.12
	void ReleaseHostCallback(lua_State *state, HostCallback callback);

	// Installs `Enum`, over the sets `ecs::EnumTable` holds.
	void OpenEnums(lua_State *state);

	// Pushes one member of one enum as an `EnumItem`.
	//
	// @param state    The VM.
	// @param enumName Which set.
	// @param member   Which member.
	void PushEnumItem(lua_State *state, core::Name enumName, core::Name member);

	// Reads an `EnumItem` or a bare string as a member name.
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param enumName The set the value must belong to.
	// @param out      Filled in with the member's name.
	// @return `false` when the value is a member of a different enum, or is
	//         neither an `EnumItem` nor a string.
	bool ReadEnumValue(lua_State *state, int index, core::Name enumName, core::Name &out);

	// Reads an `EnumItem` of any set, for a reader that does not know which one
	// to expect.
	//
	// **What `HostSurface` needs and `ReadEnumValue` cannot give it.** A host
	// call is a generic bridge — it converts whatever a script passed without
	// knowing what the host will do with it — so it has no enum name to check
	// against. This answers "is this an EnumItem, and which member" and leaves
	// the checking to whoever asked.
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param enumName Filled in with the set it belongs to.
	// @param member   Filled in with the member's name.
	// @return `false` when the value is not an `EnumItem`.
	bool ReadAnyEnumValue(lua_State *state, int index, core::Name &enumName, core::Name &member);

	// --- the camera -----------------------------------------------------------

	// Pushes the world's live camera, or nil when nothing has made one.
	void PushCurrentCamera(lua_State *state);

	// Makes the instance at `index` the world's live camera.
	//
	// @param state The VM.
	// @param index The stack index holding a `Camera` instance, or nil.
	void SetCurrentCamera(lua_State *state, int index);

	// Dispatches this tick's deliveries to their subscribers.
	//
	// @return An error message when a subscriber raised, or empty.
	std::string PumpDeliveries(lua_State *state, ecs::Store &store);

	// Fires `.Changed` for everything the last barrier recorded.
	//
	// @return An error message when a handler raised, or empty.
	std::string PumpChanges(lua_State *state);

	// Delivers everything the tree recorded since the last barrier.
	//
	// **Beside `PumpChanges` and for the same reason.** A reparent cannot fire
	// a signal from inside `SetParent` — the handler would re-enter the VM with
	// the sibling list half-relinked — so the store writes it down and this
	// hands it over one tick later. See `ecs::TreeChange`.
	//
	// @param state The VM to deliver into.
	// @return The first error a handler raised, or empty.
	std::string PumpTree(lua_State *state);

	// Delivers what a pointer did to the 2D tree since the last beat.
	//
	// **Beside `PumpTree` and for the same reason one door along**: the events
	// were produced by `gui::Router` while a host walked a compiled draw list,
	// and a handler that destroys the element it was called about would pull
	// that list out from under the walk. `Runtime::DeliverGuiEvents` writes them
	// down and this hands them over at the barrier.
	//
	// **The arguments each signal gets, and the one that is deliberately
	// empty.** `MouseEnter`, `MouseLeave` and `MouseMoved` are called with
	// `(x, y)` in canvas pixels, which is Roblox's signature exactly.
	// `Activated`, `InputBegan` and `InputEnded` are called with **nothing**:
	// Roblox passes an `InputObject`, and an argument nobody can rely on is worse
	// than an argument that is not there — the same trade `VideoFrame` and
	// `AutomaticSize` were decided on.
	//
	// **The datatype exists since v0.16 and this still passes nothing**, which is
	// a narrower gap than it was and worth stating rather than closing by reflex.
	// `gui::GuiEvent` carries a kind, an entity and two points — no key, no
	// button, no `Enum.UserInputType` — so an `InputObject` built here would have
	// to *invent* the field a handler reads it for. What closing this needs is
	// `gui::Router` recording which button produced an event, which is a change
	// in `gui` and not one here.
	//
	// @param state  The VM to deliver into.
	// @param events What the host collected since the last beat.
	// @return The first error a handler raised, or empty.
	std::string PumpGuiEvents(lua_State *state, std::span<const gui::GuiEvent> events);

	// Resumes every task due at the world's current tick.
	//
	// @return An error message when a resumed thread raised, or empty.
	std::string PumpTasks(lua_State *state);

	// Installs `workspace` — **this world's `Workspace` service**.
	//
	//     game      -> the universe
	//     workspace -> the `Workspace` instance in the world this script runs on
	//
	// **It is an instance, and until v0.7 it was not.** The old mapping made
	// `workspace` stand for the world itself, so `part.Parent = workspace` meant
	// `SetParent(part, NULL_ENTITY)` — "a root of this world" — and reading
	// `.Parent` back on a root handed `workspace` over. That was the honest
	// shape while a world had no `Workspace` in it. `scene::InstallServices`
	// changed that: a world now has a real `Workspace` instance, and keeping
	// both meant the engine held two answers to "what is in the scene" and the
	// renderer listened to neither.
	//
	// Collapsing them is what lets a null parent mean **nil**. An instance with
	// no parent is now an orphan rather than a root: nothing draws it, no walk
	// of the tree reaches it, and it becomes visible when a script says where it
	// goes. `scene/Visibility.hpp` is the other half — it is what makes
	// "under `Workspace`" the thing the renderer actually tests.
	//
	// Calls `InstallServices`, so a world that has none gets them here. That is
	// idempotent and finds before it creates.
	//
	// **Run before `OpenQueries`**, which fills in the Workspace's own method
	// table with `Raycast`.
	//
	// @param state The VM.
	// @param store The world this script runs on.
	void OpenWorkspace(lua_State *state, ecs::Store &store);

	// Calls every connected Heartbeat function with `delta`.
	//
	// @return An error message when one raised, or empty.
	std::string PumpHeartbeat(lua_State *state, float delta);

	// Pushes an instance userdata onto the stack.
	//
	// Used by `Runtime::RunInstance` to bind `script`, which is why it is not
	// file-local to `Instances.cpp`.
	//
	// @param state    The VM or one of its threads.
	// @param instance The instance to push.
	void PushInstanceValue(lua_State *state, ecs::Entity instance);

	// Reads an `Instance` argument, raising a type error when it is not one.
	//
	// **Shared rather than copied**, because the second copy is the one that
	// forgets the tag check and reads eight bytes of somebody else's userdata
	// as an entity handle. `Instances.cpp` owns the tag and this is how anything
	// else asks it.
	//
	// @param state The VM.
	// @param index The stack index.
	// @return The entity the instance names.
	ecs::Entity CheckInstanceArgument(lua_State *state, int index);

	// Installs `Instance`, and the metatable that turns `part.Size = v` into a
	// property write.
	//
	// @param state The VM.
	void OpenInstances(lua_State *state);

	// Adds every neutral instance method to the table `OpenInstances` built.
	//
	// **The Luau half of `ScriptCall.hpp`.** A method that is written once is
	// installed by both VMs from one table, so a row added in
	// `ScriptMethods.cpp` is reachable from Luau and from JavaScript in the same
	// commit — which is what the thirty-against-twenty-one drift cost when a
	// method was a `lua_CFunction` and a `JSCFunction` written separately.
	//
	// Called after `OpenInstances`, whose registry table this adds to.
	//
	// @param state The VM.
	// @since v0.16
	void InstallLuauNeutralMethods(lua_State *state);

	// The signals `InstanceIndex` answers from its branch chain.
	//
	// **Here because a branch cannot be walked.** Everything else the editor
	// offers is read back out of a live VM — the globals from its global table,
	// the instance methods from the registry table `OpenInstances` fills — and a
	// signal is the one member that exists only as a string comparison. See
	// `script::InstanceSignals`, which is the public face of this.
	//
	// @return The signal names, in the order the chain tests them.
	std::vector<std::string_view> LuauInstanceSignalNames();

	// Installs the world clock under names that do not lie about it.
	//
	// `time`, `elapsedTime`, `tick` and `DateTime`, all reading `store.Time()`.
	// **Never the wall clock**: a script that branched on real time would
	// produce a run that does not replay, and `just replay-check` would fail a
	// long way from the script that caused it.
	//
	// @param state The VM.
	void OpenClock(lua_State *state);

	// Installs `typeof` and `warn`, and replaces the base-library gaps with
	// refusals that say why.
	//
	// @param state The VM.
	void OpenBaseExtras(lua_State *state);
}
