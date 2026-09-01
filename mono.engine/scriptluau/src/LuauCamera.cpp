#include "LuauBindings.hpp"

#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/InstanceShim.hpp>

#include <lualib.h>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::Store;

		Entity CameraEntity(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Camera");
			}
			return *static_cast<Entity *>(value);
		}
	}

	void PushCurrentCamera(lua_State *state) {
		Store &store = *ContextOf(state).World;

		const auto *active = store.Resource<scene::ActiveCamera>();
		if (active == nullptr || active->Entity == ecs::NULL_ENTITY ||
			!InstanceAlive(store, active->Entity)) {
			// **Nil rather than a camera made on demand.** Roblox's
			// `workspace.CurrentCamera` is never nil because the client makes
			// one; here a headless world genuinely has none, and inventing a row
			// so a property has something to point at would put a phantom camera
			// in every server world - the same mistake `workspace` avoids by not
			// being an instance.
			lua_pushnil(state);
			return;
		}

		void *memory = lua_newuserdatatagged(state, sizeof(Entity), TAG_INSTANCE);
		*static_cast<Entity *>(memory) = active->Entity;

		luaL_getmetatable(state, "Instance");
		lua_setmetatable(state, -2);
	}

	void SetCurrentCamera(lua_State *state, int index) {
		Store &store = *ContextOf(state).World;

		if (lua_isnil(state, index)) {
			// Detaching is a real operation: a script tearing down a cutscene
			// camera wants the world to have none rather than to keep pointing
			// at a row it is about to destroy.
			scene::ActiveCamera active;
			if (const auto *existing = store.Resource<scene::ActiveCamera>(); existing != nullptr) {
				// The aspect ratio is the *consumer's*, not the camera's - a
				// window wrote it - so it survives a camera change. Clearing it
				// would make the next resolved frame use a ratio of one and
				// stretch every view until whatever owns the window wrote again.
				active = *existing;
			}
			active.Entity = ecs::NULL_ENTITY;
			store.SetResource(active);
			return;
		}

		const Entity camera = CameraEntity(state, index);

		// Refused when the instance carries no `Camera`, rather than accepted
		// and then skipped by every consumer: each of them resolves its own
		// matrices from the row this names and gives up on a row with no lens,
		// so the symptom would be a view that stopped following anything with
		// nothing reporting why.
		if (store.Get<scene::Camera>(camera) == nullptr) {
			luaL_errorL(state, "CurrentCamera must be an instance carrying a Camera");
		}

		scene::ActiveCamera active;
		if (const auto *existing = store.Resource<scene::ActiveCamera>(); existing != nullptr) {
			active = *existing;
		}
		active.Entity = camera;
		store.SetResource(active);
	}
}
