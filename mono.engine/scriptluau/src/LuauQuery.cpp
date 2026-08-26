#include "LuauBindings.hpp"

#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
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
		// `FilterDescendantsInstances` list - Roblox's - would be a per-ray
		// walk of a subtree, and the engine has a bit test that answers the same
		// question in one instruction.
		//
		// So the field is `CollisionGroup` and the omission is stated rather
		// than silent: an author who wants "everything except that model" gives
		// the model a group.
		//
		// **One type for every query, still called `RaycastParams`.** Roblox has
		// a second name - `OverlapParams` - because its two types carry
		// different fields; ours carry the same two, so a second name would be a
		// second thing to keep in step and nothing else. The name is historical
		// and the type filters every query on the workspace.
		struct RaycastFilter {
			spatial::LayerMask Mask = spatial::LayerMask::All();

			// The most entities an overlap or a cast will report.
			//
			// **A cap is not optional here, unlike in Roblox.** `physics::
			// OverlapBox` and friends write into a span the caller owns, so
			// somebody has to choose its size, and a script cannot be handed an
			// unbounded one - a `GetPartBoundsInRadius` over a stress scene
			// would allocate a table of half a million entries inside a tick.
			//
			// A result that comes back exactly this long may have been
			// truncated. That is the author's own instruction rather than a
			// silent loss, which is why the field is theirs to set.
			int MaxParts = 1024;
		};

		// The most a single query may be asked for, whatever `MaxParts` says.
		//
		// A script that writes `params.MaxParts = 1e9` is asking for four
		// gigabytes of entities inside a tick. Clamped rather than refused: the
		// number is a budget, not a promise, and the honest reading of "as many
		// as possible" is "as many as this engine will do in one call".
		constexpr int MAX_QUERY_RESULTS = 65536;

		RaycastFilter &CheckParams(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_RAYCAST_PARAMS);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "RaycastParams");
			}
			return *static_cast<RaycastFilter *>(value);
		}

		// The filter an optional trailing argument names, or the default one.
		RaycastFilter FilterAt(lua_State *state, int index) {
			return lua_isnoneornil(state, index) ? RaycastFilter{} : CheckParams(state, index);
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

			if (field == "MaxParts") {
				lua_pushinteger(state, filter.MaxParts);
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

			if (field == "MaxParts") {
				const int wanted = static_cast<int>(luaL_checkinteger(state, 3));

				// **Refused rather than clamped to one.** Zero or less is not a
				// smaller query, it is a query that can never report anything,
				// and a script that computed one has a bug that a silently
				// empty result would hide behind an ordinary miss.
				if (wanted <= 0) {
					luaL_errorL(state, "MaxParts must be at least 1, not %d", wanted);
				}
				filter.MaxParts = std::min(wanted, MAX_QUERY_RESULTS);
				return 0;
			}

			luaL_errorL(state, "RaycastParams has no member '%s'", std::string(field).c_str());
		}

		// One hit, as the table a script reads.
		//
		// **A table rather than a userdata.** A `RaycastResult` is read once and
		// discarded, so a type with a metatable would be an allocation and a
		// lookup for fields a script reads immediately.
		//
		// Shared by `Raycast` and `RaycastThroughPortals`, because a hit is a
		// hit: two builders would be two shapes for one documented result, and
		// they would stop agreeing the first time a field was added to one.
		void PushHit(lua_State *state, const ecs::Store &world, const physics::ColliderHit &hit) {
			lua_newtable(state);

			PushInstanceValue(state, hit.Owner);
			lua_setfield(state, -2, "Instance");

			*PushVector3(state) = hit.Position;
			lua_setfield(state, -2, "Position");

			*PushVector3(state) = hit.Normal;
			lua_setfield(state, -2, "Normal");

			lua_pushnumber(state, hit.Distance);
			lua_setfield(state, -2, "Distance");

			// **What the part is made of, as a `Surface` name rather than as an
			// enum member.** Roblox's `RaycastResult.Material` is the *visual*
			// material, and that is no longer a field on a part: v0.10 replaced
			// the seventeen-name enum with a `Material` instance naming a
			// published asset - `scene/Materials.hpp` - and resolving one from
			// here would mean a child walk inside a query result.
			//
			// `Surface::Material` is the better answer for this caller anyway. A
			// raycast is a contact query, this names the row a contact reads its
			// friction and restitution out of, and "what did I hit and what is it
			// like to touch" is the question an author asks a hit result. A part
			// nobody gave a surface reads back an empty string, which is the same
			// nothing an unregistered name has always meant.
			if (const auto *surface = world.Get<scene::Surface>(hit.Owner); surface != nullptr) {
				// **The empty view's pointer is null, and Luau asserts on one.**
				// An invalid `core::Name` reads back a default `string_view`, so
				// pushing `.data()` unchecked traps inside the VM rather than
				// pushing an empty string - a part with no surface is the common
				// case, not the edge one.
				const std::string_view material = surface->Material.Text();
				lua_pushlstring(state, material.empty() ? "" : material.data(), material.size());
				lua_setfield(state, -2, "Material");
			}
		}

		// The entities an overlap or a cast found, as a one-based array.
		//
		// **An array and not a set of hits.** `physics::OverlapBox` and the two
		// casts answer "which colliders are in here", not "where and with what
		// normal" - there is no contact point for a volume test - so a result
		// carrying `Position` and `Normal` fields would be a result with two of
		// them invented.
		void PushFound(lua_State *state, const std::vector<Entity> &found, size_t written) {
			lua_createtable(state, static_cast<int>(written), 0);
			for (size_t index = 0; index < written; index++) {
				PushInstanceValue(state, found[index]);
				lua_rawseti(state, -2, static_cast<int>(index) + 1);
			}
		}

		// Origin and travel, split into the unit ray the engine wants.
		//
		// Returns false for a direction of no length, which every caster answers
		// as a miss rather than an error: a script scaling a direction by a
		// variable that reached zero has found nothing, not made a mistake.
		bool RayFrom(lua_State *state, core::Ray &ray, float &distance) {
			const core::Vector3 origin = CheckVector3(state, 2);
			const core::Vector3 travel = CheckVector3(state, 3);

			distance = travel.Magnitude();
			if (distance <= 0.0f) {
				return false;
			}
			ray = core::Ray{origin, travel / distance};
			return true;
		}

		// `workspace:Raycast(origin, direction, params)`
		//
		// **Roblox's signature, including that the direction carries the
		// distance.** `Raycast(origin, direction * 500)` is what an author
		// writes, so the length of the second argument is how far to look - and
		// `core::Ray` requires a *unit* direction, so the split happens here
		// rather than being lost.
		int WorkspaceRaycast(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			core::Ray ray;
			float distance = 0.0f;
			if (!RayFrom(state, ray, distance)) {
				// A ray with no direction finds nothing, which is what
				// `core::Ray` says a default one does. Nil rather than an error:
				// a script scaling a direction by a variable that reached zero
				// has a miss, not a bug.
				lua_pushnil(state);
				return 1;
			}

			const auto hit = physics::Raycast(*context.World, ray, distance, FilterAt(state, 4).Mask);
			if (!hit.has_value()) {
				// **Nil, not a result with a flag.** `core::RayHit` carries no
				// validity field for the same reason: a flag makes reading the
				// position out of a miss compile and produce a plausible number.
				lua_pushnil(state);
				return 1;
			}

			PushHit(state, *context.World, *hit);
			return 1;
		}

		// `workspace:RaycastThroughPortals(origin, direction, params)`
		//
		// **The one query with no Roblox name**, because Roblox has no seams. It
		// carries whatever the ray has left out of the far side of a pane, which
		// is what a character standing in a doorway needs to find the floor it is
		// visibly on. `physics::RaycastThroughPortals` carries the whole of why.
		//
		// The distance comes back measured from the *original* origin, so a
		// script comparing against its own reach needs to know nothing about the
		// hole it went through.
		int WorkspaceRaycastThroughPortals(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			core::Ray ray;
			float distance = 0.0f;
			if (!RayFrom(state, ray, distance)) {
				lua_pushnil(state);
				return 1;
			}

			// **The mutable store, where `Raycast` above takes it const.** The
			// portal walk resolves the far pane through the scene, which is a
			// non-const path, and that is a real difference between the two
			// rather than an oversight in one of them.
			const auto hit =
				physics::RaycastThroughPortals(*context.World, ray, distance, FilterAt(state, 4).Mask);
			if (!hit.has_value()) {
				lua_pushnil(state);
				return 1;
			}

			PushHit(state, *context.World, *hit);
			return 1;
		}

		// `workspace:OverlapBox(centre, size, params)`
		//
		// **Axis-aligned, and the signature says so by taking a `Vector3` where
		// Roblox's `GetPartBoundsInBox` takes a `CFrame`.** `physics::OverlapBox`
		// tests an `core::AABB`; accepting a rotated frame and dropping its
		// rotation would be a query that reads as oriented and answers as if it
		// were not, which is the kind of wrong that never reports itself. An
		// author who wants an oriented box has `BlockCast` with zero motion.
		//
		// `size` is the full extent, as a part's `Size` is - not a half-extent.
		int WorkspaceOverlapBox(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			const core::Vector3 centre = CheckVector3(state, 2);
			const core::Vector3 size = CheckVector3(state, 3);
			const RaycastFilter filter = FilterAt(state, 4);

			const core::Vector3 half = size * 0.5f;
			const core::AABB box{centre - half, centre + half};

			std::vector<Entity> found(static_cast<size_t>(filter.MaxParts));
			const spatial::QueryResult result = physics::OverlapBox(*context.World, box, filter.Mask, found);

			PushFound(state, found, result.Written);
			return 1;
		}

		// `workspace:OverlapSphere(centre, radius, params)`
		//
		// Roblox's `GetPartBoundsInRadius`, and the name is the engine's because
		// this is against the exact shape rather than its bound - which is the
		// difference `physics::OverlapSphere` documents against the `spatial::`
		// one, and the difference a Roblox name would have hidden.
		int WorkspaceOverlapSphere(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			const core::Vector3 centre = CheckVector3(state, 2);
			const auto radius = static_cast<float>(luaL_checknumber(state, 3));
			const RaycastFilter filter = FilterAt(state, 4);

			std::vector<Entity> found(static_cast<size_t>(filter.MaxParts));
			const spatial::QueryResult result =
				physics::OverlapSphere(*context.World, centre, radius, filter.Mask, found);

			PushFound(state, found, result.Written);
			return 1;
		}

		// The two casts, which are one engine function with two shapes.
		//
		// **Two entry points rather than one taking a shape descriptor**, which
		// is the split Roblox makes for the same reason: a script has a size or
		// it has a radius, and a type that carried either would be a type an
		// author has to build before asking a question.
		//
		// `physics::ShapeCast` is conservative and says so - it can over-report a
		// collider whose bound the sweep meets and whose shape it does not, and
		// it never under-reports. A caller wanting the moment of contact steps
		// the shape and uses this to bound the search.
		int ShapeCastWith(lua_State *state, scene::ShapeKind kind) {
			LuauContext &context = UpvalueContext(state);

			scene::Collider shape;
			shape.Shape = kind;
			core::CFrame from;
			int motionAt = 0;

			if (kind == scene::ShapeKind::Box) {
				from = CheckCFrame(state, 2);
				shape.Extent = CheckVector3(state, 3) * 0.5f;
				motionAt = 4;
			} else {
				// A sphere has no orientation to give, so the caller passes a
				// position and this builds the frame. Taking a `CFrame` here
				// would ask for a rotation nothing reads.
				from = core::CFrame{CheckVector3(state, 2)};
				const auto radius = static_cast<float>(luaL_checknumber(state, 3));
				shape.Extent = core::Vector3{radius, radius, radius};
				motionAt = 4;
			}

			const core::Vector3 motion = CheckVector3(state, motionAt);
			const RaycastFilter filter = FilterAt(state, motionAt + 1);

			std::vector<Entity> found(static_cast<size_t>(filter.MaxParts));
			const spatial::QueryResult result =
				physics::ShapeCast(*context.World, shape, from, motion, filter.Mask, found);

			PushFound(state, found, result.Written);
			return 1;
		}

		// `workspace:BlockCast(cframe, size, motion, params)` - Roblox's `Blockcast`.
		int WorkspaceBlockCast(lua_State *state) {
			return ShapeCastWith(state, scene::ShapeKind::Box);
		}

		// `workspace:SphereCast(position, radius, motion, params)` - Roblox's `Spherecast`.
		int WorkspaceSphereCast(lua_State *state) {
			return ShapeCastWith(state, scene::ShapeKind::Sphere);
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
		// where Roblox puts it - a query is against a scene and not against a
		// part, so it must not appear on every instance. `OpenWorkspace`
		// creates that table and `LuauBindings.hpp` states that this runs after it.
		lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace.methods");

		// One table, six entries, one loop. Written out as a table rather than
		// six copies of the same four lines so that a seventh query is a row
		// here and cannot be the one that forgets its upvalue - which is the
		// mistake that makes a method throw "attempt to index nil" a long way
		// from this file.
		const struct {
			const char *Name;
			lua_CFunction Function;
		} methods[] = {
			{"Raycast", WorkspaceRaycast},
			{"RaycastThroughPortals", WorkspaceRaycastThroughPortals},
			{"OverlapBox", WorkspaceOverlapBox},
			{"OverlapSphere", WorkspaceOverlapSphere},
			{"BlockCast", WorkspaceBlockCast},
			{"SphereCast", WorkspaceSphereCast},
		};

		for (const auto &method : methods) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}

		lua_pop(state, 1);
	}
}
