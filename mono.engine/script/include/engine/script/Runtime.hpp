#pragma once

// Running a script against a world, in either language.
//
// Scripts reach the world through the property surface `scene` declares and
// `ecs::Classes` holds — `Instance.new`, then properties by name — so this
// module adds a **calling convention and not a second mechanism**.
//
// **An instance is an entity. A class is a set of components. A property is a
// projection of one or more of them. Nothing else exists.** There is no
// instance object, no per-instance allocation and no scripting-only view of a
// row: what a script holds is an `ecs::Entity`, and every operation on it
// resolves against the same storage a C++ system iterates.
//
// **Two languages, two VMs, one binding surface.** Luau and JavaScript are
// independent choices rather than one transpiled into the other, and both come
// through this interface. What is shared is everything that matters — one class
// table, one property surface, one marshalling rule (switch on `PropertyType`,
// never on a name). What differs is a file each.
//
// **What a script may not do is the design, not a hardening pass.** Neither VM
// gets a wall clock, both are bounded in memory and in steps, and both freeze
// what a script could otherwise rewrite. A game loads scripts it did not write.
//
// **A script may yield, and only from something that will resume it.**
// `docs/retired/SCRIPT_CONCURRENCY.md` §1 settles what a yield must mean — a script may
// only resume from something the barrier delivers in a deterministic order —
// and v0.6 built the three sources it names: a `Ticket` reply, a `Deliveries()`
// entry, and a tick boundary through `task`. So a suspended thread with a
// scheduled resume is legal, and one without is refused. That is a lookup rather
// than a judgement, which is what makes it enforceable.
//
// Luau suspends with a coroutine and JavaScript with a promise. Both resume at a
// point the *host* chose, which is the property that matters.
//
// @tier L9 · shared

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Input.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/Language.hpp>
#include <engine/script/Vocabulary.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// Where a script is standing.
	//
	// **`RunService:IsServer()` is a question about the host, not about the
	// world**, and the two are genuinely different: `mono.unified_server_client`
	// hosts a server in the same process as a client, so a world can be
	// authoritative while the process it runs in also draws. A script asking
	// which it is on is asking about the program, so the program is what says.
	//
	// **Both true is a legal answer and not a bug.** A single-player host is a
	// server and a client, and a script written for one that refuses to run
	// because the other is also true would be wrong exactly where it matters.
	//
	// @since v0.6
	struct HostRole {
		// Whether this host simulates authoritatively.
		bool Server = true;

		// Whether this host presents.
		bool Client = false;

		// Whether this is the editor.
		//
		// **Defaults to false, deliberately.** v0.6's roadmap line says so in
		// as many words, and the reason is that a script guarding editor-only
		// behaviour behind `IsStudio()` must not have that behaviour appear in
		// a shipped game because a default was optimistic. `mono.studio` is
		// v0.7, and it is the only thing that will ever set this.
		bool Studio = false;

		// The role a headless authoritative host has.
		static constexpr HostRole OfServer() {
			return HostRole{true, false, false};
		}

		// The role a presenting host connected to a server has.
		static constexpr HostRole OfClient() {
			return HostRole{false, true, false};
		}

		// The role a host that is both has — single-player, and
		// `mono.unified_server_client`.
		static constexpr HostRole OfBoth() {
			return HostRole{true, true, false};
		}
	};

	// What bounds a script, and what a host may change about it.
	//
	// Both limits are refusals rather than throttles: past either one the script
	// stops with an error a caller can report. A budget that slowed a script
	// down instead would turn a bug into a frame-rate mystery.
	//
	// @since v0.5
	struct RuntimeLimits {
		// The most memory one VM may hold, in bytes.
		//
		// Allocation past this fails inside the VM, which surfaces as an
		// ordinary script error rather than as a bad_alloc in the host.
		size_t MemoryBytes = 64u * 1024u * 1024u;

		// How many interpreter safepoints one call may pass before it is cut
		// off. Loop back-edges and calls are safepoints, so this bounds
		// `while true do end` without bounding ordinary work.
		//
		// Zero disables the check, which is for a host that has some other way
		// to bound a script and knows it.
		uint64_t StepBudget = 200u * 1000u * 1000u;

		// Where scripts running under this runtime are standing.
		HostRole Role;
	};

	// What one script cost the last time it ran.
	//
	// @since v0.10
	struct ScriptCost {
		// Which script. A handle, so the panel can name it from the store
		// rather than holding a copy of a name that a rename would stale.
		ecs::Entity Instance;

		// VM steps spent inside its top level.
		uint64_t Steps = 0;

		// Whether it ran without raising. A script that failed half way through
		// has a step count and it is not comparable to one that finished.
		bool Completed = true;
	};

	// One VM, bound to one world.
	//
	// @since v0.5
	class Runtime {
	  public:
		virtual ~Runtime() = default;

		Runtime(const Runtime &) = delete;
		Runtime &operator=(const Runtime &) = delete;
		Runtime(Runtime &&) = delete;
		Runtime &operator=(Runtime &&) = delete;

		// Compiles and runs a chunk of source.
		//
		// @param source The script text.
		// @param name   What errors call it — a path, usually.
		// @return `true` when it compiled and ran without error.
		virtual bool Run(std::string_view source, std::string_view name = "script") = 0;

		// Reads a file and runs it.
		//
		// @param path The file to load.
		// @return `true` when it was read, compiled and ran without error.
		bool RunFile(const std::string &path);

		// Runs one script **instance**, with `script` naming it.
		//
		// **The difference from `RunFile` is the global.** A chunk run this way
		// knows which instance it is, so it can reach its own parent, its own
		// siblings and its own properties — which is what makes a game of many
		// scripts possible rather than one file that happens to build a world.
		//
		// The program is read from `script::Source::Path`, relative to the
		// assets root. A script with no path is a no-op rather than an error: an
		// author makes the instance before choosing the file, and that is a
		// legal state rather than a mistake.
		//
		// @param instance The script instance.
		// @return `false` when the file could not be read, compiled or run.
		virtual bool RunInstance(ecs::Entity instance) = 0;

		// Runs every script instance in the world this host should run.
		//
		// **This is what makes a world own its scripts.** `--script PATH` runs
		// one file; a game has many, each parented somewhere, and this is the
		// call that starts them — in creation order, skipping disabled ones, and
		// filtered by `RuntimeLimits::Role` so a `LocalScript` does not run on a
		// dedicated server.
		//
		// Every script runs even when one fails, for the reason every heartbeat
		// connection does: a game where half the scripts silently did not start
		// is a bug report with nothing in it. The first failure is in
		// `LastError`.
		//
		// @return How many ran without error.
		size_t RunWorldScripts();

		// Runs the scripts in `wanted` this runtime has not already run.
		//
		// **What a world that gains scripts after it started needs, and a
		// replica is the world that does.** `RunWorldScripts` is a host saying
		// "start this game": it is called once, over a world that is already
		// built, and calling it twice starts every script twice. A client's
		// replica is empty when its VM opens and fills from the wire afterwards,
		// so the question there is not "what does this world hold" but "what has
		// arrived since last time".
		//
		// **The selection is the caller's**, because a replica's is not a host's:
		// `ClientScriptsIn` adds Roblox's container rule to the class rule
		// `RunWorldScripts` applies through `RuntimeLimits::Role`, and a runtime
		// that decided for itself would have to know it was in a replica.
		//
		// Every script runs even when one fails, and the first failure is in
		// `LastError` — `RunWorldScripts`' reason exactly.
		//
		// @param wanted The instances to consider, in the order to run them.
		// @return How many started on this call.
		// @since v0.15
		size_t RunNewScripts(std::span<const ecs::Entity> wanted);

		// Calls everything connected to `RunService.Heartbeat`.
		//
		// **This is what makes a script the simulation rather than a setup
		// step.** A Roblox author writes behaviour by connecting to a signal
		// and moving instances on each beat; without it, the only thing a
		// script can do is describe a world and hand it to a C++ system to
		// animate — which is a scene format, not a scripting layer.
		//
		// `delta` is the world's **fixed tick delta**, never wall time. A
		// script that integrated against a real clock would put the scene in a
		// different place on a busy machine, and the recording would stop
		// replaying — the desync rule 5 names, arriving through the one call a
		// script uses most.
		//
		// @param delta Seconds of simulated time since the last beat.
		// @return `false` when a connected function raised, with `LastError`
		//         filled in. Remaining connections still run.
		virtual bool Heartbeat(float delta) = 0;

		// Hands this VM what a pointer did to the 2D tree.
		//
		// **The one way gui input reaches a script, and the reason it is here
		// rather than in whoever owns the pointer.** `gui` is L7 and produces
		// events; this module is L9 and owns every path into a VM. A host that
		// turned a `gui::GuiEvent` into a call itself would be the second such
		// path, and the two would disagree about ordering the first time one was
		// fixed — which is the same argument `Signals.hpp` makes about two
		// hand-written copies of the connection rules.
		//
		// **Queued, not dispatched.** The events are held until the next
		// `Heartbeat`, where they are delivered alongside the tree signals and
		// the property changes. Firing them on arrival would call a script from
		// inside the caller's walk of its own compiled draw list — and a handler
		// that destroys the element it was called about would then invalidate
		// the list the caller is still reading. The barrier is where every other
		// resume happens and this is not worth making an exception of.
		//
		// **A copy, because the router's span is not the caller's to keep.**
		// `Router::Update` returns a view into a vector it reuses every frame,
		// so anything held past the call has to own its bytes.
		//
		// Safe to call with an empty span, which is most frames.
		//
		// @param events What the router produced this frame.
		void DeliverGuiEvents(std::span<const gui::GuiEvent> events);

		// How many gui events are waiting for the next beat.
		//
		// For a test and for a panel. A number that only grows is a host
		// delivering events to a runtime whose `Heartbeat` nothing calls, which
		// is a wiring mistake that otherwise reads as "the buttons do nothing".
		size_t PendingGuiEventCount() const {
			return PendingGuiEvents.size();
		}

		// Which VM this is.
		//
		// @return The language.
		virtual Language Which() const = 0;

		// What a script running here may name.
		//
		// **Asked of the runtime rather than kept in a table**, because there is
		// no table: a global is one of about fifty `lua_setglobal` and
		// `JS_SetPropertyStr` calls spread across fifteen files, and a list of
		// them written anywhere else is a copy that goes stale silently. The
		// editor's completion is built from this, so a global added anywhere in
		// this module is offered without a second edit. `Vocabulary.hpp` carries
		// the argument and the two bugs that paid for it.
		//
		// **Call it on a runtime that has not run anything.** JavaScript's
		// chunks share one global object — `JavaScriptRuntime` says why — so a
		// script that had assigned a global would appear in the answer as though
		// the engine installed it.
		//
		// The default is empty rather than pure virtual: a runtime that cannot
		// describe itself is a completion list with no engine names in it, which
		// is a degraded editor and not a broken one.
		//
		// @return The globals and the instance members.
		// @since v0.14
		virtual ScriptSurface Surface() const {
			return {};
		}

		// The error from the last `Run` or `RunFile` that returned false.
		//
		// @return The message, or empty when nothing has failed.
		const std::string &LastError() const {
			return Error;
		}

		// Which host this runtime's scripts believe they are on.
		//
		// @return The role given at construction.
		const HostRole &Role() const {
			return HostRoleValue;
		}

		// The world this runtime builds into.
		//
		// @return The store passed at construction.
		ecs::Store &World() const {
			return Store;
		}

		// --- the host seam ---------------------------------------------------
		//
		// What a *program* offers this script beyond the world. See `Host.hpp`
		// for why it is one method over a value tree rather than a growing
		// interface, and for the two things a host call can carry that a bus
		// message cannot.

		// Installs the host, replacing any previous one.
		//
		// **Before the first `Run`, because the global is built from
		// `HostSurface::Names`.** A host installed afterwards is a `host` table
		// a chunk already captured, and `luaL_sandbox` makes that capture
		// permanent — see `D00030`, which is the same mechanism biting a
		// different surface.
		//
		// The surface must outlive this runtime. A null one removes the global.
		//
		// @param host What answers a script's calls, or null.
		// @since v0.12
		virtual void SetHost(HostSurface *host) {
			(void)host;
		}

		// Calls a function a script handed the host.
		//
		// **The other direction of the seam.** A button's handler lives in the
		// script's VM and the press happens in the host's frame, so this is what
		// joins the two — and it is a method here rather than a callable the
		// host holds, because the reference behind a `HostCallback` is a VM
		// concept that does not leave this module.
		//
		// Runs the handler immediately rather than at a barrier, which is the
		// one place this differs from every signal in the engine and is
		// deliberate: a host calls this from its own frame in response to a
		// person, not from inside a tick over a world somebody is iterating.
		//
		// @param callback  What to call.
		// @param arguments What to pass, in order.
		// @return `false` when the handler raised or the callback is unknown;
		//         `LastError` says which.
		// @since v0.12
		virtual bool Invoke(HostCallback callback, HostArguments arguments) {
			(void)callback;
			(void)arguments;
			return false;
		}

		// Lets go of a function a script handed the host.
		//
		// **Called when whatever held it goes away** — a button removed, a panel
		// closed. A host that never releases holds a closure for the life of the
		// runtime, which is survivable and still wrong for a panel somebody
		// opens and closes a hundred times.
		//
		// @param callback What to release. An invalid one is ignored.
		// @since v0.12
		virtual void Release(HostCallback callback) {
			(void)callback;
		}

		// How many VM steps this runtime has spent since it was made.
		//
		// **Steps rather than seconds, for the reason the budget is counted in
		// them**: a wall-clock figure makes what a script cost depend on how
		// busy the machine was, and two runs of one recording would then
		// disagree about it. A step is the same on every machine.
		//
		// Zero from a VM with no equivalent counter, which is honest rather than
		// approximate — a fabricated number here would be compared against a
		// real one in the same table.
		//
		// @return The cumulative step count.
		virtual uint64_t StepsTaken() const {
			return 0;
		}

		// What each script cost when it last ran, most expensive first.
		//
		// **Filled by `RunWorldScripts` and by nothing else**, so it measures a
		// script's own top level rather than the heartbeat work it went on to
		// connect. That distinction is worth keeping rather than blurring: the
		// connections are held by `SignalTable` as opaque callables and nothing
		// records which script made one, so attributing beat time to a script
		// would be a guess wearing a number's clothes.
		//
		// @return One entry per script that ran, valid until the next run.
		std::span<const ScriptCost> Costs() const {
			return ScriptCosts;
		}

		// This runtime's breakpoints, and what they caught.
		//
		// **One per runtime rather than a global**, for the reason the store is:
		// two runtimes over two worlds must not share them, and a file-static
		// would have made that mistake available.
		//
		// A VM that cannot honour a breakpoint still hands one back — the
		// breakpoints are then a list nothing consults, which is visible in the
		// panel rather than silently ignored.
		//
		// @return The debugger.
		Debugger &Debug() {
			return Breakpoints;
		}

		// This runtime's breakpoints, for a reader.
		//
		// @return The debugger.
		const Debugger &Debug() const {
			return Breakpoints;
		}

	  protected:
		// Binds a runtime to the world it builds into and the role it believes
		// it is on.
		//
		// @param store The world. Outlives the runtime.
		// @param role  Where scripts under this runtime are standing.
		Runtime(ecs::Store &store, const HostRole &role) : Store(store), HostRoleValue(role) {}

		// The world this runtime builds into. A reference rather than a handle,
		// because a VM is created for one world and dies with it.
		ecs::Store &Store;

		// Where scripts under this runtime believe they are standing.
		HostRole HostRoleValue;

		// The last failure, or empty. Read through `LastError`.
		std::string Error;

		// What each script cost, rebuilt by every `RunWorldScripts`. Read
		// through `Costs`.
		std::vector<ScriptCost> ScriptCosts;

		// Gui events waiting for the next beat, in the order the router
		// produced them.
		//
		// **In the base rather than in either VM's context, because it holds
		// nothing either VM owns** — a kind, an entity and two points. Both
		// runtimes drain it from their own `Heartbeat`, which is the same split
		// `SignalTable` uses: the ordering lives once, the calling lives twice.
		//
		// Order is the router's and is never sorted. `MouseLeave` before
		// `MouseEnter` is a rule `gui::Router` already decided, and re-deriving
		// it here would be a second answer to it.
		std::vector<gui::GuiEvent> PendingGuiEvents;

		// Where execution should be reported from. Read through `Debug`.
		Debugger Breakpoints;

	  private:
		// Records `instance` as started here, answering whether it is new.
		//
		// @param instance The script instance.
		// @return `true` the first time this runtime is asked about it.
		bool RememberStarted(ecs::Entity instance);

		// Which script instances this runtime has already run, by entity id.
		//
		// **The VM's own record and not a fact about the world**, which is why
		// it is here rather than a tag on the row: "has this program been given
		// to this interpreter" is a property of the interpreter, and two VMs over
		// one world would each have to answer it separately. Rule 2 asks that two
		// modules not keep two copies of one fact; nothing else keeps this one.
		//
		// Sorted by id, so a lookup is a binary search and the order never
		// depends on a hash.
		std::vector<ecs::Entity> StartedScripts;
	};

	// Opens a VM of the given language over `store`.
	//
	// @param store    The world scripts create instances in. Outlives the result.
	// @param language Which VM.
	// @param limits   What bounds a script.
	// @return The runtime.
	std::unique_ptr<Runtime>
	MakeRuntime(ecs::Store &store, Language language, const RuntimeLimits &limits = {});

	// Takes in everybody a teleport has sent to this world.
	//
	// **A world that is not running scripts still has to accept people.** This
	// used to happen inside the Luau runtime's own delivery pump, which meant a
	// teleport was only ever admitted by a world with a *Luau* script actually
	// executing — so a destination the studio was not playing, a world whose
	// scripts are JavaScript, or a scene furnished entirely by C++ took the
	// payload into its inbox and left it there. The player was destroyed in the
	// world they left, never built in the world they went to, and the host that
	// followed them searched every world and found nobody.
	//
	// That is what an immersive cross-world portal looks like when it goes
	// wrong: the far room draws, live, through the pane — because a picture is a
	// draw list and needs no runtime — and walking into it deletes you.
	//
	// **Here rather than in `world`, because admitting is a `scene` act.** What
	// arrives is a name and a payload; what is built is a `Player`, a character
	// and a `StringValue` of teleport data, all from *this* world's own class
	// definitions. `world` may not name any of those, and that is the rule that
	// keeps two worlds from having to agree on class versions.
	//
	// **Idempotent within a tick and safe to call beside a runtime.** It reads
	// the same delivery list the pump reads and admits only `BusKind::Teleport`;
	// the pump no longer touches those, so there is exactly one admitter however
	// many runtimes a world has.
	//
	// @param store The destination world.
	// @return How many players were admitted. Zero on nearly every tick.
	// @since v0.15
	size_t AdmitTeleports(ecs::Store &store);

	// Installs `AdmitTeleports` as a system on a world.
	//
	// **`PreSimulation`, so somebody who arrived this tick is simulated this
	// tick** rather than standing still for one — which at sixty ticks is
	// invisible and at a low tick rate is a body that appears already falling.
	//
	// **Every world, whether or not it runs scripts.** A destination is chosen
	// by a script in *another* world, so a world can be a destination without
	// containing a single line of code — which is exactly the case that was
	// broken.
	//
	// @param scheduler The world's scheduler.
	// @since v0.15
	void RegisterTeleportAdmission(ecs::Scheduler &scheduler);
}
