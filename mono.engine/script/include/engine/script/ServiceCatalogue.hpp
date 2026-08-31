#pragma once

// What services this engine has, said once, for every language that binds them.
//
// **The layer between "what a service is" and "how a VM builds one".** Each side
// carried its own list once, and the two drifted exactly as two lists do: Luau
// installed nine surface services and JavaScript five, `ContentService`,
// `UserInputService`, `ContextActionService` and `BreakpointService` were
// reachable from one language and not the other, nothing in the build said so,
// and the TypeScript declarations claimed two of them anyway.
//
// **This header answers *which services exist*; `ServiceSurface.hpp` answers
// *what is on one*.** The second was VM-shaped until v0.16 and cost the same
// thing one level down - a service described in `lua_CFunction`s can only be
// built by Luau - which is why `ContentService`, `CollectionService`,
// `HttpService`, `CrossWorldService` and `ContextActionService` stayed
// Luau-only after the catalogue had already named the gap. `UserInputService`
// and `SoundService` outlasted those five by one mechanism, a live *property*,
// and crossed when `ServiceProperty` gave one a neutral shape.
//
// **Every surface service is now in both languages, and the one row that is not
// is not a binding gap.** `BreakpointService` arms `lua_callbacks`' `debugstep`
// and switches Luau into single-step mode; `Debugger::Add` refuses a `.js`,
// `.mjs`, `.cjs`, `.ts` or `.tsx` chunk outright, so the JavaScript half would
// answer "nothing can be armed" to everything. A JavaScript debugger is a
// feature and not a binding - see `DEFERRED.md` D00106.
//
// **A catalogue rather than self-registration, and the difference is a linker
// one.** The obvious shape is a static registrar per service file, each adding
// itself to a vector at load. It does not survive this build: modules here are
// static libraries, and an object file whose only exported symbol is a registrar
// nobody references is one the archive is free to drop - so the service compiles,
// links, and is absent at runtime, typically in the release build and typically
// months later. `scene::RegisterSceneComponents` and `RegisterSceneClasses` are
// explicit for the same reason.
//
// `ServiceCatalogue.cpp` is the one file that names every installer, which is
// what makes the linker keep them, and it is the only place a service is
// declared. Everything else - both adapters and the binding generator - reads
// it.
//
// **It has met neither VM since v0.19.** The table below hands back rows of
// data, and each adapter module walks them in its own currency - `scriptluau`
// builds a Luau table from a row and `scriptjs` a JavaScript object. That is
// what let the VM boundary become a module boundary: this module links no VM at
// all, so the tier and layer checks enforce what a filename convention used to.
//
// **A service missing from a language is a stated fact and not an absence.** A
// runtime asked for a service its language does not bind refuses by *name*,
// saying which language has it, rather than failing the way an invented service
// fails. A script author who gets "there is no such service" from something the
// documentation lists goes looking in the wrong place.
//
// @tier L9 · shared
// @since v0.15

#include <engine/script/Runtime.hpp>

#include <cstdint>
#include <span>

namespace engine::script {

	// What is on one service. Declared rather than included: every use here is a
	// reference behind a function pointer, so this header stays the small one.
	struct ServiceSurface;

	// Which languages bind a service.
	//
	// A mask rather than a language enum, because the answer is a *set*: most
	// services are in both, some in one, and "which" is what a refusal has to
	// name. `Language` says which VM is running; this says which VMs can.
	enum class ServiceLanguages : uint8_t {
		None = 0,
		Luau = 1u << 0,
		JavaScript = 1u << 1,
		Both = Luau | JavaScript,
	};

	// Joins two masks, so a set can be spelled from its parts at a call site.
	//
	// The cast a scoped enum costs is written once, here, rather than at every
	// place that wants to say "these two languages".
	constexpr ServiceLanguages operator|(ServiceLanguages left, ServiceLanguages right) {
		return static_cast<ServiceLanguages>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
	}

	// Whether `set` contains `one`. What a refusal consults: a service the
	// catalogue lists but this language does not bind is answered by name, not
	// as an absence.
	//
	// @param set What a definition declares.
	// @param one The language asking.
	// @return Whether that language has a real binding.
	constexpr bool Binds(ServiceLanguages set, ServiceLanguages one) {
		return (static_cast<uint8_t>(set) & static_cast<uint8_t>(one)) != 0;
	}

	// Whether a service exists in a given process at all.
	//
	// **Separate from `ServiceLanguages`, because they are different questions
	// and were the same field for about ten minutes.** A language mask says
	// which VM *can* bind a service; this says which processes *have* one.
	// `BreakpointService` is bound by Luau and exists only in a studio, and
	// folding the two would make "absent because you are not in a studio"
	// indistinguishable from "absent because you are writing JavaScript".
	enum class ServiceAvailability : uint8_t {
		// Every host. The ordinary case.
		Always,

		// A studio only. The installer still decides - `OpenBreakpointService`
		// checks the debugger pointer as well - so this is what the *catalogue*
		// knows, which is what a generator and a refusal message need. It is not
		// a second gate on the install.
		Studio,
	};

	// One service, independent of any VM.
	//
	// **No installer here, and that is what keeps this header clean.** A
	// function pointer taking a `lua_State *` would pull Luau into every
	// JavaScript translation unit and a `JSContext *` would do the reverse -
	// which is precisely the coupling the module split refuses. Nothing in this
	// module has met either VM.
	struct ServiceDefinition {
		// What a script calls it, and what `game:GetService` resolves.
		const char *Name;

		// Which hosts have one at all.
		//
		// The row tables are already split by phase, so this is the fact stated
		// on the row rather than a second gate on the install - what a refusal
		// and the binding generator need to tell "absent because you are not in
		// a studio" from "no such service".
		ServiceAvailability Availability;

		// Which languages have a real binding for it.
		//
		// **A language without one is a refusal, not a silence.** See the header
		// comment: the runtime says which language does have it.
		ServiceLanguages Languages;

		// The grants a runtime needs before either VM installs this service.
		// Core deterministic services use `World`; host-sensitive services name
		// their narrower capability on the catalogue row.
		ScriptCapabilities RequiredCapabilities = ScriptCapabilities::World;
	};

	// Reports whether a grant set permits installation of one service.
	constexpr bool Permits(const ServiceDefinition &service, ScriptCapabilities granted) {
		return HasCapabilities(granted, service.RequiredCapabilities);
	}

	// Every service this engine declares, in install order.
	//
	// **Install order and not alphabetical**, because it is the order the
	// runtimes walk and some of it is load-bearing: `game` has to exist before
	// anything resolves through it, and `BreakpointService` has to be installed
	// after the debugger pointer is set and before the sandbox freezes the
	// globals. Sorting this would be sorting the tick.
	//
	// @return The table, valid for the life of the program.
	std::span<const ServiceDefinition> Services();

	// One service by name, or null.
	//
	// What a refusal consults: a name that is here but not bound by this
	// language gets a sentence naming the language that binds it, and a name
	// that is not here at all gets "no such service".
	//
	// @param name What the script asked for.
	// @return The definition, or null.
	const ServiceDefinition *FindService(const char *name);

	// One catalogue row: what the service is, and the one description both
	// languages build it from.
	//
	// **A `ServiceSurface` accessor rather than an installer, and that is the
	// whole of the seam.** An installer can only build the VM it was written
	// against; a surface is data, so each adapter walks this list once and
	// builds its own object from the same description. A service added here is a
	// service in both languages in the same commit.
	//
	// **`Surface` is null for a row no neutral description can express.** One
	// row is: `BreakpointService` arms Luau's `debugstep` callback, which is not
	// a method, a property or a signal. `Languages` says who binds it, and the
	// Luau adapter is the only walker that acts on such a row.
	//
	// @since v0.19
	struct ServiceRow {
		// What the service is. Held by value and not by pointer, because the
		// rows *are* the declaration: `Services()` is built by flattening these,
		// so a service is written down exactly once.
		ServiceDefinition Definition;

		// The description both languages build from, or null.
		//
		// Returns a table with static storage duration: a property-bearing
		// service puts the surface's *address* on its metamethods rather than
		// copying the lists, so one built as a local would read freed memory on
		// the first property access.
		const ServiceSurface &(*Surface)();
	};

	// The rows of one install phase, in install order.
	//
	// **Install order and not alphabetical**, for `Services()`' reason: `game`
	// has to exist before anything resolves through it, and the studio phase
	// installs after a runtime's debugger pointer is set and before the sandbox
	// freezes the globals. Sorting this would be sorting the tick.
	//
	// @param phase Which set.
	// @return The rows, valid for the life of the program.
	// @since v0.19
	std::span<const ServiceRow> ServiceRows(ServiceAvailability phase);

	// --- the services described once, for both languages ----------------------
	//
	// **A surface rather than an installer, which is the whole of what made
	// these five reachable from JavaScript.** An `Open*` function can only build
	// the VM it was written against; a `ServiceSurface` is data, so
	// `ServiceCatalogue.cpp` - the one file that has met both VMs - reads it
	// twice and builds a Luau table and a JavaScript object from one
	// description.
	//
	// Each returns a table with static storage duration, valid for the life of
	// the program.
	//@{

	// `ContextActionService`, whose priority stack is what it adds over polling.
	//
	// **All six methods are neutral**, and the last two to get there are the ones
	// worth naming: `GetBoundActionInfo` and `GetAllBoundActionInfo` answer a
	// record holding a list of `Enum.KeyCode` members, and an `EnumItem` has no
	// neutral return - `ScriptValue` has no tag for one, and inventing a record
	// return for one service's shape is what `ScriptCall.hpp` says the interface
	// is not for. `ScriptCall::ReturnBoundAction` over a `BoundActionReport` is
	// what closed it, which is the split `ReturnInputObjects` was already on.
	//
	// **The stack is a walk and not a lookup**, because a handler answers
	// `Enum.ContextActionResult` - see `ActionStack::ClaimingFrom`.
	const ServiceSurface &ContextActionServiceSurface();

	// `ContentService`, which answers what content this world holds.
	//
	// **The other half of rule 4.** A script names an asset and had no way to
	// ask what the names were, so every demo carried string literals for files
	// that only existed if somebody had baked that exact tree. See
	// `ContentService.cpp`.
	//
	// @since v0.10
	const ServiceSurface &ContentServiceSurface();

	// `ComputeService`, the bounded asynchronous numeric batch surface.
	//
	// Work crosses a heartbeat only through a typed noise-grid request and an
	// owned numeric result. Arbitrary script functions do not leave their VM.
	//
	// @since v0.20
	const ServiceSurface &ComputeServiceSurface();

	// `CollectionService`, which answers what carries a tag.
	//
	// **The other side of `Instance:AddTag`.** The same three methods, plus the
	// one neither the instance surface nor anything else in the engine could
	// answer from a script: `GetTagged`. A scene that wanted every door had to
	// keep its own list beside the tags, which is rule 2's second copy.
	//
	// No `GetInstanceAddedSignal`, and `CollectionService.cpp`'s header says
	// what firing one honestly would take.
	//
	// @since v0.15
	const ServiceSurface &CollectionServiceSurface();

	// `HttpService` - **the half of it that observes nothing**.
	//
	// `JSONEncode`, `JSONDecode`, `GenerateGUID` and `UrlEncode`, and no
	// `RequestAsync`, `GetAsync` or `PostAsync`. Arbitrary outbound HTTP from a
	// game script is a security decision nobody has taken, and this engine's one
	// existing route to the network is a signed manifest verified against a
	// publisher key - a different thing, not a smaller one. `HttpService.cpp`
	// carries the argument and the note asking the next reader not to add the
	// three by reflex.
	//
	// @since v0.15
	const ServiceSurface &HttpServiceSurface();

	// `CrossWorldService`, the addressed route out of a world.
	//
	// **`MessagingService` is a fan-out and this is a channel**, which is the
	// whole distinction: a topic has no destination, so a game saying one thing
	// to one world had to broadcast it to everybody or send a player carrying
	// it. `world::BusKind::Channel` is the kind, appended beside `Teleport`
	// because a channel is a teleport with nobody attached.
	//
	// **The address is `(world, channel)` and both halves matter.** A receiver
	// opens a named channel and gets that channel's signal back; a sender names
	// both and suspends on the answer. See the file for why the catch-all signal
	// it replaced could not be kept beside it.
	//
	// @since v0.15
	const ServiceSurface &CrossWorldServiceSurface();

	// `UserInputService`, over `scene::InputState`.
	//
	// **Reads `scene::InputState` and never `engine::input`**, which is the tier
	// seam `Input.hpp` exists for: this module is `shared` and the SDL pump is
	// `client`.
	//
	// **The last service to cross, and a property is what held it.**
	// `MouseBehavior` and `MouseDeltaSensitivity` are live values, so the Luau
	// half is a *userdata* that defeats `safeenv`'s `GETIMPORT` caching where the
	// JavaScript half is a plain object with an accessor per name - see
	// `ServiceSurface::Properties`. Both build from this one description.
	//
	// @since v0.16
	const ServiceSurface &UserInputServiceSurface();

	// `SoundService` - **the part of it that is not the mixer**.
	//
	// A `Volume` and a listener, over `scene::AudioState`. `engine::audio` is L12
	// `client` and this module is L9 `shared`, so nothing here can name a mixer:
	// the seam is a resource on the world that `client::SoundStage` reads, which
	// is the arrangement `scene::InputState` established. `SoundService.cpp`
	// lists the eleven Roblox members that are absent and what each would need
	// first.
	//
	// @since v0.16
	const ServiceSurface &SoundServiceSurface();

	// `Debris`, whose one method destroys an instance later.
	//
	// **The queue is per VM and everything about it is shared** - the tick
	// arithmetic, the drain order and the cap that evicts rather than refusing.
	// See `Debris.hpp`; `PumpDebris` is the other half.
	//
	// @since v0.16
	const ServiceSurface &DebrisServiceSurface();

	// `TweenService`: `GetValue`, and `Create`.
	//
	// **`GetValue` is the whole easing surface a script can reach without
	// building anything**, and `Create` is the rest: a target, a `TweenInfo` and
	// a map from property name to where it should end up. See `Tweens.hpp` for
	// what a tween is, why it is an entity, and why its handle is a userdata of
	// its own rather than an ordinary instance.
	//
	// **The `Tween` handle's three methods stay per language on purpose.** The
	// neutral instance methods are installed flat on *every* instance, and `Play`
	// is a name Roblox puts on three classes - claiming it there would take it
	// from every part and sound in the engine. `ScriptCall::ReturnTween` is where
	// the two halves meet.
	//
	// @since v0.16
	const ServiceSurface &TweenServiceSurface();

	// `RunService`: `Heartbeat`, and the four questions about this host.
	//
	// **`IsReplica` is not Roblox's and is the more precise question**:
	// `IsServer` is about the process and this is about the *world*, so a
	// single-player host answers true to both from its client-side world. See
	// `RunService.cpp`.
	//
	// @since v0.16
	const ServiceSurface &RunServiceSurface();

	// The Universe's services - the only route out of a world.
	//
	// `MessagingService` fans out to a topic, `TeleportService` moves a person to
	// a named world, and the two stores answer a key. The last two want a reply,
	// and a reply arrives at a later barrier, so a script **suspends** on one -
	// which is legal under `docs/retired/SCRIPT_CONCURRENCY.md` §1 precisely
	// because the barrier applies replies in a deterministic order, and which is
	// what `ScriptCall::Await` is for.
	//
	// @since v0.16
	//@{
	const ServiceSurface &MessagingServiceSurface();
	const ServiceSurface &TeleportServiceSurface();
	const ServiceSurface &MemoryStoreServiceSurface();
	const ServiceSurface &DataStoreServiceSurface();
	//@}
	//@}
}
