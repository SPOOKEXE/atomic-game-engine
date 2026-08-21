#include "LuauBindings.hpp"

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/script/Datatypes.hpp>

#include <array>
#include <lualib.h>
#include <new>
#include <string_view>

namespace engine::script {

	namespace {
		using core::AABB;
		using core::ColorKeypoint;
		using core::ColorSequence;
		using core::EasingDirection;
		using core::EasingStyle;
		using core::NumberKeypoint;
		using core::NumberRange;
		using core::NumberSequence;
		using core::Ray;
		using core::Rect;
		using core::UDim;
		using core::UDim2;
		using core::Vector2;

		// One userdata of a plain value type.
		//
		// Every datatype below is trivially copyable and needs the same three
		// operations - push, check, and a metatable - so they are written once
		// here rather than eleven times. Luau's tag is what makes the check
		// safe: `Vector2` and `UDim` are both two floats, so a check on shape
		// would pass for either.
		template <class T, int Tag> T *Push(lua_State *state, const char *metatable) {
			void *memory = lua_newuserdatatagged(state, sizeof(T), Tag);
			auto *value = new (memory) T();

			luaL_getmetatable(state, metatable);
			lua_setmetatable(state, -2);
			return value;
		}

		template <class T, int Tag> T &Check(lua_State *state, int index, const char *name) {
			void *value = lua_touserdatatagged(state, index, Tag);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, name);
			}
			return *static_cast<T *>(value);
		}

		float Number(lua_State *state, int index, double fallback = 0.0) {
			return static_cast<float>(luaL_optnumber(state, index, fallback));
		}

		// --- Vector2 ---------------------------------------------------------

		Vector2 &CheckVector2(lua_State *state, int index) {
			return Check<Vector2, TAG_VECTOR2>(state, index, "Vector2");
		}

		int Vector2New(lua_State *state) {
			*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = Vector2{Number(state, 1), Number(state, 2)};
			return 1;
		}

		int Vector2Index(lua_State *state) {
			const Vector2 &value = CheckVector2(state, 1);
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "X" || field == "x") {
				lua_pushnumber(state, value.X);
				return 1;
			}
			if (field == "Y" || field == "y") {
				lua_pushnumber(state, value.Y);
				return 1;
			}
			if (field == "Magnitude") {
				lua_pushnumber(state, value.Magnitude());
				return 1;
			}
			if (field == "Unit") {
				*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = value.Unit();
				return 1;
			}

			luaL_errorL(state, "Vector2 has no member '%s'", std::string(field).c_str());
		}

		int Vector2Add(lua_State *state) {
			*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = CheckVector2(state, 1) + CheckVector2(state, 2);
			return 1;
		}

		int Vector2Sub(lua_State *state) {
			*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = CheckVector2(state, 1) - CheckVector2(state, 2);
			return 1;
		}

		int Vector2Mul(lua_State *state) {
			// A number on either side, matching Roblox: `2 * v` and `v * 2` are
			// both what an author writes, and Lua hands the metamethod the
			// operands in whichever order they appeared.
			if (lua_isnumber(state, 2)) {
				*Push<Vector2, TAG_VECTOR2>(state, "Vector2") =
					CheckVector2(state, 1) * static_cast<float>(lua_tonumber(state, 2));
				return 1;
			}
			if (lua_isnumber(state, 1)) {
				*Push<Vector2, TAG_VECTOR2>(state, "Vector2") =
					CheckVector2(state, 2) * static_cast<float>(lua_tonumber(state, 1));
				return 1;
			}

			*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = CheckVector2(state, 1) * CheckVector2(state, 2);
			return 1;
		}

		int Vector2Equal(lua_State *state) {
			lua_pushboolean(state, CheckVector2(state, 1) == CheckVector2(state, 2));
			return 1;
		}

		int Vector2ToString(lua_State *state) {
			const Vector2 &value = CheckVector2(state, 1);
			lua_pushfstring(state, "%f, %f", value.X, value.Y);
			return 1;
		}

		// --- UDim / UDim2 ----------------------------------------------------

		int UDimNew(lua_State *state) {
			*Push<UDim, TAG_UDIM>(state, "UDim") = UDim{Number(state, 1), Number(state, 2)};
			return 1;
		}

		int UDimIndex(lua_State *state) {
			const UDim &value = Check<UDim, TAG_UDIM>(state, 1, "UDim");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Scale") {
				lua_pushnumber(state, value.Scale);
				return 1;
			}
			if (field == "Offset") {
				lua_pushnumber(state, value.Offset);
				return 1;
			}

			luaL_errorL(state, "UDim has no member '%s'", std::string(field).c_str());
		}

		int UDim2New(lua_State *state) {
			*Push<UDim2, TAG_UDIM2>(state, "UDim2") =
				UDim2{Number(state, 1), Number(state, 2), Number(state, 3), Number(state, 4)};
			return 1;
		}

		// `UDim2.fromScale(x, y)` and `UDim2.fromOffset(x, y)`, which is how
		// most real layout code is written - four numbers where two are zero is
		// noise an author stops reading.
		int UDim2FromScale(lua_State *state) {
			*Push<UDim2, TAG_UDIM2>(state, "UDim2") = UDim2{Number(state, 1), 0.0f, Number(state, 2), 0.0f};
			return 1;
		}

		int UDim2FromOffset(lua_State *state) {
			*Push<UDim2, TAG_UDIM2>(state, "UDim2") = UDim2{0.0f, Number(state, 1), 0.0f, Number(state, 2)};
			return 1;
		}

		int UDim2Index(lua_State *state) {
			const UDim2 &value = Check<UDim2, TAG_UDIM2>(state, 1, "UDim2");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "X" || field == "Width") {
				*Push<UDim, TAG_UDIM>(state, "UDim") = value.X;
				return 1;
			}
			if (field == "Y" || field == "Height") {
				*Push<UDim, TAG_UDIM>(state, "UDim") = value.Y;
				return 1;
			}

			luaL_errorL(state, "UDim2 has no member '%s'", std::string(field).c_str());
		}

		int UDim2Add(lua_State *state) {
			*Push<UDim2, TAG_UDIM2>(state, "UDim2") =
				Check<UDim2, TAG_UDIM2>(state, 1, "UDim2") + Check<UDim2, TAG_UDIM2>(state, 2, "UDim2");
			return 1;
		}

		int UDim2Sub(lua_State *state) {
			*Push<UDim2, TAG_UDIM2>(state, "UDim2") =
				Check<UDim2, TAG_UDIM2>(state, 1, "UDim2") - Check<UDim2, TAG_UDIM2>(state, 2, "UDim2");
			return 1;
		}

		int UDim2Equal(lua_State *state) {
			lua_pushboolean(
				state,
				Check<UDim2, TAG_UDIM2>(state, 1, "UDim2") == Check<UDim2, TAG_UDIM2>(state, 2, "UDim2")
			);
			return 1;
		}

		int UDimEqual(lua_State *state) {
			lua_pushboolean(
				state, Check<UDim, TAG_UDIM>(state, 1, "UDim") == Check<UDim, TAG_UDIM>(state, 2, "UDim")
			);
			return 1;
		}

		// --- Rect ------------------------------------------------------------

		int RectNew(lua_State *state) {
			// Two corners or four numbers, both of which Roblox accepts.
			const Rect built =
				lua_touserdatatagged(state, 1, TAG_VECTOR2) != nullptr
					? Rect{CheckVector2(state, 1), CheckVector2(state, 2)}
					: Rect{Number(state, 1), Number(state, 2), Number(state, 3), Number(state, 4)};

			*Push<Rect, TAG_RECT>(state, "Rect") = built;
			return 1;
		}

		int RectIndex(lua_State *state) {
			const Rect &value = Check<Rect, TAG_RECT>(state, 1, "Rect");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Min") {
				*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = value.Min;
				return 1;
			}
			if (field == "Max") {
				*Push<Vector2, TAG_VECTOR2>(state, "Vector2") = value.Max;
				return 1;
			}
			if (field == "Width") {
				lua_pushnumber(state, value.Width());
				return 1;
			}
			if (field == "Height") {
				lua_pushnumber(state, value.Height());
				return 1;
			}

			luaL_errorL(state, "Rect has no member '%s'", std::string(field).c_str());
		}

		int RectEqual(lua_State *state) {
			lua_pushboolean(
				state, Check<Rect, TAG_RECT>(state, 1, "Rect") == Check<Rect, TAG_RECT>(state, 2, "Rect")
			);
			return 1;
		}

		// --- Region3 ---------------------------------------------------------
		//
		// **`core::AABB`, not a new type.** Roblox's `Region3` is a box with a
		// `CFrame` and a `Size`; the engine already holds exactly that box under
		// the name every spatial query uses. Adding a second spelling would be
		// the duplicate the root `AGENTS.md` calls the most expensive kind of
		// debt, so the shim converts on the way out instead.

		int Region3New(lua_State *state) {
			const AABB built{CheckVector3(state, 1), CheckVector3(state, 2)};
			*Push<AABB, TAG_REGION3>(state, "Region3") = built;
			return 1;
		}

		int Region3Index(lua_State *state) {
			const AABB &value = Check<AABB, TAG_REGION3>(state, 1, "Region3");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "CFrame") {
				// The centre, with no rotation. An axis-aligned box has none,
				// and Roblox's `Region3.CFrame` is the same identity-rotation
				// frame at the centre.
				*PushCFrame(state) = core::CFrame{value.Centre()};
				return 1;
			}
			if (field == "Size") {
				*PushVector3(state) = value.Size();
				return 1;
			}

			luaL_errorL(state, "Region3 has no member '%s'", std::string(field).c_str());
		}

		// --- NumberRange -----------------------------------------------------

		int NumberRangeNew(lua_State *state) {
			const float minimum = Number(state, 1);
			// One argument is the degenerate range, which is Roblox's shape.
			const float maximum = lua_isnoneornil(state, 2) ? minimum : Number(state, 2);

			const NumberRange built{minimum, maximum};
			*Push<NumberRange, TAG_NUMBER_RANGE>(state, "NumberRange") = built;
			return 1;
		}

		int NumberRangeIndex(lua_State *state) {
			const NumberRange &value = Check<NumberRange, TAG_NUMBER_RANGE>(state, 1, "NumberRange");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Min") {
				lua_pushnumber(state, value.Minimum);
				return 1;
			}
			if (field == "Max") {
				lua_pushnumber(state, value.Maximum);
				return 1;
			}

			luaL_errorL(state, "NumberRange has no member '%s'", std::string(field).c_str());
		}

		// --- sequences -------------------------------------------------------

		// A keypoint arrives as its own userdata, or as a table.
		//
		// **Both forms, and the table one is not deprecated.** Roblox has
		// `NumberSequenceKeypoint`, and until v0.10 this engine deliberately did
		// not - the note here said two more userdata types for a value an author
		// writes inline once is surface nobody asked for, and while a sequence was
		// only ever built from a literal that was true.
		//
		// What made it false is that a sequence became a *property*. An emitter's
		// `Transparency` is read back, and a getter handing out
		// `{time, value, envelope}` gives three anonymous numbers with no
		// `typeof` - so `emitter.Transparency.Keypoints[1].Value` did not work and
		// nothing said why. `TAG_NUMBER_KEYPOINT` carries the rest of the
		// reasoning.
		//
		// The table stays because a literal is still how a gradient is written,
		// and `{{0, 1}, {1, 0}}` is much better to read than two constructor
		// calls. What changed is that it is no longer the *only* form.
		bool ReadNumberKeypoint(lua_State *state, int index, NumberKeypoint &out) {
			if (void *tagged = lua_touserdatatagged(state, index, TAG_NUMBER_KEYPOINT)) {
				out = *static_cast<const NumberKeypoint *>(tagged);
				return true;
			}
			if (!lua_istable(state, index)) {
				return false;
			}

			// **Made absolute before anything is pushed, and this was a real bug
			// rather than a tidy-up.** A negative index is relative to the top *at
			// the moment of the call*, so after the first `lua_rawgeti` grows the
			// stack, `index` of -1 names the value that was just pushed instead of
			// the table it came from - and the second read then indexes a number.
			//
			// It survived because nothing exercised it: every sequence in the
			// suite was built from the two-argument form, and the table form is
			// the one that goes through here. The v0.10 keypoint tests are what
			// found it, by crashing.
			const int table = lua_absindex(state, index);

			lua_rawgeti(state, table, 1);
			lua_rawgeti(state, table, 2);
			lua_rawgeti(state, table, 3);

			out.Time = static_cast<float>(lua_tonumber(state, -3));
			out.Value = static_cast<float>(lua_tonumber(state, -2));
			out.Envelope = static_cast<float>(luaL_optnumber(state, -1, 0.0));

			lua_pop(state, 3);
			return true;
		}

		// The colour twin, and it is a separate function rather than a template
		// because the table layouts differ: a number keypoint's second element is
		// a number and a colour keypoint's is a `Color3` userdata, so the two
		// bodies share no line worth factoring.
		bool ReadColorKeypoint(lua_State *state, int index, ColorKeypoint &out) {
			if (void *tagged = lua_touserdatatagged(state, index, TAG_COLOR_KEYPOINT)) {
				out = *static_cast<const ColorKeypoint *>(tagged);
				return true;
			}
			if (!lua_istable(state, index)) {
				return false;
			}

			// Absolute, for `ReadNumberKeypoint`'s reason one function up.
			const int table = lua_absindex(state, index);

			lua_rawgeti(state, table, 1);
			lua_rawgeti(state, table, 2);

			// Read before the pop, and `CheckColor3` may longjmp out of here -
			// which is fine and is why nothing between these two lines allocates.
			out.Time = static_cast<float>(lua_tonumber(state, -2));
			out.Value = CheckColor3(state, -1);

			lua_pop(state, 2);
			return true;
		}

		// `NumberSequenceKeypoint.new(time, value, envelope)`.
		//
		// The envelope defaults to zero, which is Roblox's default and is also the
		// only value that means "no band" - `Sequence.hpp` declines to sample the
		// band itself because picking a number inside it needs a generator, and
		// `effects::ParticleSystem` is the caller that has one.
		int NumberKeypointNew(lua_State *state) {
			const NumberKeypoint built{Number(state, 1), Number(state, 2), Number(state, 3)};
			*Push<NumberKeypoint, TAG_NUMBER_KEYPOINT>(state, "NumberSequenceKeypoint") = built;
			return 1;
		}

		int NumberKeypointIndex(lua_State *state) {
			const NumberKeypoint &value =
				Check<NumberKeypoint, TAG_NUMBER_KEYPOINT>(state, 1, "NumberSequenceKeypoint");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Time") {
				lua_pushnumber(state, value.Time);
				return 1;
			}
			if (field == "Value") {
				lua_pushnumber(state, value.Value);
				return 1;
			}
			if (field == "Envelope") {
				lua_pushnumber(state, value.Envelope);
				return 1;
			}

			luaL_errorL(state, "NumberSequenceKeypoint has no member '%s'", std::string(field).c_str());
		}

		int NumberKeypointEqual(lua_State *state) {
			lua_pushboolean(
				state,
				Check<NumberKeypoint, TAG_NUMBER_KEYPOINT>(state, 1, "NumberSequenceKeypoint") ==
					Check<NumberKeypoint, TAG_NUMBER_KEYPOINT>(state, 2, "NumberSequenceKeypoint")
			);
			return 1;
		}

		// `ColorSequenceKeypoint.new(time, colour)`. No envelope: Roblox's colour
		// keypoint has none, and `core::ColorKeypoint` has no field for one.
		int ColorKeypointNew(lua_State *state) {
			const ColorKeypoint built{Number(state, 1), CheckColor3(state, 2)};
			*Push<ColorKeypoint, TAG_COLOR_KEYPOINT>(state, "ColorSequenceKeypoint") = built;
			return 1;
		}

		int ColorKeypointIndex(lua_State *state) {
			const ColorKeypoint &value =
				Check<ColorKeypoint, TAG_COLOR_KEYPOINT>(state, 1, "ColorSequenceKeypoint");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Time") {
				lua_pushnumber(state, value.Time);
				return 1;
			}
			if (field == "Value") {
				*PushColor3(state) = value.Value;
				return 1;
			}

			luaL_errorL(state, "ColorSequenceKeypoint has no member '%s'", std::string(field).c_str());
		}

		int ColorKeypointEqual(lua_State *state) {
			lua_pushboolean(
				state,
				Check<ColorKeypoint, TAG_COLOR_KEYPOINT>(state, 1, "ColorSequenceKeypoint") ==
					Check<ColorKeypoint, TAG_COLOR_KEYPOINT>(state, 2, "ColorSequenceKeypoint")
			);
			return 1;
		}

		int NumberSequenceNew(lua_State *state) {
			NumberSequence built;

			// A list of keypoints.
			if (lua_istable(state, 1)) {
				const int count = lua_objlen(state, 1);
				for (int index = 1; index <= count; index++) {
					lua_rawgeti(state, 1, index);

					NumberKeypoint keypoint;
					if (!ReadNumberKeypoint(state, -1, keypoint)) {
						luaL_errorL(
							state, "NumberSequence.new: keypoint %d is not a {time, value} table", index
						);
					}
					lua_pop(state, 1);

					// Refused rather than dropped. A sequence silently missing
					// its last stop is a gradient that is subtly wrong
					// everywhere and obviously wrong nowhere.
					if (!built.Add(keypoint)) {
						luaL_errorL(
							state, "NumberSequence.new: more than %d keypoints", core::SEQUENCE_CAPACITY
						);
					}
				}
			} else {
				const float from = Number(state, 1);
				built =
					lua_isnoneornil(state, 2) ? NumberSequence{from} : NumberSequence{from, Number(state, 2)};
			}

			*Push<NumberSequence, TAG_NUMBER_SEQUENCE>(state, "NumberSequence") = built;
			return 1;
		}

		int NumberSequenceIndex(lua_State *state) {
			const NumberSequence &value =
				Check<NumberSequence, TAG_NUMBER_SEQUENCE>(state, 1, "NumberSequence");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Keypoints") {
				// **Userdata rather than the `{time, value, envelope}` tables this
				// used to hand back.** A read that does not come back in a shape
				// the constructor accepts is not a round trip, and three anonymous
				// numbers were not one: nothing said which was the time, `typeof`
				// answered "table", and a stop from a colour ramp was
				// indistinguishable from a stop from a number ramp. See
				// `TAG_NUMBER_KEYPOINT`.
				lua_newtable(state);
				for (uint32_t index = 0; index < value.Count; index++) {
					*Push<NumberKeypoint, TAG_NUMBER_KEYPOINT>(state, "NumberSequenceKeypoint") =
						value.Keypoints[index];
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				return 1;
			}

			luaL_errorL(state, "NumberSequence has no member '%s'", std::string(field).c_str());
		}

		// `sequence:Evaluate(t)`. Roblox has no such method - an author writes
		// the interpolation by hand every time - and the engine already has the
		// exact function, so not exposing it would be hiding the useful half.
		int NumberSequenceEvaluate(lua_State *state) {
			const NumberSequence &value =
				Check<NumberSequence, TAG_NUMBER_SEQUENCE>(state, 1, "NumberSequence");
			lua_pushnumber(state, value.Evaluate(static_cast<float>(luaL_checknumber(state, 2))));
			return 1;
		}

		int ColorSequenceNew(lua_State *state) {
			ColorSequence built;

			if (lua_istable(state, 1)) {
				const int count = lua_objlen(state, 1);
				for (int index = 1; index <= count; index++) {
					lua_rawgeti(state, 1, index);

					ColorKeypoint keypoint;
					if (!ReadColorKeypoint(state, -1, keypoint)) {
						luaL_errorL(
							state,
							"ColorSequence.new: keypoint %d is not a ColorSequenceKeypoint or a "
							"{time, Color3} table",
							index
						);
					}
					lua_pop(state, 1);

					if (!built.Add(keypoint)) {
						luaL_errorL(
							state, "ColorSequence.new: more than %d keypoints", core::SEQUENCE_CAPACITY
						);
					}
				}
			} else {
				const core::Color3 from = CheckColor3(state, 1);
				built = lua_isnoneornil(state, 2) ? ColorSequence{from}
												  : ColorSequence{from, CheckColor3(state, 2)};
			}

			*Push<ColorSequence, TAG_COLOR_SEQUENCE>(state, "ColorSequence") = built;
			return 1;
		}

		int ColorSequenceEvaluate(lua_State *state) {
			const ColorSequence &value = Check<ColorSequence, TAG_COLOR_SEQUENCE>(state, 1, "ColorSequence");
			*PushColor3(state) = value.Evaluate(static_cast<float>(luaL_checknumber(state, 2)));
			return 1;
		}

		int SequenceIndexDispatch(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Evaluate") {
				lua_pushcfunction(state, ColorSequenceEvaluate, "Evaluate");
				return 1;
			}
			if (field == "Keypoints") {
				const ColorSequence &value =
					Check<ColorSequence, TAG_COLOR_SEQUENCE>(state, 1, "ColorSequence");

				// Userdata, for `NumberSequenceIndex`'s reason.
				lua_newtable(state);
				for (uint32_t index = 0; index < value.Count; index++) {
					*Push<ColorKeypoint, TAG_COLOR_KEYPOINT>(state, "ColorSequenceKeypoint") =
						value.Keypoints[index];
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				return 1;
			}

			luaL_errorL(state, "ColorSequence has no member '%s'", std::string(field).c_str());
		}

		int NumberSequenceIndexDispatch(lua_State *state) {
			if (std::string_view(luaL_checkstring(state, 2)) == "Evaluate") {
				lua_pushcfunction(state, NumberSequenceEvaluate, "Evaluate");
				return 1;
			}
			return NumberSequenceIndex(state);
		}

		// --- TweenInfo -------------------------------------------------------

		int TweenInfoNew(lua_State *state) {
			// **Every argument read before the result is pushed.** Pushing first
			// puts the new userdata on top of the stack, so it becomes argument
			// N+1 - and `TweenInfo.new(2, style, direction)` then failed with
			// "invalid argument #4 (number expected, got TweenInfo)", naming the
			// value it had just created. Every constructor below reads first for
			// the same reason.
			core::TweenInfo built;
			built.Time = static_cast<float>(luaL_optnumber(state, 1, 1.0));

			// The style and direction arrive as `EnumItem`s or as strings, for
			// the reason an `Enum` property accepts both.
			core::Name style;
			if (!lua_isnoneornil(state, 2) && ReadEnumValue(state, 2, core::Name("EasingStyle"), style)) {
				built.Style = EasingStyleOf(style);
			}

			core::Name direction;
			if (!lua_isnoneornil(state, 3) &&
				ReadEnumValue(state, 3, core::Name("EasingDirection"), direction)) {
				built.Direction = EasingDirectionOf(direction);
			}

			built.RepeatCount = static_cast<int32_t>(luaL_optinteger(state, 4, 0));
			built.Reverses = lua_toboolean(state, 5) != 0;
			built.DelayTime = static_cast<float>(luaL_optnumber(state, 6, 0.0));

			*Push<core::TweenInfo, TAG_TWEEN_INFO>(state, "TweenInfo") = built;
			return 1;
		}

		int TweenInfoEvaluate(lua_State *state) {
			const core::TweenInfo &info = Check<core::TweenInfo, TAG_TWEEN_INFO>(state, 1, "TweenInfo");
			lua_pushnumber(state, info.Evaluate(static_cast<float>(luaL_checknumber(state, 2))));
			return 1;
		}

		int TweenInfoIndex(lua_State *state) {
			const core::TweenInfo &value = Check<core::TweenInfo, TAG_TWEEN_INFO>(state, 1, "TweenInfo");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Time") {
				lua_pushnumber(state, value.Time);
				return 1;
			}
			if (field == "DelayTime") {
				lua_pushnumber(state, value.DelayTime);
				return 1;
			}
			if (field == "RepeatCount") {
				lua_pushinteger(state, value.RepeatCount);
				return 1;
			}
			if (field == "Reverses") {
				lua_pushboolean(state, value.Reverses);
				return 1;
			}
			if (field == "EasingStyle") {
				PushEnumItem(state, core::Name("EasingStyle"), NameOf(value.Style));
				return 1;
			}
			if (field == "EasingDirection") {
				PushEnumItem(state, core::Name("EasingDirection"), NameOf(value.Direction));
				return 1;
			}
			if (field == "Evaluate") {
				lua_pushcfunction(state, TweenInfoEvaluate, "Evaluate");
				return 1;
			}

			luaL_errorL(state, "TweenInfo has no member '%s'", std::string(field).c_str());
		}

		// --- Ray -------------------------------------------------------------
		//
		// `core::Ray`, which the engine already had. The one difference worth
		// naming: `core::Ray::Direction` **must be unit length** and Roblox's
		// need not be, so the constructor normalises and the length is not
		// silently lost - `Ray.Unit` hands back the same thing.

		int RayNew(lua_State *state) {
			const Ray built{CheckVector3(state, 1), CheckVector3(state, 2).Unit()};
			*Push<Ray, TAG_RAY>(state, "Ray") = built;
			return 1;
		}

		int RayPointAt(lua_State *state) {
			const Ray &ray = Check<Ray, TAG_RAY>(state, 1, "Ray");
			*PushVector3(state) = ray.PointAt(static_cast<float>(luaL_checknumber(state, 2)));
			return 1;
		}

		int RayIndex(lua_State *state) {
			const Ray &value = Check<Ray, TAG_RAY>(state, 1, "Ray");
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Origin") {
				*PushVector3(state) = value.Origin;
				return 1;
			}
			if (field == "Direction") {
				*PushVector3(state) = value.Direction;
				return 1;
			}
			if (field == "Unit") {
				*Push<Ray, TAG_RAY>(state, "Ray") = value;
				return 1;
			}
			if (field == "PointAt") {
				lua_pushcfunction(state, RayPointAt, "PointAt");
				return 1;
			}

			luaL_errorL(state, "Ray has no member '%s'", std::string(field).c_str());
		}

		// --- Random ----------------------------------------------------------
		//
		// **The consumer `D00004` has been waiting for.** `core::Random` is
		// indexed rather than streamed - `Float(index, salt)` depends on nothing
		// but its arguments - and Roblox's `Random` is a stream. The shim is the
		// counter: the seed is the salt and the draw number is the index, so a
		// script's sequence is a pure function of its seed and how many values
		// it has taken. Two runs agree, and a recording replays.

		struct RandomStream {
			uint32_t Seed = 0;
			uint32_t Drawn = 0;
		};

		RandomStream &CheckRandom(lua_State *state, int index) {
			return Check<RandomStream, TAG_RANDOM>(state, index, "Random");
		}

		int RandomNew(lua_State *state) {
			const auto seed = static_cast<uint32_t>(luaL_optinteger(state, 1, 0));

			auto *stream = Push<RandomStream, TAG_RANDOM>(state, "Random");
			stream->Seed = seed;
			return 1;
		}

		int RandomNextNumber(lua_State *state) {
			RandomStream &stream = CheckRandom(state, 1);
			const float value = core::Random::Float(stream.Drawn++, stream.Seed);

			if (lua_isnoneornil(state, 2)) {
				lua_pushnumber(state, value);
				return 1;
			}

			const auto minimum = static_cast<float>(luaL_checknumber(state, 2));
			const auto maximum = static_cast<float>(luaL_checknumber(state, 3));
			lua_pushnumber(state, minimum + value * (maximum - minimum));
			return 1;
		}

		int RandomNextInteger(lua_State *state) {
			RandomStream &stream = CheckRandom(state, 1);
			const auto minimum = static_cast<int64_t>(luaL_checkinteger(state, 2));
			const auto maximum = static_cast<int64_t>(luaL_checkinteger(state, 3));

			if (maximum < minimum) {
				luaL_errorL(state, "Random:NextInteger: the maximum is below the minimum");
			}

			// **Inclusive of both ends**, which is Roblox's contract. `Float` is
			// half-open, so the span is `max - min + 1` and the result cannot
			// reach `max + 1`.
			const uint64_t span = static_cast<uint64_t>(maximum - minimum) + 1;
			const float value = core::Random::Float(stream.Drawn++, stream.Seed);
			const auto offset = static_cast<uint64_t>(static_cast<double>(value) * static_cast<double>(span));

			lua_pushinteger(
				state, static_cast<int>(minimum + static_cast<int64_t>(offset < span ? offset : span - 1))
			);
			return 1;
		}

		int RandomIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "NextNumber") {
				lua_pushcfunction(state, RandomNextNumber, "NextNumber");
				return 1;
			}
			if (field == "NextInteger") {
				lua_pushcfunction(state, RandomNextInteger, "NextInteger");
				return 1;
			}

			luaL_errorL(state, "Random has no member '%s'", std::string(field).c_str());
		}

		// One datatype: a metatable and a global constructor table.
		struct Datatype {
			const char *Name;
			lua_CFunction Index;
			lua_CFunction ToString;
			lua_CFunction Equal;
			lua_CFunction Add;
			lua_CFunction Sub;
			lua_CFunction Mul;
			const luaL_Reg *Constructors;
		};

		void Install(lua_State *state, const Datatype &type) {
			luaL_newmetatable(state, type.Name);

			lua_pushcfunction(state, type.Index, "__index");
			lua_setfield(state, -2, "__index");

			// What `typeof` reads - see `LuauValues.cpp`'s `Install`.
			lua_pushstring(state, type.Name);
			lua_setfield(state, -2, "__type");

			const struct {
				const char *Field;
				lua_CFunction Function;
			} METAMETHODS[] = {
				{"__tostring", type.ToString},
				{"__eq", type.Equal},
				{"__add", type.Add},
				{"__sub", type.Sub},
				{"__mul", type.Mul},
			};

			for (const auto &entry : METAMETHODS) {
				if (entry.Function != nullptr) {
					lua_pushcfunction(state, entry.Function, entry.Field);
					lua_setfield(state, -2, entry.Field);
				}
			}

			// Hidden, for the reason every other value type hides its own: a
			// metatable a script can reach is one it can rewrite, and then every
			// value of that type changes underneath everything holding one.
			lua_pushstring(state, type.Name);
			lua_setfield(state, -2, "__metatable");
			lua_pop(state, 1);

			lua_newtable(state);
			luaL_register(state, nullptr, type.Constructors);
			lua_setglobal(state, type.Name);
		}
	}

	namespace {
		// One member of `Enum.Axis`: the name a script spells and the direction
		// it means.
		struct AxisEntry {
			std::string_view Name;
			core::Vector3 Direction;
		};

		// The members of `Enum.Axis`, in Roblox's ordinal order.
		//
		// **One array rather than a member list here and a switch over there.**
		// The registration below and `DirectionOfAxis` are two readings of the
		// same three facts, and the second copy is always the one that goes
		// stale. Function-local, so it is built on first use and no other
		// translation unit's start-up can reach it before `Vector3::XAxis` has
		// a value.
		const std::array<AxisEntry, 3> &Axes() {
			static const std::array<AxisEntry, 3> AXES{{
				{"X", core::Vector3::XAxis},
				{"Y", core::Vector3::YAxis},
				{"Z", core::Vector3::ZAxis},
			}};
			return AXES;
		}
	}

	// The three enums this vocabulary needs.
	//
	// **Here rather than in each VM's open, because there are three callers.**
	// Both surfaces consume them through `TweenInfo`, and `mono.tools/bindings`
	// needs them without opening a VM at all - see `script/Datatypes.hpp` for
	// what having written the list twice actually cost.
	//
	// `Axis` is here for the same reason and one more: `Vector3.FromAxis` is the
	// only thing that names it, and no world registers it - so a script in a
	// process that never built a scene would otherwise be told `Axis` is not an
	// enum this engine registers.
	void RegisterDatatypeEnums() {
		// **Walked off the enum rather than typed out**, which is the rule the
		// comment above is about and which this pair broke: `gui` registers the
		// same two sets for `UIPageLayout`, and two literal lists would be two
		// orderings that agree until somebody adds a curve to one of them. The
		// ordinal is the storage, so the order is not a detail.
		std::array<std::string_view, core::EASING_STYLE_COUNT> styles{};
		for (size_t index = 0; index < styles.size(); index++) {
			styles[index] = core::Describe(static_cast<core::EasingStyle>(index));
		}

		std::array<std::string_view, core::EASING_DIRECTION_COUNT> directions{};
		for (size_t index = 0; index < directions.size(); index++) {
			directions[index] = core::Describe(static_cast<core::EasingDirection>(index));
		}

		std::array<std::string_view, 3> axes{};
		for (size_t index = 0; index < axes.size(); index++) {
			axes[index] = Axes()[index].Name;
		}

		ecs::EnumTable::Register("EasingStyle", styles);
		ecs::EnumTable::Register("EasingDirection", directions);
		ecs::EnumTable::Register("Axis", axes);
	}

	bool DirectionOfNormalId(core::Name member, core::Vector3 &out) {
		size_t ordinal = 0;
		if (!ecs::EnumTable::OrdinalOf(core::Name("NormalId"), member, ordinal)) {
			return false;
		}

		// A game may register a member of its own onto any enum, and
		// `scene::NormalOf` has six faces and a fallback rather than an answer
		// for a seventh. Refusing here says so; falling through would hand back
		// `Front` for a name that means nothing.
		if (ordinal > static_cast<size_t>(scene::NormalId::Front)) {
			return false;
		}

		out = scene::NormalOf(static_cast<scene::NormalId>(ordinal));
		return true;
	}

	bool DirectionOfAxis(core::Name member, core::Vector3 &out) {
		for (const AxisEntry &axis : Axes()) {
			// The text rather than a `core::Name` built from it: interning takes
			// the registry's mutex, and three of them per call to answer a
			// question a string comparison already answers.
			if (member.Text() == axis.Name) {
				out = axis.Direction;
				return true;
			}
		}
		return false;
	}

	void OpenDatatypes(lua_State *state) {
		RegisterDatatypeEnums();

		static const luaL_Reg vector2[] = {{"new", Vector2New}, {nullptr, nullptr}};
		static const luaL_Reg udim[] = {{"new", UDimNew}, {nullptr, nullptr}};
		static const luaL_Reg udim2[] = {
			{"new", UDim2New},
			{"fromScale", UDim2FromScale},
			{"fromOffset", UDim2FromOffset},
			{nullptr, nullptr}
		};
		static const luaL_Reg rect[] = {{"new", RectNew}, {nullptr, nullptr}};
		static const luaL_Reg region3[] = {{"new", Region3New}, {nullptr, nullptr}};
		static const luaL_Reg numberRange[] = {{"new", NumberRangeNew}, {nullptr, nullptr}};
		static const luaL_Reg numberSequence[] = {{"new", NumberSequenceNew}, {nullptr, nullptr}};
		static const luaL_Reg colorSequence[] = {{"new", ColorSequenceNew}, {nullptr, nullptr}};
		static const luaL_Reg numberKeypoint[] = {{"new", NumberKeypointNew}, {nullptr, nullptr}};
		static const luaL_Reg colorKeypoint[] = {{"new", ColorKeypointNew}, {nullptr, nullptr}};
		static const luaL_Reg tweenInfo[] = {{"new", TweenInfoNew}, {nullptr, nullptr}};
		static const luaL_Reg ray[] = {{"new", RayNew}, {nullptr, nullptr}};
		static const luaL_Reg random[] = {{"new", RandomNew}, {nullptr, nullptr}};

		static const Datatype TYPES[] = {
			{"Vector2",
			 Vector2Index,
			 Vector2ToString,
			 Vector2Equal,
			 Vector2Add,
			 Vector2Sub,
			 Vector2Mul,
			 vector2},
			{"UDim", UDimIndex, nullptr, UDimEqual, nullptr, nullptr, nullptr, udim},
			{"UDim2", UDim2Index, nullptr, UDim2Equal, UDim2Add, UDim2Sub, nullptr, udim2},
			{"Rect", RectIndex, nullptr, RectEqual, nullptr, nullptr, nullptr, rect},
			{"Region3", Region3Index, nullptr, nullptr, nullptr, nullptr, nullptr, region3},
			{"NumberRange", NumberRangeIndex, nullptr, nullptr, nullptr, nullptr, nullptr, numberRange},
			{"NumberSequence",
			 NumberSequenceIndexDispatch,
			 nullptr,
			 nullptr,
			 nullptr,
			 nullptr,
			 nullptr,
			 numberSequence},
			{"ColorSequence",
			 SequenceIndexDispatch,
			 nullptr,
			 nullptr,
			 nullptr,
			 nullptr,
			 nullptr,
			 colorSequence},
			// **Registered after the sequences they belong to, which is only
			// legibility - `Install` is order-independent.** Equality is bound on
			// both because comparing two stops is what a test of a gradient does,
			// and without it `==` compares userdata addresses and is false for two
			// keypoints holding identical numbers.
			{"NumberSequenceKeypoint",
			 NumberKeypointIndex,
			 nullptr,
			 NumberKeypointEqual,
			 nullptr,
			 nullptr,
			 nullptr,
			 numberKeypoint},
			{"ColorSequenceKeypoint",
			 ColorKeypointIndex,
			 nullptr,
			 ColorKeypointEqual,
			 nullptr,
			 nullptr,
			 nullptr,
			 colorKeypoint},
			{"TweenInfo", TweenInfoIndex, nullptr, nullptr, nullptr, nullptr, nullptr, tweenInfo},
			{"Ray", RayIndex, nullptr, nullptr, nullptr, nullptr, nullptr, ray},
			{"Random", RandomIndex, nullptr, nullptr, nullptr, nullptr, nullptr, random},
		};

		for (const Datatype &type : TYPES) {
			Install(state, type);
		}
	}
}
