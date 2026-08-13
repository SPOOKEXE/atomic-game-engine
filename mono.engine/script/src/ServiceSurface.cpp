// How a surface service becomes a global.
//
// **One file for the ten lines that were in five.** `Bindings.hpp`'s services
// section carries the argument; what is here is the loop itself, which is short
// enough that having it five times looked harmless and was not — the reason to
// have it once is that it is now somewhere a rule about services can be written
// down and stay true, rather than five places that each have to remember.
//
// @tier L9 · shared

#include "Bindings.hpp"

#include <lua.h>
#include <lualib.h>

namespace engine::script {

	void InstallService(lua_State *state, const ServiceSurface &surface) {
		// A service with no name is a table nothing can reach — installed as a
		// global called nothing, or as nothing at all. Refused loudly here
		// rather than at the `lua_setglobal`, which would take a null and put
		// the table somewhere no script can name.
		if (surface.Name == nullptr) {
			return;
		}

		LuauContext &context = ContextOf(state);

		lua_newtable(state);

		// **Signals first, so a method of the same name wins.** See
		// `ServiceSurface::Signals` — nothing relies on this today and the order
		// is fixed here so that the day something does, the answer was decided
		// rather than inherited from whichever loop happened to run last.
		//
		// No context upvalue: a signal is a value `PushSignal` builds, and what
		// it needs to know is the kind and the subject. A service signal has no
		// subject — `RunService.Heartbeat` is the world's, not any instance's —
		// which is what `NULL_ENTITY` means here.
		for (const ServiceSignal &signal : surface.Signals) {
			PushSignal(state, signal.Kind, ecs::NULL_ENTITY);
			lua_setfield(state, -2, signal.Name);
		}

		// **The context as upvalue 1, on every method, without exception.** This
		// is the invariant the five copies each had to remember on their own:
		// `UpvalueContext` reads index 1 and a method installed with
		// `lua_pushcfunction` gives it nothing to read — which compiles, links,
		// runs, and dereferences whatever was there.
		for (const ServiceMethod &method : surface.Methods) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}

		// **A service with no properties is the table just built, and that is
		// most of them.** One object, and this line is why: `RunService::
		// GetService` looks in the globals before it looks at the tree, so the
		// table set here is the same table `game:GetService(name)` hands back —
		// never a second one built to look like it, which a script comparing the
		// two would tell apart immediately.
		if (surface.Index == nullptr) {
			lua_setglobal(state, surface.Name);
			return;
		}

		// **A service with properties is a *userdata*, and the table above
		// becomes its method table in the registry.** `ServiceSurface::Index`
		// carries the whole argument: on a table, `safeenv` turns a property
		// read into a `GETIMPORT` that resolves once and caches, so a live value
		// reads as a frozen one. A userdata's field access is never an import.
		//
		// **Refused rather than half-built** when the two fields a userdata
		// service cannot do without are missing. A tag of zero is `lua_
		// newuserdatatagged`'s untagged case, which every other untagged
		// userdata in the VM also matches; a null key leaves the methods
		// unreachable and every method call on the service a nil-index error at
		// a distance from the install that caused it.
		if (surface.Tag == 0 || surface.MethodsKey == nullptr) {
			lua_pop(state, 1);
			return;
		}

		lua_setfield(state, LUA_REGISTRYINDEX, surface.MethodsKey);

		// Zero bytes of payload: what the object *is* is its metatable. An
		// `Instance` carries an `ecs::Entity` because it needs one; a service
		// needs nothing, because the world is on the context every method and
		// both metamethods already hold.
		lua_newuserdatatagged(state, 1, surface.Tag);
		lua_newtable(state);

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, surface.Index, "__index", 1);
		lua_setfield(state, -2, "__index");

		if (surface.NewIndex != nullptr) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, surface.NewIndex, "__newindex", 1);
			lua_setfield(state, -2, "__newindex");
		}

		// **Named, so `getmetatable` hands back a string rather than the table.**
		// A script that could reach the metatable could replace `__index` and
		// change what every other script on this VM sees the service do.
		lua_pushstring(state, surface.Name);
		lua_setfield(state, -2, "__metatable");

		lua_setmetatable(state, -2);
		lua_setglobal(state, surface.Name);
	}
}
