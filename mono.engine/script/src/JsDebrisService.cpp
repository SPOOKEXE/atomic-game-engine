// `Debris`, in JavaScript.
//
// The twin of `DebrisService.cpp`, down to the rounding: a lifetime is seconds
// in and ticks underneath, and it rounds up so a deadline is never shorter than
// the one asked for. `Debris.hpp` carries the rest.
//
// @tier L9 · shared

#include "JsBindings.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		// How many ticks a duration in seconds rounds to. `DebrisService.cpp`'s,
		// and it says why up and never to zero.
		uint64_t TicksFor(const ecs::Store &store, double seconds) {
			const float delta = store.Time().Delta;
			if (seconds <= 0.0 || delta <= 0.0f) {
				return 1;
			}

			const double ticks = std::ceil(seconds / static_cast<double>(delta));
			return ticks < 1.0 ? 1 : static_cast<uint64_t>(ticks);
		}

		// `Debris.AddItem(instance, lifetime)`
		JSValue AddItem(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			JsContext &bound = JsOf(context);

			const ecs::Entity instance = count > 0 ? JsEntityOf(context, argv[0]) : ecs::NULL_ENTITY;
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "Debris.AddItem: expected an Instance");
			}

			// Roblox's default, and it is the one argument an author omits.
			double lifetime = 10.0;
			if (count > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(context, &lifetime, argv[1]) != 0) {
				return JS_ThrowTypeError(context, "Debris.AddItem: expected a lifetime in seconds");
			}

			const ecs::Entity evicted =
				bound.Debris.Add(instance, bound.World->Time().Tick + TicksFor(*bound.World, lifetime));
			if (evicted != ecs::NULL_ENTITY) {
				bound.World->DestroyInstance(evicted);
			}
			return JS_UNDEFINED;
		}
	}

	void OpenJsDebrisService(JSContext *context, JSValueConst global) {
		JSValue service = JS_NewObject(context);
		JS_SetPropertyStr(context, service, "AddItem", JS_NewCFunction(context, AddItem, "AddItem", 2));
		JS_SetPropertyStr(context, global, "Debris", service);
	}

	void PumpJsDebris(JSContext *context) {
		JsContext &bound = JsOf(context);

		// Collected and then destroyed, for the Luau half's reason:
		// `DestroyInstance` dispatches `DescendantRemoving` synchronously, so a
		// handler runs here and must not be able to add to a list being walked.
		std::vector<ecs::Entity> expired;
		bound.Debris.Advance(bound.World->Time().Tick, expired);

		for (const ecs::Entity instance : expired) {
			bound.World->DestroyInstance(instance);
		}

		// **Answers nothing, unlike every other pump.** A removal fires
		// `DescendantRemoving` from inside the store, where there is nowhere to
		// return an error to and the handler's failure is already logged — see
		// the hook `LuauSignals.cpp` and `JsSurface.cpp` each install. A
		// `std::string` return here would always be empty, which is a promise
		// that errors are reported through it.
	}
}
