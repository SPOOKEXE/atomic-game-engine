// `Debris`, in Luau.
//
// One method, and everything interesting about it is in `Debris.hpp`: the unit
// is a tick, the drain order is stated, and the cap fails by destroying early
// because that is the conservative direction for a cleanup call to be wrong in.
//
// @tier L9 · shared

#include "Bindings.hpp"

#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		// How many ticks a duration in seconds rounds to.
		//
		// **Up, and never to zero**, which is `LuauTask.cpp`'s rule for
		// `task.wait` and is settled the same way here rather than differently:
		// a lifetime is never *shorter* than asked, and `AddItem(part, 0)`
		// destroys on the next beat rather than inside the call — where the
		// instance would go while the script that named it is still running.
		uint64_t TicksFor(const ecs::Store &store, double seconds) {
			const float delta = store.Time().Delta;
			if (seconds <= 0.0 || delta <= 0.0f) {
				return 1;
			}

			const double ticks = std::ceil(seconds / static_cast<double>(delta));
			return ticks < 1.0 ? 1 : static_cast<uint64_t>(ticks);
		}

		// `Debris:AddItem(instance, lifetime)`
		//
		// **Answers nothing, like Roblox's.** The one thing a caller might want
		// to know — whether the queue was full — is answered by the item that
		// was evicted going early rather than by this call, and a boolean here
		// would be one almost nobody reads and everybody has to think about.
		int AddItem(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			const ecs::Entity instance = CheckInstanceArgument(state, 2);

			// Roblox's default, and it is the one argument an author omits.
			const double lifetime = luaL_optnumber(state, 3, 10.0);

			const ecs::Entity evicted =
				context.Debris.Add(instance, context.World->Time().Tick + TicksFor(*context.World, lifetime));
			if (evicted != ecs::NULL_ENTITY) {
				context.World->DestroyInstance(evicted);
			}
			return 0;
		}
	}

	void OpenDebrisService(lua_State *state) {
		static constexpr LuauServiceMethod METHODS[] = {
			{"AddItem", AddItem},
		};

		ServiceSurface surface;
		surface.Name = "Debris";
		surface.LuauMethods = METHODS;

		InstallService(state, surface);
	}

	void PumpDebris(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// **Collected and then destroyed**, rather than destroyed from inside
		// the drain: `DestroyInstance` dispatches `DescendantRemoving`
		// synchronously, so a handler runs here — and a handler that adds
		// something to the queue must not do it to a list being walked.
		std::vector<ecs::Entity> expired;
		context.Debris.Advance(context.World->Time().Tick, expired);

		for (const ecs::Entity instance : expired) {
			// **No liveness test first.** An instance destroyed by some other
			// route between being queued and coming due is ordinary, and
			// `DestroyInstance` on a row that has gone is already a no-op —
			// a check here would be a second answer to the same question.
			context.World->DestroyInstance(instance);
		}
	}
}
