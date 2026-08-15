// `RunService`, in neither language.
//
// **The four predicates a script opens with, and they are a description rather
// than a binding.** Each reads `HostRole` or the world, so each is a
// `ScriptMethod` and both VMs install the same rows.
//
// **The `game` locator moved to `LuauGame.cpp` at v0.18.** It lived here on the
// argument that "how a script reaches a service" is one subject, which is true
// and was costing this description a dependency on `<lua.h>`: `game` is a global
// with a metatable in one language and an object in the other, which is
// apparatus rather than method. The seam between the two is that a service
// installs as a global of its own name, and `GetService` looks one up.
//
// @tier L9 · shared

#include "ScriptCall.hpp"
#include "ServiceSurface.hpp"

#include <array>

namespace engine::script {

	namespace {
		// `RunService:IsServer()` / `IsClient()` / `IsStudio()`
		//
		// **A script needs to be able to ask before it tries.**
		// `Store::SetProperty` already refuses a write on an adopt-only store
		// and says why, but a refusal is an error a script has to catch - and
		// the whole point of a client-side script is that it knows it is one.
		// These are the question that makes the refusal avoidable.
		void IsServer(ScriptCall &call) {
			call.ReturnBoolean(call.Role().Server);
		}

		void IsClient(ScriptCall &call) {
			call.ReturnBoolean(call.Role().Client);
		}

		void IsStudio(ScriptCall &call) {
			call.ReturnBoolean(call.Role().Studio);
		}

		// `RunService:IsReplica()` - whether this world's rows belong to somebody
		// else.
		//
		// **Not Roblox's, and it is the more precise question.** `IsServer()`
		// is about the *host*; this is about the *world*, and a single-player
		// process is a server whose client-side world is still a replica. A
		// script that guarded a write with `IsServer()` alone would be right on
		// a dedicated server and wrong in single player, which is the worst
		// place for a guard to be wrong.
		void IsReplica(ScriptCall &call) {
			call.ReturnBoolean(call.World().AdoptOnly());
		}

		// **No `IsRunning` and no `IsEdit`, which Roblox has.** Both answer
		// whether the *editor* is simulating, and this engine has no such state
		// to read: a runtime exists because something is running it, so
		// `IsRunning` would be `true` wherever a script could ask and `IsEdit`
		// would be `false`. Two members that answer a constant look decided,
		// which is the surface `HttpService`'s absent three are refused for
		// being. `mono.studio` growing a paused mode is what would make them
		// mean something.
		constexpr std::array<ServiceMethod, 4> METHODS{{
			{"IsServer", IsServer},
			{"IsClient", IsClient},
			{"IsStudio", IsStudio},
			{"IsReplica", IsReplica},
		}};

		// `Heartbeat` is a real signal rather than a list of its own, so
		// `:Connect` hands back an `RBXScriptConnection` a script can
		// `:Disconnect` - which is the thing v0.5 said was worse to fake than to
		// omit. It is a *field* rather than a method, which is why
		// `ServiceSurface` carries two lists.
		//
		// **No `RenderStepped`, `PreSimulation` or `PostSimulation`.** Each is a
		// different point in a frame, this engine's barrier has one, and a signal
		// that exists and fires at the wrong moment is worse than one an author
		// has to notice is missing - `CollectionService.cpp` argues the general
		// case at length.
		constexpr std::array<ServiceSignal, 1> SIGNALS{{
			{"Heartbeat", SignalKind::Heartbeat},
		}};
	}

	const ServiceSurface &RunServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "RunService";
			surface.Methods = METHODS;
			surface.Signals = SIGNALS;
			return surface;
		}();
		return SURFACE;
	}
}
