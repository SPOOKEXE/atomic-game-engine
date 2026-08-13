// The one place a service is declared.
//
// **This file exists to be the only list.** `ServiceCatalogue.hpp` carries the
// argument in full; the short version is that "which services exist" was two
// lists — one per VM, each written as control flow inside an install function —
// so Luau bound nine and JavaScript bound five, four of them were reachable from
// one language and not the other, and nothing in the build said so. The
// TypeScript declarations claimed two of the four anyway.
//
// **It is also the only file that has met both VMs**, which is what lets the
// table hold a function pointer per language. A `void (*)(lua_State *)` in a
// shared header would pull Luau into every quickjs translation unit and the
// reverse would do the same — the coupling `mono.engine/script/CMakeLists.txt`
// refuses when it keeps both VMs `VENDOR` rather than `VENDOR_PUBLIC`. So the
// two runtimes each get a view of this table in their own currency, built here.
//
// **And naming every installer is what keeps them.** Modules here are static
// libraries, and an object file no symbol reaches is one the archive may drop —
// which is why self-registration was refused. Every row below is a reference,
// so every service survives the link.
//
// @tier L9 · shared

#include "ServiceCatalogue.hpp"

#include "Bindings.hpp"
#include "JsBindings.hpp"

#include <array>
#include <cstring>

namespace engine::script {

	namespace {

		// One row: what the service is, and how each language builds it.
		//
		// **A null installer is a language that does not bind this service.** It
		// is not a hole to be filled in silently — `ServiceLanguages` says the
		// same fact in a form a refusal message and the binding generator can
		// read, and the two are checked against each other below so a row cannot
		// claim a language it has no installer for.
		struct Row {
			ServiceDefinition Definition;
			void (*Luau)(lua_State *);
			void (*JavaScript)(JSContext *, JSValueConst);
		};

		// Every service, in install order.
		//
		// **The order is the tick's, not the alphabet's.** Nothing here may be
		// sorted: the bus services need `OpenBusSupport` to have run, and the
		// studio row installs after the debugger pointer is set and before
		// `luaL_sandbox` freezes the globals — which is why it is walked in a
		// second pass rather than moved up the list.
		constexpr std::array<Row, 14> ROWS{{
			// --- the bus, which is the only route out of a world ---------------
			{{"MessagingService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenMessagingService,
			 OpenJsMessagingService},

			// **`GetTeleportData` is Luau-only inside a service both languages
			// bind**, which this table cannot express and is worth naming here
			// rather than losing: the mask is per service, and per *method*
			// parity is the next layer down. See `ServiceCatalogue.hpp`.
			{{"TeleportService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenTeleportService,
			 OpenJsTeleportService},

			{{"MemoryStoreService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenMemoryStoreService,
			 OpenJsMemoryStoreService},

			{{"DataStoreService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenDataStoreService,
			 OpenJsDataStoreService},

			// --- where a script is standing, and when ---------------------------
			// **The addressed route, beside the fan-out it is not.** See
			// `world::BusKind::Channel`: a topic has no destination and a
			// teleport carries a person, so this is the one way a world says
			// something to one named world.
			{{"CrossWorldService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenCrossWorldService,
			 nullptr},

			{{"RunService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenRunService,
			 OpenJsRunService},

			// --- the Luau-only four, stated rather than discovered --------------
			//
			// Each of these is a real gap and not a decision. They are declared
			// with the languages they actually have, so `GetService` can say which
			// language binds them and the generator can stop declaring a type for
			// a runtime that has none.
			{{"UserInputService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenUserInputService,
			 nullptr},

			{{"ContextActionService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenContextActionService,
			 nullptr},

			{{"ContentService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenContentService,
			 nullptr},

			{{"CollectionService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenCollectionService,
			 nullptr},

			{{"HttpService", ServiceAvailability::Always, ServiceLanguages::Luau}, OpenHttpService, nullptr},

			// **Luau only because it carries a live property**, which is a
			// stronger reason than the four above it have. `ServiceSurface`
			// builds a property-bearing service as a *userdata* to defeat
			// `safeenv`'s `GETIMPORT` caching, and `JsBindings.hpp` has no such
			// mechanism — every JavaScript service is a hand-built object with
			// method properties on it. Binding this there means either a
			// `JS_DefinePropertyGetSet` pair per property or a JavaScript twin of
			// `ServiceSurface`, and the second is the one worth having, because
			// `UserInputService` is waiting behind the same gap.
			{{"SoundService", ServiceAvailability::Always, ServiceLanguages::Luau},
			 OpenSoundService,
			 nullptr},

			// --- the two that step on the tick ---------------------------------
			//
			// **Both languages, because nothing about either is per language.**
			// What a tween is and when a deadline arrives live in `Tweens.hpp`
			// and `Debris.hpp`; each binding supplies a handle and a callable,
			// which is the split every shared piece of this module is on.
			//
			// Ordinary rows despite being time-stepped: what makes them step is
			// `PumpTweens` and `PumpDebris` at the barrier, not anything about
			// where they are installed.
			{{"TweenService", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenTweenService,
			 OpenJsTweenService},

			{{"Debris", ServiceAvailability::Always, ServiceLanguages::Both},
			 OpenDebrisService,
			 OpenJsDebrisService},
		}};

		// The studio's, walked separately.
		//
		// **A second table rather than a flag on the first**, because the two are
		// walked at different moments and a single list would have every caller
		// filtering it. `BreakpointService` cannot install with the others: it
		// reads `LuauContext::Breakpoints` to decide whether to install at all, so
		// it must run after that pointer is set, and it writes a global, so it
		// must run before the sandbox freezes the table.
		constexpr std::array<Row, 1> STUDIO_ROWS{{
			{{"BreakpointService", ServiceAvailability::Studio, ServiceLanguages::Luau},
			 OpenBreakpointService,
			 nullptr},
		}};

		// Every definition, both phases, for the callers that want the whole
		// picture rather than one phase's work.
		const std::array<ServiceDefinition, ROWS.size() + STUDIO_ROWS.size()> &Definitions() {
			static const auto built = [] {
				std::array<ServiceDefinition, ROWS.size() + STUDIO_ROWS.size()> all{};
				size_t next = 0;
				for (const Row &row : ROWS) {
					all[next++] = row.Definition;
				}
				for (const Row &row : STUDIO_ROWS) {
					all[next++] = row.Definition;
				}
				return all;
			}();
			return built;
		}
	}

	std::span<const ServiceDefinition> Services() {
		return Definitions();
	}

	const ServiceDefinition *FindService(const char *name) {
		if (name == nullptr) {
			return nullptr;
		}

		// A linear walk over the table, and it stays one until the count is a
		// reason rather than a habit. **Deliberately not a number here**: the row
		// count has been wrong in this comment twice already, and a stale number
		// in a sentence about performance is worse than no number. This is reached
		// once per `GetService` on a name the globals did not answer, which is the
		// miss path.
		for (const ServiceDefinition &definition : Definitions()) {
			if (std::strcmp(definition.Name, name) == 0) {
				return &definition;
			}
		}
		return nullptr;
	}

	void InstallLuauServices(lua_State *state, ServiceAvailability phase) {
		// **The bus prep, once, before the services that need it.** A `Postbox`
		// read on a store that never registered its resources mints them under
		// the compiler's spelling and aborts the *next* `Universe` to register
		// them properly — see `OpenBusSupport`. Idempotent, so the studio phase
		// calling it again costs a hash lookup.
		if (phase == ServiceAvailability::Always) {
			OpenBusSupport(state);
		}

		for (const Row &row : phase == ServiceAvailability::Always ? std::span<const Row>(ROWS)
																   : std::span<const Row>(STUDIO_ROWS)) {
			// A row this language does not bind installs nothing. The refusal
			// happens where a script asks for it by name, which is the only place
			// a script can tell the difference.
			if (row.Luau != nullptr) {
				row.Luau(state);
			}
		}
	}

	void InstallJsServices(JSContext *context, JSValueConst global, ServiceAvailability phase) {
		// **No bus prep here, unlike the Luau walk.** `OpenJsBindings` already
		// calls `world::RegisterMailboxTypes()` before anything constructs a
		// `Postbox`, for the reason `OpenBusSupport` states at length. A second
		// call would be harmless and would also be a second place that rule is
		// remembered.
		for (const Row &row : phase == ServiceAvailability::Always ? std::span<const Row>(ROWS)
																   : std::span<const Row>(STUDIO_ROWS)) {
			if (row.JavaScript != nullptr) {
				row.JavaScript(context, global);
			}
		}
	}
}
