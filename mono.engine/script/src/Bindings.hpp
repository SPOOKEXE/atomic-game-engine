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
#include "Signals.hpp"
#include "Tasks.hpp"

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

	// Installs `RunService`, whose `Heartbeat` a script connects behaviour to,
	// and whose `IsServer`/`IsClient`/`IsStudio` say where a script is standing.
	void OpenRunService(lua_State *state);

	// Installs `UserInputService` and `ContextActionService`.
	//
	// **Both read `scene::InputState` and neither reads `engine::input`**, which
	// is the tier seam `Input.hpp` exists for: this module is `shared` and the SDL
	// pump is `client`.
	void OpenInputServices(lua_State *state);

	// Turns this frame's key edges into bound actions and input signals.
	//
	// Called at a barrier beside `PumpChanges`, for the same reason: a handler
	// that mutated the world would otherwise do it in the middle of a loop over
	// it.
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
	void OpenServices(lua_State *state);

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
	// Roblox passes an `InputObject`, this engine has no such datatype yet, and
	// a different shape invented here would have to change the day one arrives.
	// An argument nobody can rely on is worse than an argument that is not
	// there — the same trade `VideoFrame` and `AutomaticSize` were decided on.
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

	// Installs `Instance`, and the metatable that turns `part.Size = v` into a
	// property write.
	//
	// @param state The VM.
	void OpenInstances(lua_State *state);

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
