// The one place a service is declared.
//
// **This file exists to be the only list.** `ServiceCatalogue.hpp` carries the
// argument in full; the short version is that "which services exist" was two
// lists - one per VM, each written as control flow inside an install function -
// so Luau bound nine and JavaScript bound five, four of them were reachable from
// one language and not the other, and nothing in the build said so. The
// TypeScript declarations claimed two of the four anyway.
//
// **It has met neither VM since v0.19, and that is the point of the split.**
// This file decides *which* services install and in *what order*, and hands the
// answer back as data. `scriptluau/src/LuauServices.cpp` walks it into a Luau
// table and `scriptjs/src/JsServices.cpp` into a JavaScript object; neither
// walk is this file's business, and neither VM is this module's.
//
// **And naming every installer is what keeps them.** Modules here are static
// libraries, and an object file no symbol reaches is one the archive may drop -
// which is why self-registration was refused. Every row below is a reference,
// so every service survives the link.
//
// @tier L9 · shared

#include <engine/script/ServiceCatalogue.hpp>

#include <array>
#include <cstring>

namespace engine::script {

	namespace {

		using Row = ServiceRow;

		// Every service, in install order.
		//
		// **The order is the tick's, not the alphabet's.** Nothing here may be
		// sorted: the bus services need the mailbox types registered, and the
		// studio row installs after the debugger pointer is set and before
		// `luaL_sandbox` freezes the globals - which is why it is walked in a
		// second pass rather than moved up the list.
		constexpr std::array<Row, 16> ROWS{{
			// --- the bus, which is the only route out of a world ---------------
			//
			// **All four described once since v0.16, and the last of them is what
			// retired `ServiceSurface::LuauMethods`' last excuse.**
			// `TeleportService::GetTeleportData` was a per-*method* gap inside a
			// service both languages bound - something the language mask cannot
			// express - and it closed with no new mechanism: it is a
			// `ServiceMethod` row, and both VMs install every row.
			//
			// What made the two stores describable is `ScriptCall::Await`. They
			// suspend, and the two languages suspend differently - a yielded
			// coroutine and a `Promise` - which is one member on the interface
			// rather than seven methods written twice.
			{{"MessagingService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Messaging},
			 MessagingServiceSurface},

			{{"TeleportService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Teleport},
			 TeleportServiceSurface},

			{{"MemoryStoreService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Persistence},
			 MemoryStoreServiceSurface},

			{{"DataStoreService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Persistence},
			 DataStoreServiceSurface},

			// --- where a script is standing, and when ---------------------------
			// **The addressed route, beside the fan-out it is not.** See
			// `world::BusKind::Channel`: a topic has no destination and a
			// teleport carries a person, so this is the one way a world says
			// something to one named world.
			//
			// **The first service described once**, which is what made it
			// reachable from JavaScript - its signal needed nothing new, because
			// a signal has crossed languages since v0.6. What it needed was
			// `PumpJsDeliveries` learning `BusKind::Channel`.
			//
			// **A channel is named as well as addressed since v0.17**, so
			// `OpenChannel` hands back the signal for one channel rather than the
			// service carrying a field that heard all of them.
			{{"CrossWorldService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Messaging},
			 CrossWorldServiceSurface},

			{{"RunService", ServiceAvailability::Always, ServiceLanguages::Both}, RunServiceSurface},

			{{"ComputeService", ServiceAvailability::Always, ServiceLanguages::Both}, ComputeServiceSurface},

			// --- the two the property mechanism closed --------------------------
			//
			// **Both carry a live *property*, and that was the last mechanism a
			// `ServiceSurface` could not describe.** It could describe a method,
			// so five services crossed at v0.16 and these two did not: Luau binds
			// a property by making the service a *userdata* to defeat `safeenv`'s
			// `GETIMPORT` caching, and a `ScriptMethod` is a call where a property
			// is an accessor.
			//
			// `ServiceProperty` is what closed it, and the shape it took is
			// decided by JavaScript rather than by Luau: a native accessor runs on
			// every read and needs no userdata at all, but is registered *per
			// name* - so the catch-all `__index` had to become a list before the
			// other language could have one. The Luau half walks that same list
			// now, which also retires a chain of `if (field == ...)`.
			{{"UserInputService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Input},
			 UserInputServiceSurface},

			{{"SettingsService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Input},
			 SettingsServiceSurface},

			{{"SoundService", ServiceAvailability::Always, ServiceLanguages::Both, ScriptCapabilities::Audio},
			 SoundServiceSurface},

			// --- the four that stopped being Luau's at v0.16 --------------------
			//
			// Each was a real gap and not a decision, and each closed the same
			// way: the methods became `ScriptMethod` rows and the service became
			// a `ServiceSurface` both installers read. See `ServiceSurface.hpp`.
			//
			// **`ContextActionService` is the one that needed more than a
			// rewrite.** Its handler is a callable, so `ActionStack` had to
			// become shared state with the callables left opaque; and a bound
			// action that never fires is worse than one that cannot be bound, so
			// this language gained an input pump - `PumpJsInput` - at the same
			// time.
			{{"ContextActionService",
			  ServiceAvailability::Always,
			  ServiceLanguages::Both,
			  ScriptCapabilities::Input},
			 ContextActionServiceSurface},

			{{"ContentService", ServiceAvailability::Always, ServiceLanguages::Both}, ContentServiceSurface},

			{{"CollectionService", ServiceAvailability::Always, ServiceLanguages::Both},
			 CollectionServiceSurface},

			{{"HttpService", ServiceAvailability::Always, ServiceLanguages::Both}, HttpServiceSurface},

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
			//
			// **`TweenService` hands back a handle whose three methods are still
			// per language, and that is a decision rather than a remainder** -
			// `Tweens.hpp` and `ScriptCall::ReturnTween` carry it. The apparatus
			// is installed with the datatypes rather than from a row here, because
			// a `Tween` is a value type a method answers with and not a service.
			{{"TweenService", ServiceAvailability::Always, ServiceLanguages::Both}, TweenServiceSurface},

			{{"Debris", ServiceAvailability::Always, ServiceLanguages::Both}, DebrisServiceSurface},
		}};

		// The studio's, walked separately.
		//
		// **A second table rather than a flag on the first**, because the two are
		// walked at different moments and a single list would have every caller
		// filtering it. `BreakpointService` cannot install with the others: it
		// reads `LuauContext::Breakpoints` to decide whether to install at all, so
		// it must run after that pointer is set, and it writes a global, so it
		// must run before the sandbox freezes the table.
		//
		// **`BreakpointService` is Luau's alone because breakpoints are, and
		// that is a feature gap rather than a binding one.** `Debugger::Add`
		// refuses a `.js`, `.mjs`, `.cjs`, `.ts` or `.tsx` chunk outright - see
		// `BreakpointsRefused` - so a JavaScript binding would be a service every
		// method of which reports that nothing can be armed. That is the surface
		// `HttpService`'s absent three are refused for being: one that looks
		// decided. Closing it means teaching QuickJS to report a line, which is
		// `DEFERRED.md` D00106 and not a `ServiceSurface`.
		constexpr std::array<Row, 1> STUDIO_ROWS{{
			{{"BreakpointService",
			  ServiceAvailability::Studio,
			  ServiceLanguages::Luau,
			  ScriptCapabilities::StudioDebug},
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

	std::span<const ServiceRow> ServiceRows(ServiceAvailability phase) {
		return phase == ServiceAvailability::Always ? std::span<const Row>(ROWS)
													: std::span<const Row>(STUDIO_ROWS);
	}
}
