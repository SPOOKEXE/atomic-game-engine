#include "Bindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>

#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		// The world every instance in this VM belongs to.
		//
		// A light userdata upvalue on each bound function rather than a global,
		// so two runtimes over two worlds cannot reach each other's store — the
		// mistake a file-static would make available.
		Store &StoreOf(lua_State *state) {
			return *static_cast<Store *>(lua_tolightuserdata(state, lua_upvalueindex(1)));
		}

		Entity CheckInstance(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Instance");
			}
			return *static_cast<Entity *>(value);
		}

		// Whether a value is the world.
		bool IsWorkspace(lua_State *state, int index) {
			return lua_touserdatatagged(state, index, TAG_WORLD) != nullptr;
		}

		void PushWorkspace(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace");
		}

		void PushInstance(lua_State *state, Entity entity) {
			void *memory = lua_newuserdatatagged(state, sizeof(Entity), TAG_INSTANCE);
			*static_cast<Entity *>(memory) = entity;

			luaL_getmetatable(state, "Instance");
			lua_setmetatable(state, -2);
		}

		// --- marshalling -----------------------------------------------------
		//
		// **A switch over `PropertyType` and nothing else.** No property is
		// named here and none ever should be: a property `scene` declares
		// tomorrow is readable and writable from Luau today, because this code
		// only ever learned the seven shapes a value can have.

		// Pushes a property's value, having read it into a buffer of its size.
		bool PushValue(lua_State *state, const PropertyDescriptor &property, const void *bytes) {
			switch (property.Type) {
			case PropertyType::Bool:
				lua_pushboolean(state, *static_cast<const bool *>(bytes));
				return true;
			case PropertyType::Float:
				lua_pushnumber(state, *static_cast<const float *>(bytes));
				return true;
			case PropertyType::Double:
				lua_pushnumber(state, *static_cast<const double *>(bytes));
				return true;
			case PropertyType::Int32:
				lua_pushinteger(state, *static_cast<const int32_t *>(bytes));
				return true;
			case PropertyType::Int64:
				lua_pushnumber(state, static_cast<double>(*static_cast<const int64_t *>(bytes)));
				return true;
			case PropertyType::Name:
				// Text, never the interned id. A number that means a string in
				// one process and a different string in the next is the whole
				// hazard `core::Name` exists around.
				lua_pushstring(state, static_cast<const Name *>(bytes)->Text().data());
				return true;
			case PropertyType::Vector3:
				*PushVector3(state) = *static_cast<const core::Vector3 *>(bytes);
				return true;
			case PropertyType::Color3:
				*PushColor3(state) = *static_cast<const core::Color3 *>(bytes);
				return true;
			case PropertyType::CFrame:
				*PushCFrame(state) = *static_cast<const core::CFrame *>(bytes);
				return true;
			case PropertyType::Reference: {
				// A root instance's parent is the world, which is what
				// `workspace` is. Handing back nil would make
				// `part.Parent = workspace` a write a script could not read
				// back, and the two would disagree about the same fact.
				const Entity referenced = *static_cast<const Entity *>(bytes);
				if (referenced == ecs::NULL_ENTITY) {
					PushWorkspace(state);
				} else {
					PushInstance(state, referenced);
				}
				return true;
			}
			case PropertyType::Opaque:
				break;
			}
			return false;
		}

		// Reads a Luau value into a buffer of the property's size.
		bool ReadValue(lua_State *state, int index, const PropertyDescriptor &property, void *out) {
			switch (property.Type) {
			case PropertyType::Bool:
				*static_cast<bool *>(out) = lua_toboolean(state, index) != 0;
				return true;
			case PropertyType::Float:
				*static_cast<float *>(out) = static_cast<float>(luaL_checknumber(state, index));
				return true;
			case PropertyType::Double:
				*static_cast<double *>(out) = luaL_checknumber(state, index);
				return true;
			case PropertyType::Int32:
				*static_cast<int32_t *>(out) = luaL_checkinteger(state, index);
				return true;
			case PropertyType::Int64:
				*static_cast<int64_t *>(out) = static_cast<int64_t>(luaL_checknumber(state, index));
				return true;
			case PropertyType::Name:
				*static_cast<Name *>(out) = Name(luaL_checkstring(state, index));
				return true;
			case PropertyType::Vector3:
				*static_cast<core::Vector3 *>(out) = CheckVector3(state, index);
				return true;
			case PropertyType::Color3:
				*static_cast<core::Color3 *>(out) = CheckColor3(state, index);
				return true;
			case PropertyType::CFrame:
				*static_cast<core::CFrame *>(out) = CheckCFrame(state, index);
				return true;
			case PropertyType::Reference:
				// `part.Parent = workspace` — a root of this world.
				if (IsWorkspace(state, index) || lua_isnil(state, index)) {
					*static_cast<Entity *>(out) = ecs::NULL_ENTITY;
					return true;
				}
				*static_cast<Entity *>(out) = CheckInstance(state, index);
				return true;
			case PropertyType::Opaque:
				break;
			}
			return false;
		}

		const PropertyDescriptor *Find(const Store &store, Entity instance, std::string_view name) {
			const Name key(name);
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Name == key) {
					return &property;
				}
			}
			return nullptr;
		}

		// --- the metatable ---------------------------------------------------

		int InstanceIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}

			// Sized from the descriptor rather than from a guess, so this
			// cannot be the place a size mismatch is introduced.
			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property->Size > sizeof(bytes) ||
				!store.GetProperty(instance, property->Name, bytes, property->Size)) {
				luaL_errorL(state, "could not read '%s'", field);
			}

			if (!PushValue(state, *property, bytes)) {
				luaL_errorL(state, "'%s' has no script representation", field);
			}
			return 1;
		}

		int InstanceNewIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}
			if (!property->Writable) {
				luaL_errorL(state, "'%s' is read-only", field);
			}

			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property->Size > sizeof(bytes) || !ReadValue(state, 3, *property, bytes)) {
				luaL_errorL(state, "'%s' cannot take that value", field);
			}

			// A refusal is an error rather than a silent no-op. A replica
			// rejecting the write is the case that matters: a script author
			// cannot tell "rejected" from "applied and then overwritten by the
			// next delta" without being told.
			if (!store.SetProperty(instance, property->Name, bytes, property->Size)) {
				luaL_errorL(state, "could not set '%s'", field);
			}
			return 0;
		}

		int InstanceToString(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushstring(state, store.InstanceNameOf(instance).Text().data());
			return 1;
		}

		int InstanceNew(lua_State *state) {
			Store &store = StoreOf(state);
			const char *className = luaL_checkstring(state, 1);

			const ecs::ClassId id = ecs::Classes::Find(Name(className));
			if (!id.IsValid()) {
				luaL_errorL(state, "'%s' is not a registered class", className);
			}

			const Entity instance = store.CreateInstance(id, className);
			if (instance == ecs::NULL_ENTITY) {
				luaL_errorL(state, "could not create a '%s'", className);
			}

			PushInstance(state, instance);
			return 1;
		}
	}

	void OpenInstances(lua_State *state, ecs::Store &store) {
		luaL_newmetatable(state, "Instance");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, InstanceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, InstanceNewIndex, "__newindex", 1);
		lua_setfield(state, -2, "__newindex");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, InstanceToString, "__tostring", 1);
		lua_setfield(state, -2, "__tostring");

		// Hidden, for the reason the value types hide theirs: a metatable a
		// script can reach is one it can rewrite, and then every instance in
		// the world changes behaviour underneath everything holding one.
		lua_pushstring(state, "Instance");
		lua_setfield(state, -2, "__metatable");

		lua_pop(state, 1);

		lua_newtable(state);
		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, InstanceNew, "new", 1);
		lua_setfield(state, -2, "new");
		lua_setglobal(state, "Instance");
	}

	namespace {
		int WorkspaceIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const char *field = luaL_checkstring(state, 2);

			// The world's own name — `client.world`, `unified.server`. A
			// script that logs it is telling you which world it is running on,
			// which is a real question the moment a universe holds several.
			if (std::string_view(field) == "Name") {
				lua_pushlstring(state, store.Name().data(), store.Name().size());
				return 1;
			}

			luaL_errorL(state, "the world has no member '%s'", field);
		}

		int WorkspaceToString(lua_State *state) {
			Store &store = StoreOf(state);
			lua_pushlstring(state, store.Name().data(), store.Name().size());
			return 1;
		}
	}

	void OpenWorkspace(lua_State *state, ecs::Store &store) {
		// Zero-sized userdata: it carries no state because the world it stands
		// for is reached through the upvalue every bound function already has.
		// What it needs is an identity a script can compare and assign.
		lua_newuserdatatagged(state, 1, TAG_WORLD);

		luaL_newmetatable(state, "World");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, WorkspaceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, WorkspaceToString, "__tostring", 1);
		lua_setfield(state, -2, "__tostring");

		lua_pushstring(state, "World");
		lua_setfield(state, -2, "__metatable");

		lua_setmetatable(state, -2);

		// Kept in the registry as well as in a global, so the `Parent` getter
		// can hand back *the same* value a script assigned rather than a second
		// object that merely behaves alike.
		lua_pushvalue(state, -1);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.workspace");
		lua_setglobal(state, "workspace");
	}
}
