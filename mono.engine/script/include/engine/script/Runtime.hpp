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
// `docs/SCRIPT_CONCURRENCY.md` §1 settles what a yield must mean — a script may
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

#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace engine::script {

	// Which VM runs a script.
	//
	// @since v0.5
	enum class Language : uint8_t {
		// Luau, from `mono.vendor/luau`.
		Luau,

		// JavaScript, from `mono.vendor/quickjs`. TypeScript is the typed
		// authoring surface over this one — it erases its types by design, so
		// there is nothing else for a "TypeScript VM" to have been.
		JavaScript,
	};

	// The language a file's extension names.
	//
	// `.luau` and `.lua` are Luau; `.js`, `.mjs` and `.ts` are JavaScript. A
	// `.ts` file is expected to have been type-stripped already — nothing in
	// the C++ build compiles TypeScript, and nothing should: the engine loads
	// what a toolchain emitted.
	//
	// @param path The file name or path.
	// @return The language, defaulting to Luau when the extension says nothing.
	Language LanguageOf(std::string_view path);

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

		// Which VM this is.
		//
		// @return The language.
		virtual Language Which() const = 0;

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

	  protected:
		Runtime(ecs::Store &store, const HostRole &role) : Store(store), HostRoleValue(role) {}

		ecs::Store &Store;
		HostRole HostRoleValue;
		std::string Error;
	};

	// Opens a VM of the given language over `store`.
	//
	// @param store    The world scripts create instances in. Outlives the result.
	// @param language Which VM.
	// @param limits   What bounds a script.
	// @return The runtime.
	std::unique_ptr<Runtime>
	MakeRuntime(ecs::Store &store, Language language, const RuntimeLimits &limits = {});

	// Opens the VM a file's extension names and runs it.
	//
	// @param store  The world to build into.
	// @param path   The script to run.
	// @param error  Filled in with the failure when this returns false.
	// @param limits What bounds the script.
	// @return `false` when the file could not be read, compiled or run.
	bool RunScriptFile(
		ecs::Store &store, const std::string &path, std::string &error, const RuntimeLimits &limits = {}
	);
}
