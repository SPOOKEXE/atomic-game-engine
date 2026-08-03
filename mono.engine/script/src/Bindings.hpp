#pragma once

// What a script can hold and what it can touch.
//
// Two halves, and they meet at `PropertyType`. The value types are what a
// property's bytes mean on the script side; the instance binding is what turns
// a name and one of those values into `Store::SetProperty`.
//
// **The marshalling is a switch over `PropertyType` and nothing else.** No
// per-property code, no table of special cases: a property added to `scene`
// tomorrow is reachable from Luau today, because the binding never learned any
// property's name. That is the payoff for making a property a conversion.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>

#include <lua.h>
#include <string>

namespace engine::script {

	// Userdata tags. Luau checks these on every access, so a `Color3` handed to
	// something expecting a `Vector3` is caught by the VM rather than by a
	// reinterpret_cast that happens to line up — the two are three floats each.
	enum : int {
		TAG_VECTOR3 = 1,
		TAG_COLOR3 = 2,
		TAG_CFRAME = 3,
		TAG_INSTANCE = 4,

		// The world a script is running on. Not an instance: a world is the
		// container entities live in, not one of them.
		TAG_WORLD = 5,
	};

	// Installs `Vector3`, `Color3` and `CFrame` as globals.
	void OpenValues(lua_State *state);

	// Pushes a fresh value of each type and returns it for filling in. The
	// instance binding uses these to hand a property's bytes back to a script.
	core::Vector3 *PushVector3(lua_State *state);
	core::Color3 *PushColor3(lua_State *state);
	core::CFrame *PushCFrame(lua_State *state);

	// Reads a value of each type, raising a Luau type error when the argument
	// is something else. The tag is what makes this safe: `Vector3` and
	// `Color3` are the same three floats, so a check on shape would pass.
	core::Vector3 &CheckVector3(lua_State *state, int index);
	core::Color3 &CheckColor3(lua_State *state, int index);
	core::CFrame &CheckCFrame(lua_State *state, int index);

	// Installs `RunService`, whose `Heartbeat` a script connects behaviour to.
	//
	// One signal, and deliberately only one. Roblox has `Heartbeat`, `Stepped`
	// and `RenderStepped`, and the difference between them is *where in the
	// frame* they run — which is a question about the tick this engine has not
	// answered for scripts yet. Three names for one point would be three
	// promises, two of them false.
	void OpenRunService(lua_State *state);

	// Installs `game`, whose `GetService` is how a Roblox script reaches one.
	void OpenGame(lua_State *state);

	// Installs the Universe's services — the only route out of a world.
	//
	// `MessagingService` today. `MemoryStoreService` and `DataStoreService`
	// want a reply, and a reply arrives at a later barrier, so a script would
	// have to *yield* on it — which `docs/SCRIPT_CONCURRENCY.md` §1 says needs
	// a resume rule this version has not built. Publishing and subscribing need
	// no such thing, which is exactly why they are here and those are not.
	void OpenServices(lua_State *state, ecs::Store &store);

	// Dispatches this tick's deliveries to their subscribers.
	//
	// @return An error message when a subscriber raised, or empty.
	std::string PumpDeliveries(lua_State *state, ecs::Store &store);

	// Installs `workspace` — **the world this script is running on**.
	//
	// Roblox's `Workspace` is a service inside the `DataModel` holding
	// everything with a position. Here the mapping is one step more direct:
	//
	//     game      -> the universe
	//     workspace -> the world this script runs on
	//
	// **It is not an instance**, and that is the honest shape rather than a
	// simplification. A world is what entities live *in*; making it an entity
	// would have put a phantom row in every scene, counted by nothing and drawn
	// by nothing, so that one property could have something to point at.
	//
	// `part.Parent = workspace` therefore means "a root instance of this
	// world", which is what `Store::SetParent(part, NULL_ENTITY)` already
	// means, and reading `part.Parent` back on a root hands `workspace` over.
	//
	// @param state The VM.
	// @param store The world this script runs on.
	void OpenWorkspace(lua_State *state, ecs::Store &store);

	// Calls every connected Heartbeat function with `delta`.
	//
	// @return An error message when one raised, or empty.
	std::string PumpHeartbeat(lua_State *state, float delta);

	// Installs `Instance`, and the metatable that turns `part.Size = v` into a
	// property write.
	//
	// @param state The VM.
	// @param store The world instances are created in.
	void OpenInstances(lua_State *state, ecs::Store &store);
}
