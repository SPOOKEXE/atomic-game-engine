// `TweenService` and the `Tween` it hands back, in Luau.
//
// **The service is per language and the tween is not.** `TweenTable` holds what
// a tween is and decides every order a recording depends on; this file is what a
// stack slot, a tagged userdata and a metatable look like on this side, and
// `JsTweenService.cpp` is the same thing said in the other language.
//
// **`GetValue` is the piece worth having first**, and it is why the easing maths
// live in `core::TweenInfo` rather than here: a curve inside a service is
// checked by watching something move, and a pure function of an alpha is checked
// by asserting that `Quad`/`Out` at a half is three quarters.
//
// @tier L9 · shared

#include "Bindings.hpp"

#include <engine/ecs/Classes.hpp>

#include <algorithm>
#include <lua.h>
#include <lualib.h>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::PropertyDescriptor;

		// The world this call's service table was installed against.
		ecs::Store &StoreOfUpvalue(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		// A `TweenInfo` argument.
		//
		// **Its own four lines rather than `LuauDatatypes.cpp`'s `Check<T, TAG>`
		// template**, which is file-local there by design — this is the same
		// shape `CheckSignal` and `CheckConnection` each write for themselves,
		// and the tag is what makes it safe.
		const core::TweenInfo &CheckTweenInfo(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_TWEEN_INFO);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "TweenInfo");
			}
			return *static_cast<const core::TweenInfo *>(value);
		}

		// A `Tween` argument, as the entity that names it.
		Entity CheckTween(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_TWEEN);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Tween");
			}
			return *static_cast<const Entity *>(value);
		}

		// The property a script named, or null when the instance has no such
		// scriptable property.
		//
		// The same rule `Instances.cpp` states for the read path: a
		// non-scriptable property is *not found* rather than found and refused,
		// so the error message does not tell a program what is there to reach
		// for.
		const PropertyDescriptor *FindGoal(const ecs::Store &store, Entity instance, std::string_view name) {
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Spelling == name) {
					return property.Scriptable ? &property : nullptr;
				}
			}
			return nullptr;
		}

		// Reads the goal map, refusing anything a tween cannot drive **by name**.
		//
		// **Every refusal names the property**, because the alternative is a
		// tween that runs for its whole duration and moves nothing — which reads
		// as a broken engine rather than as a scene asking for something that
		// does not mean anything. A `Bool` has no midpoint, `Anchored` is a
		// `Bool`, and saying so is the difference between a minute and an
		// afternoon.
		//
		// @param state  The VM.
		// @param index  The absolute stack index of the goal table.
		// @param store  The world.
		// @param target What the tween drives.
		// @return The goals, sorted by property name.
		std::vector<TweenGoal> ReadGoals(lua_State *state, int index, ecs::Store &store, Entity target) {
			luaL_checktype(state, index, LUA_TTABLE);

			std::vector<TweenGoal> goals;

			lua_pushnil(state);
			while (lua_next(state, index) != 0) {
				// **The key is tested rather than converted.** `lua_tostring` on
				// a number key rewrites the value on the stack, which is exactly
				// what `lua_next` is documented to break on — a goal table with
				// a numeric key would have ended the walk somewhere arbitrary.
				if (lua_type(state, -2) != LUA_TSTRING) {
					luaL_errorL(state, "TweenService:Create: every goal is named by a property");
				}

				const std::string name(lua_tostring(state, -2));
				const PropertyDescriptor *property = FindGoal(store, target, name);
				if (property == nullptr) {
					luaL_errorL(
						state, "TweenService:Create: '%s' is not a property of this instance", name.c_str()
					);
				}
				if (!property->Writable) {
					luaL_errorL(state, "TweenService:Create: '%s' cannot be assigned", name.c_str());
				}
				if (!Interpolable(property->Type)) {
					luaL_errorL(
						state,
						"TweenService:Create: '%s' is a %s, which has no midpoint to interpolate through",
						name.c_str(),
						ecs::Describe(property->Type)
					);
				}

				TweenGoal goal;
				goal.Property = property->Name;
				goal.Type = property->Type;
				goal.Size = property->Size;

				// The value is the top of the stack, and `ReadPropertyValue`
				// wants an absolute index — a relative one would be wrong the
				// moment it pushed anything of its own.
				if (!ReadPropertyValue(
						state, lua_gettop(state), property->Type, property->EnumName, goal.Goal
					)) {
					luaL_errorL(state, "TweenService:Create: could not read the goal for '%s'", name.c_str());
				}

				goals.push_back(goal);
				lua_pop(state, 1);
			}

			// **Sorted by spelling, which is what makes two goals a stated
			// order.** A Luau table is walked in hash order, and two properties
			// of one instance may project onto one component — `Position` and
			// `CFrame` both write `Transform` — so which of them lands last is
			// observable and must not depend on how the keys happened to hash.
			std::sort(goals.begin(), goals.end(), [](const TweenGoal &left, const TweenGoal &right) {
				return left.Property.Text() < right.Property.Text();
			});
			return goals;
		}

		// Drops a tween nothing holds any more: its connections, then its row.
		//
		// **The callables are released here rather than by `TweenTable`**,
		// because only a VM knows what a `CallbackRef` means — the same split
		// `SignalTable::DropSubject` is on.
		void ReleaseTween(lua_State *state, LuauContext &context, Entity tween) {
			std::vector<CallbackRef> released;
			context.Signals.DropSubject(tween, released);
			for (const CallbackRef reference : released) {
				lua_unref(state, reference);
			}
			context.World->Destroy(tween);
		}

		// --- the service ------------------------------------------------------

		// `TweenService:GetValue(alpha, easingStyle, easingDirection)`
		//
		// **Pure, and the only part of this service a scene can use without
		// building anything.** A layout that wants a curve without a target — an
		// emitter's rate, a camera's own easing, a value written into an
		// attribute — reaches for this rather than making a tween to read.
		int GetValue(lua_State *state) {
			const double alpha = luaL_checknumber(state, 2);

			core::Name style;
			if (!ReadEnumValue(state, 3, core::Name("EasingStyle"), style)) {
				luaL_errorL(state, "TweenService:GetValue: expected an Enum.EasingStyle");
			}

			core::Name direction;
			if (!ReadEnumValue(state, 4, core::Name("EasingDirection"), direction)) {
				luaL_errorL(state, "TweenService:GetValue: expected an Enum.EasingDirection");
			}

			// Clamped by `Ease` itself rather than here — past the end a tween is
			// finished, and an elastic curve extrapolated past one grows without
			// bound.
			lua_pushnumber(
				state,
				core::TweenInfo::Ease(
					static_cast<float>(alpha), EasingStyleOf(style), EasingDirectionOf(direction)
				)
			);
			return 1;
		}

		// `TweenService:Create(instance, tweenInfo, goals)` -> Tween
		int Create(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			ecs::Store &store = StoreOfUpvalue(state);

			const Entity target = CheckInstanceArgument(state, 2);
			const core::TweenInfo info = CheckTweenInfo(state, 3);
			std::vector<TweenGoal> goals = ReadGoals(state, 4, store, target);

			std::vector<Entity> dropped;
			const Entity tween = context.Tweens.Create(store, target, info, std::move(goals), dropped);
			for (const Entity stale : dropped) {
				ReleaseTween(state, context, stale);
			}

			// **The cap is named in the refusal**, because the alternative is a
			// tween that was never made answering `Play` with silence. See
			// `TweenTable::MAXIMUM` for why a table of live tweens refuses where
			// `Debris` evicts.
			if (tween == ecs::NULL_ENTITY) {
				luaL_errorL(
					state,
					"TweenService:Create: this world already holds %d running tweens",
					static_cast<int>(TweenTable::MAXIMUM)
				);
			}

			PushTween(state, tween);
			return 1;
		}

		// --- the tween --------------------------------------------------------

		// `tween:Play()`
		//
		// **Answers whether it started**, where Roblox answers nothing. A tween
		// whose target has been destroyed is the one case that can fail, it is an
		// ordinary thing for it to have happened, and a caller that ignores the
		// answer reads exactly as Roblox's does.
		int Play(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Play(*context.World, CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween:Pause()`
		int Pause(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Pause(CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween:Cancel()`
		int Cancel(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Cancel(CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween.Completed`, and the three methods.
		//
		// **A branch for the signal and a table for the rest**, which is the
		// shape `InstanceIndex` uses one door along and for the same reason: a
		// signal is not a member of any table, so it exists only as a string
		// comparison.
		int TweenIndex(lua_State *state) {
			const Entity tween = CheckTween(state, 1);
			const std::string_view field(luaL_checkstring(state, 2));

			if (field == "Completed") {
				PushSignal(state, SignalKind::TweenCompleted, tween);
				return 1;
			}

			lua_getfield(state, LUA_REGISTRYINDEX, "engine.tween.methods");
			lua_pushvalue(state, 2);
			lua_rawget(state, -2);
			return 1;
		}

		int TweenToString(lua_State *state) {
			(void)CheckTween(state, 1);
			lua_pushstring(state, "Tween");
			return 1;
		}

		// Two handles to one tween are equal, which they would not be by
		// identity: `PushTween` builds a fresh userdata per call, exactly as
		// `PushInstanceValue` does.
		int TweenEqual(lua_State *state) {
			void *left = lua_touserdatatagged(state, 1, TAG_TWEEN);
			void *right = lua_touserdatatagged(state, 2, TAG_TWEEN);
			lua_pushboolean(
				state,
				left != nullptr && right != nullptr &&
					*static_cast<Entity *>(left) == *static_cast<Entity *>(right)
			);
			return 1;
		}
	}

	void PushTween(lua_State *state, ecs::Entity tween) {
		void *memory = lua_newuserdatatagged(state, sizeof(ecs::Entity), TAG_TWEEN);
		*static_cast<ecs::Entity *>(memory) = tween;

		luaL_getmetatable(state, "Tween");
		lua_setmetatable(state, -2);
	}

	void OpenTweenService(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// The method table, in the registry so `__index` hands a closure back
		// rather than building one per access — `Instances.cpp` does the same
		// with `engine.instance.methods`, and for the same reason.
		//
		// **A `ServiceMethod` row rather than a type of its own**, because the
		// pair is exactly what one is: a name and a `lua_CFunction` that reads
		// the context off upvalue 1.
		static constexpr ServiceMethod TWEEN_METHODS[] = {
			{"Play", Play},
			{"Pause", Pause},
			{"Cancel", Cancel},
		};

		lua_newtable(state);
		for (const ServiceMethod &method : TWEEN_METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.tween.methods");

		luaL_newmetatable(state, "Tween");
		lua_pushcfunction(state, TweenIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, TweenToString, "__tostring");
		lua_setfield(state, -2, "__tostring");
		lua_pushcfunction(state, TweenEqual, "__eq");
		lua_setfield(state, -2, "__eq");

		// Locked, like every other value type's: a script that could replace
		// `__index` here would replace it for every tween in the world.
		lua_pushstring(state, "Tween");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "Tween");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);

		static constexpr ServiceMethod METHODS[] = {
			{"GetValue", GetValue},
			{"Create", Create},
		};

		ServiceSurface surface;
		surface.Name = "TweenService";
		surface.Methods = METHODS;

		InstallService(state, surface);
	}

	std::string PumpTweens(lua_State *state, float delta) {
		LuauContext &context = ContextOf(state);

		// **Collected and then fired**, rather than fired from inside the walk:
		// a `Completed` handler may cancel the tween it was told about or start
		// another, and `TweenTable::Advance` is walking the list that names it.
		std::vector<ecs::Entity> completed;
		std::vector<ecs::Entity> dropped;
		context.Tweens.Advance(*context.World, delta, completed, dropped);

		std::string firstError;
		for (const ecs::Entity tween : completed) {
			// Every handler runs even when one raises, and the first error is
			// what the host hears about — `FireSignal`'s own rule, one level up.
			const std::string failed = FireSignal(state, SignalKind::TweenCompleted, tween, 0);
			if (firstError.empty()) {
				firstError = failed;
			}
		}

		// After the signals, so a tween dropped because its target died still
		// had whatever it was going to do this tick — and so a handler cannot be
		// called on a subject whose connections have already been released.
		for (const ecs::Entity tween : dropped) {
			ReleaseTween(state, context, tween);
		}
		return firstError;
	}
}
