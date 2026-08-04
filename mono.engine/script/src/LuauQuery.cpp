#include "Bindings.hpp"

#include <engine/physics/Query.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <lualib.h>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;

		// What a raycast is told to ignore.
		//
		// **A layer mask, not a list of instances**, and that is the shape the
		// engine already has: `physics::Raycast` filters on
		// `spatial::LayerMask`, which is what a collision group resolves to. A
		// `FilterDescendantsInstances` list — Roblox's — would be a per-ray
		// walk of a subtree, and the engine has a bit test that answers the same
		// question in one instruction.
		//
		// So the field is `CollisionGroup` and the omission is stated rather
		// than silent: an author who wants "everything except that model" gives
		// the model a group.
		struct RaycastFilter {
			spatial::LayerMask Mask = spatial::LayerMask::All();
		};

		RaycastFilter &CheckParams(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_RAYCAST_PARAMS);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "RaycastParams");
			}
			return *static_cast<RaycastFilter *>(value);
		}

		int RaycastParamsNew(lua_State *state) {
			void *memory = lua_newuserdatatagged(state, sizeof(RaycastFilter), TAG_RAYCAST_PARAMS);
			new (memory) RaycastFilter();

			luaL_getmetatable(state, "RaycastParams");
			lua_setmetatable(state, -2);
			return 1;
		}

		int RaycastParamsIndex(lua_State *state) {
			const RaycastFilter &filter = CheckParams(state, 1);
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "CollisionGroup") {
				// The lowest set bit, for the reason `CollisionGroup` on a part
				// reads one: a filter names one group even though the mask could
				// hold several.
				for (uint32_t index = 0; index < spatial::LayerMask::LAYER_COUNT; index++) {
					if ((filter.Mask.Bits & (1u << index)) != 0) {
						lua_pushstring(state, spatial::CollisionGroups::NameOf(index).Text().data());
						return 1;
					}
				}
				lua_pushstring(state, "");
				return 1;
			}

			luaL_errorL(state, "RaycastParams has no member '%s'", std::string(field).c_str());
		}

		int RaycastParamsNewIndex(lua_State *state) {
			RaycastFilter &filter = CheckParams(state, 1);
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "CollisionGroup") {
				const char *group = luaL_checkstring(state, 3);
				const uint32_t index = spatial::CollisionGroups::IndexOf(Name(group));

				// Refused rather than defaulted to everything. A typo that
				// quietly widened a filter is a ray that hits what it was told
				// to ignore, and nothing about the result would say why.
				if (index == spatial::NO_GROUP) {
					luaL_errorL(state, "'%s' is not a registered collision group", group);
				}

				filter.Mask = spatial::CollisionGroups::MaskFor(index);
				return 0;
			}

			luaL_errorL(state, "RaycastParams has no member '%s'", std::string(field).c_str());
		}

		// `workspace:Raycast(origin, direction, params)`
		//
		// **Roblox's signature, including that the direction carries the
		// distance.** `Raycast(origin, direction * 500)` is what an author
		// writes, so the length of the second argument is how far to look — and
		// `core::Ray` requires a *unit* direction, so the split happens here
		// rather than being lost.
		int WorkspaceRaycast(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			const core::Vector3 origin = CheckVector3(state, 2);
			const core::Vector3 travel = CheckVector3(state, 3);

			const float distance = travel.Magnitude();
			if (distance <= 0.0f) {
				// A ray with no direction finds nothing, which is what
				// `core::Ray` says a default one does. Nil rather than an error:
				// a script scaling a direction by a variable that reached zero
				// has a miss, not a bug.
				lua_pushnil(state);
				return 1;
			}

			spatial::LayerMask mask = spatial::LayerMask::All();
			if (!lua_isnoneornil(state, 4)) {
				mask = CheckParams(state, 4).Mask;
			}

			const auto hit =
				physics::Raycast(*context.World, core::Ray{origin, travel / distance}, distance, mask);
			if (!hit.has_value()) {
				// **Nil, not a result with a flag.** `core::RayHit` carries no
				// validity field for the same reason: a flag makes reading the
				// position out of a miss compile and produce a plausible number.
				lua_pushnil(state);
				return 1;
			}

			// A table rather than a userdata. `RaycastResult` is read once and
			// discarded, so a type with a metatable would be an allocation and a
			// lookup for fields a script reads immediately.
			lua_newtable(state);

			PushInstanceValue(state, hit->Owner);
			lua_setfield(state, -2, "Instance");

			*PushVector3(state) = hit->Position;
			lua_setfield(state, -2, "Position");

			*PushVector3(state) = hit->Normal;
			lua_setfield(state, -2, "Normal");

			lua_pushnumber(state, hit->Distance);
			lua_setfield(state, -2, "Distance");

			// The material the part is drawn with, which is what an author
			// checking a surface reaches for.
			if (const auto *visual = context.World->Get<scene::Visual>(hit->Owner); visual != nullptr) {
				PushEnumItem(state, Name("Material"), visual->Material);
				lua_setfield(state, -2, "Material");
			}

			return 1;
		}
	}

	void OpenQueries(lua_State *state) {
		LuauContext &context = ContextOf(state);

		luaL_newmetatable(state, "RaycastParams");
		lua_pushcfunction(state, RaycastParamsIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, RaycastParamsNewIndex, "__newindex");
		lua_setfield(state, -2, "__newindex");
		lua_pushstring(state, "RaycastParams");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "RaycastParams");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);

		lua_newtable(state);
		lua_pushcfunction(state, RaycastParamsNew, "new");
		lua_setfield(state, -2, "new");
		lua_setglobal(state, "RaycastParams");

		// On the Workspace's own method table, because `workspace:Raycast` is
		// where Roblox puts it — a query is against a scene and not against a
		// part, so it must not appear on every instance. `OpenWorkspace`
		// creates that table and `Bindings.hpp` states that this runs after it.
		lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace.methods");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceRaycast, "Raycast", 1);
		lua_setfield(state, -2, "Raycast");
		lua_pop(state, 1);
	}
}
