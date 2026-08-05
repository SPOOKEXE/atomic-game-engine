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
#include <engine/ecs/Store.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/Runtime.hpp>

#include <lua.h>
#include <string>
#include <unordered_map>

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

		// The VM this context belongs to, so a `Connection` userdata can reach
		// the table without a second upvalue on every method.
		lua_State *State = nullptr;

		// Where execution should be reported from, owned by the `Runtime`.
		//
		// A pointer rather than a copy: breakpoints are edited from the editor
		// while the world runs, and a copy taken at construction would be a
		// second answer to what is armed.
		Debugger *Breakpoints = nullptr;

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

		// Which thread is suspended on which `world::Ticket`.
		//
		// **The first of `docs/SCRIPT_CONCURRENCY.md` §1's three legal resume
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
	// `docs/SCRIPT_CONCURRENCY.md` §1 permits. One that suspended any other way
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

	// Installs `game`, whose `GetService` is how a Roblox script reaches one.
	void OpenGame(lua_State *state);

	// Installs the Universe's services — the only route out of a world.
	//
	// `MessagingService`, `MemoryStoreService` and `DataStoreService`. The last
	// two want a reply, and a reply arrives at a later barrier, so a script
	// **yields** on one — which is legal under `docs/SCRIPT_CONCURRENCY.md` §1
	// precisely because the barrier applies replies in a deterministic order.
	void OpenServices(lua_State *state);

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
