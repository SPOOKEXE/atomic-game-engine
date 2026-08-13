#pragma once

// What services this engine has, said once, for every language that binds them.
//
// **The layer between "what a service is" and "how a VM builds one".**
// `Bindings.hpp`'s `ServiceSurface` is the Luau half — a name, methods, signals
// and a metatable — and `JsBindings.hpp` has its own. Neither can say the thing
// both need to agree about: *which services exist*. So each side carried its own
// list, and the two drifted exactly as two lists do. Luau installs nine surface
// services; JavaScript installs five. `ContentService`, `UserInputService`,
// `ContextActionService` and `BreakpointService` are reachable from one language
// and not the other, nothing in the build says so, and the TypeScript
// declarations claim two of them anyway.
//
// **A catalogue rather than self-registration, and the difference is a linker
// one.** The obvious shape is a static registrar per service file, each adding
// itself to a vector at load. It does not survive this build: modules here are
// static libraries, and an object file whose only exported symbol is a registrar
// nobody references is one the archive is free to drop — so the service compiles,
// links, and is absent at runtime, typically in the release build and typically
// months later. `scene::RegisterSceneComponents` and `RegisterSceneClasses` are
// explicit for the same reason.
//
// `ServiceCatalogue.cpp` is the one file that names every installer, which is
// what makes the linker keep them, and it is the only place a service is
// declared. Everything else — both runtimes and the binding generator — reads
// it.
//
// **A service missing from a language is a stated fact and not an absence.** A
// runtime asked for a service its language does not bind refuses by *name*,
// saying which language has it, rather than failing the way an invented service
// fails. A script author who gets "there is no such service" from something the
// documentation lists goes looking in the wrong place.
//
// @tier L9 · shared
// @since v0.15

#include <cstdint>
#include <span>

namespace engine::script {

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

	constexpr ServiceLanguages operator|(ServiceLanguages left, ServiceLanguages right) {
		return static_cast<ServiceLanguages>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
	}

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

		// A studio only. The installer still decides — `OpenBreakpointService`
		// checks the debugger pointer as well — so this is what the *catalogue*
		// knows, which is what a generator and a refusal message need. It is not
		// a second gate on the install.
		Studio,
	};

	// One service, independent of any VM.
	//
	// **No installer here, and that is what keeps this header clean.** A
	// function pointer taking a `lua_State *` would pull Luau into every
	// JavaScript translation unit and a `JSContext *` would do the reverse —
	// which is precisely the coupling `mono.engine/script/CMakeLists.txt`
	// refuses when it keeps both VMs `VENDOR` rather than `VENDOR_PUBLIC`. The
	// installers live in `ServiceCatalogue.cpp`, which is the one file allowed
	// to have met both.
	struct ServiceDefinition {
		// What a script calls it, and what `game:GetService` resolves.
		const char *Name;

		ServiceAvailability Availability;

		// Which languages have a real binding for it.
		//
		// **A language without one is a refusal, not a silence.** See the header
		// comment: the runtime says which language does have it.
		ServiceLanguages Languages;
	};

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
}
