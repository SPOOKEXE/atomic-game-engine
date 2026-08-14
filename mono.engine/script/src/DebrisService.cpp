// `Debris`, in neither language.
//
// One method, and everything interesting about it is in `Debris.hpp`: the unit
// is a tick, the drain order is stated, and the cap fails by destroying early
// because that is the conservative direction for a cleanup call to be wrong in.
//
// **Written twice until v0.16, and neither half did anything a VM decides.**
// This file and a retired `JsDebrisService.cpp` were the same twenty lines with
// two spellings of "read an instance and a number" around them — including two
// copies of `TicksFor`, which is where the two could most easily have come to
// disagree about what half a second means. The service is a `ServiceSurface`
// now and the pump takes a store and a queue, so there is nothing left for a
// second file to hold.
//
// **And `TicksFor` is `Tasks.hpp`'s since v0.18**, because a third copy of it
// survived the merge in each of the two `task.wait` adapters. A deadline here is
// the same arithmetic a wait uses, which is a rule this module states and now
// only spells once.
//
// @tier L9 · shared

#include "Debris.hpp"
#include "ScriptCall.hpp"
#include "ServiceSurface.hpp"
#include "Tasks.hpp"

#include <array>
#include <vector>

namespace engine::script {

	namespace {
		// `Debris:AddItem(instance, lifetime)`
		//
		// **Answers nothing, like Roblox's.** The one thing a caller might want
		// to know — whether the queue was full — is answered by the item that
		// was evicted going early rather than by this call, and a boolean here
		// would be one almost nobody reads and everybody has to think about.
		void AddItem(ScriptCall &call) {
			const ecs::Entity instance = call.AsInstance(0);

			// Roblox's default, and it is the one argument an author omits.
			const double lifetime = call.IsNil(1) ? 10.0 : call.AsNumber(1);

			ecs::Store &store = call.World();
			const ecs::Entity evicted =
				call.Debris().Add(instance, store.Time().Tick + TicksFor(store, lifetime));
			if (evicted != ecs::NULL_ENTITY) {
				store.DestroyInstance(evicted);
			}
		}

		constexpr std::array<ServiceMethod, 1> METHODS{{
			{"AddItem", AddItem},
		}};
	}

	const ServiceSurface &DebrisServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "Debris";
			surface.Methods = METHODS;
			return surface;
		}();
		return SURFACE;
	}

	void PumpDebris(ecs::Store &store, DebrisQueue &queue) {
		// **Collected and then destroyed**, rather than destroyed from inside
		// the drain: `DestroyInstance` dispatches `DescendantRemoving`
		// synchronously, so a handler runs here — and a handler that adds
		// something to the queue must not do it to a list being walked.
		std::vector<ecs::Entity> expired;
		queue.Advance(store.Time().Tick, expired);

		for (const ecs::Entity instance : expired) {
			// **No liveness test first.** An instance destroyed by some other
			// route between being queued and coming due is ordinary, and
			// `DestroyInstance` on a row that has gone is already a no-op —
			// a check here would be a second answer to the same question.
			store.DestroyInstance(instance);
		}
	}
}
