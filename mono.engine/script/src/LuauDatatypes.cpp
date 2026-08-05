#include "Bindings.hpp"

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/script/Datatypes.hpp>

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
		// operations — push, check, and a metatable — so they are written once
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
		// most real layout code is written — four numbers where two are zero is
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

		// A keypoint arrives as a table rather than as its own userdata.
		//
		// Roblox has `NumberSequenceKeypoint`, and two more userdata types for
		// something an author writes inline once is surface nobody asked for.
		// The table form is what the constructor already has to accept.
		bool ReadNumberKeypoint(lua_State *state, int index, NumberKeypoint &out) {
			if (!lua_istable(state, index)) {
				return false;
			}

			lua_rawgeti(state, index, 1);
			lua_rawgeti(state, index, 2);
			lua_rawgeti(state, index, 3);

			out.Time = static_cast<float>(lua_tonumber(state, -3));
			out.Value = static_cast<float>(lua_tonumber(state, -2));
			out.Envelope = static_cast<float>(luaL_optnumber(state, -1, 0.0));

			lua_pop(state, 3);
			return true;
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
				lua_newtable(state);
				for (uint32_t index = 0; index < value.Count; index++) {
					lua_newtable(state);
					lua_pushnumber(state, value.Keypoints[index].Time);
					lua_rawseti(state, -2, 1);
					lua_pushnumber(state, value.Keypoints[index].Value);
					lua_rawseti(state, -2, 2);
					lua_pushnumber(state, value.Keypoints[index].Envelope);
					lua_rawseti(state, -2, 3);
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				return 1;
			}

			luaL_errorL(state, "NumberSequence has no member '%s'", std::string(field).c_str());
		}

		// `sequence:Evaluate(t)`. Roblox has no such method — an author writes
		// the interpolation by hand every time — and the engine already has the
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
					if (!lua_istable(state, -1)) {
						luaL_errorL(
							state, "ColorSequence.new: keypoint %d is not a {time, Color3} table", index
						);
					}

					lua_rawgeti(state, -1, 1);
					lua_rawgeti(state, -2, 2);

					const ColorKeypoint keypoint{
						static_cast<float>(lua_tonumber(state, -2)), CheckColor3(state, -1)
					};
					lua_pop(state, 3);

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

				lua_newtable(state);
				for (uint32_t index = 0; index < value.Count; index++) {
					lua_newtable(state);
					lua_pushnumber(state, value.Keypoints[index].Time);
					lua_rawseti(state, -2, 1);
					*PushColor3(state) = value.Keypoints[index].Value;
					lua_rawseti(state, -2, 2);
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
			// N+1 — and `TweenInfo.new(2, style, direction)` then failed with
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
		// silently lost — `Ray.Unit` hands back the same thing.

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
		// indexed rather than streamed — `Float(index, salt)` depends on nothing
		// but its arguments — and Roblox's `Random` is a stream. The shim is the
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

			// What `typeof` reads — see `Values.cpp`'s `Install`.
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

	core::EasingStyle EasingStyleOf(core::Name member) {
		static const struct {
			const char *Name;
			core::EasingStyle Style;
		} STYLES[] = {
			{"Linear", EasingStyle::Linear},
			{"Quad", EasingStyle::Quad},
			{"Cubic", EasingStyle::Cubic},
			{"Quart", EasingStyle::Quart},
			{"Quint", EasingStyle::Quint},
			{"Sine", EasingStyle::Sine},
			{"Exponential", EasingStyle::Exponential},
			{"Circular", EasingStyle::Circular},
			{"Back", EasingStyle::Back},
			{"Elastic", EasingStyle::Elastic},
			{"Bounce", EasingStyle::Bounce},
		};

		for (const auto &entry : STYLES) {
			if (member == core::Name(entry.Name)) {
				return entry.Style;
			}
		}
		return EasingStyle::Linear;
	}

	core::Name NameOf(core::EasingStyle style) {
		static const char *NAMES[] = {
			"Linear",
			"Quad",
			"Cubic",
			"Quart",
			"Quint",
			"Sine",
			"Exponential",
			"Circular",
			"Back",
			"Elastic",
			"Bounce",
		};
		return core::Name(NAMES[static_cast<size_t>(style)]);
	}

	core::EasingDirection EasingDirectionOf(core::Name member) {
		if (member == core::Name("In")) {
			return EasingDirection::In;
		}
		if (member == core::Name("InOut")) {
			return EasingDirection::InOut;
		}
		return EasingDirection::Out;
	}

	core::Name NameOf(core::EasingDirection direction) {
		static const char *NAMES[] = {"In", "Out", "InOut"};
		return core::Name(NAMES[static_cast<size_t>(direction)]);
	}

	// The two enums this vocabulary needs.
	//
	// **Here rather than in each VM's open, because there are three callers.**
	// Both surfaces consume them through `TweenInfo`, and `mono.tools/bindings`
	// needs them without opening a VM at all — see `script/Datatypes.hpp` for
	// what having written the list twice actually cost.
	void RegisterDatatypeEnums() {
		static const std::string_view EASING_STYLES[] = {
			"Linear",
			"Quad",
			"Cubic",
			"Quart",
			"Quint",
			"Sine",
			"Exponential",
			"Circular",
			"Back",
			"Elastic",
			"Bounce",
		};
		static const std::string_view EASING_DIRECTIONS[] = {"In", "Out", "InOut"};

		ecs::EnumTable::Register("EasingStyle", EASING_STYLES);
		ecs::EnumTable::Register("EasingDirection", EASING_DIRECTIONS);
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
			{"TweenInfo", TweenInfoIndex, nullptr, nullptr, nullptr, nullptr, nullptr, tweenInfo},
			{"Ray", RayIndex, nullptr, nullptr, nullptr, nullptr, nullptr, ray},
			{"Random", RandomIndex, nullptr, nullptr, nullptr, nullptr, nullptr, random},
		};

		for (const Datatype &type : TYPES) {
			Install(state, type);
		}
	}
}
