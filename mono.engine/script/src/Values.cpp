#include "Bindings.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cmath>
#include <cstring>
#include <lualib.h>
#include <numbers>

namespace engine::script {

	namespace {
		using core::CFrame;
		using core::Color3;
		using core::Vector3;

		constexpr float RADIANS_PER_DEGREE = std::numbers::pi_v<float> / 180.0f;

		// --- Vector3 ---------------------------------------------------------

		int Vector3New(lua_State *state) {
			const auto x = static_cast<float>(luaL_optnumber(state, 1, 0.0));
			const auto y = static_cast<float>(luaL_optnumber(state, 2, 0.0));
			const auto z = static_cast<float>(luaL_optnumber(state, 3, 0.0));

			*PushVector3(state) = Vector3{x, y, z};
			return 1;
		}

		int Vector3Index(lua_State *state) {
			const Vector3 &value = CheckVector3(state, 1);
			const char *field = luaL_checkstring(state, 2);

			// Roblox spells these upper case and a script author will type them
			// that way. Lower case is accepted too rather than being a silent
			// nil, because `v.x` is what every Lua habit reaches for first.
			switch (field[0]) {
			case 'X':
			case 'x':
				lua_pushnumber(state, value.X);
				return 1;
			case 'Y':
			case 'y':
				lua_pushnumber(state, value.Y);
				return 1;
			case 'Z':
			case 'z':
				lua_pushnumber(state, value.Z);
				return 1;
			default:
				break;
			}

			// **`Magnitude` and `Unit`, which the declarations have always
			// promised and this never delivered.** `engine.d.luau` carries both
			// on `Vector3`, `Vector2` implements both, and a script reaching for
			// `direction.Unit` got "Vector3 has no member 'Unit'" at run time
			// after typechecking clean. `bindings-check` cannot see it: it
			// compares the declarations against the *class table*, and a value
			// type's members are in neither.
			if (std::strcmp(field, "Magnitude") == 0) {
				lua_pushnumber(state, value.Magnitude());
				return 1;
			}

			if (std::strcmp(field, "Unit") == 0) {
				// A zero vector has no direction, and normalising one gives
				// three NaNs that propagate into a transform and surface as
				// geometry vanishing somewhere else entirely. Named here.
				if (value.Magnitude() <= 0.0f) {
					luaL_errorL(state, "the zero vector has no Unit");
				}
				*PushVector3(state) = value.Unit();
				return 1;
			}

			luaL_errorL(state, "Vector3 has no member '%s'", field);
		}

		int Vector3ToString(lua_State *state) {
			const Vector3 &value = CheckVector3(state, 1);
			lua_pushfstring(state, "%f, %f, %f", value.X, value.Y, value.Z);
			return 1;
		}

		// --- Color3 ----------------------------------------------------------

		int Color3New(lua_State *state) {
			const auto r = static_cast<float>(luaL_optnumber(state, 1, 0.0));
			const auto g = static_cast<float>(luaL_optnumber(state, 2, 0.0));
			const auto b = static_cast<float>(luaL_optnumber(state, 3, 0.0));

			*PushColor3(state) = Color3::FromLinear(r, g, b);
			return 1;
		}

		// 0-255, the way an author reads a colour off a palette. Roblox has both
		// and `fromRGB` is the one that appears in real code.
		int Color3FromRgb(lua_State *state) {
			const auto r = static_cast<float>(luaL_optnumber(state, 1, 0.0));
			const auto g = static_cast<float>(luaL_optnumber(state, 2, 0.0));
			const auto b = static_cast<float>(luaL_optnumber(state, 3, 0.0));

			*PushColor3(state) = Color3::FromLinear(r / 255.0f, g / 255.0f, b / 255.0f);
			return 1;
		}

		int Color3Index(lua_State *state) {
			const Color3 &value = CheckColor3(state, 1);
			const char *field = luaL_checkstring(state, 2);

			switch (field[0]) {
			case 'R':
			case 'r':
				lua_pushnumber(state, value.R);
				return 1;
			case 'G':
			case 'g':
				lua_pushnumber(state, value.G);
				return 1;
			case 'B':
			case 'b':
				lua_pushnumber(state, value.B);
				return 1;
			default:
				break;
			}

			luaL_errorL(state, "Color3 has no member '%s'", field);
		}

		// --- CFrame ----------------------------------------------------------

		int CFrameNew(lua_State *state) {
			// **A `Vector3` or three numbers, because Roblox takes both.** An
			// author holding a position writes `CFrame.new(position)` far more
			// often than they unpack it, and a binding that only took numbers
			// made every such line a type error naming the wrong argument.
			if (lua_touserdatatagged(state, 1, TAG_VECTOR3) != nullptr) {
				const Vector3 position = CheckVector3(state, 1);
				*PushCFrame(state) = CFrame{position};
				return 1;
			}

			const auto x = static_cast<float>(luaL_optnumber(state, 1, 0.0));
			const auto y = static_cast<float>(luaL_optnumber(state, 2, 0.0));
			const auto z = static_cast<float>(luaL_optnumber(state, 3, 0.0));

			*PushCFrame(state) = CFrame{Vector3{x, y, z}};
			return 1;
		}

		// **Radians, because Roblox's `CFrame.Angles` is radians** — and the
		// `Orientation` property is degrees, because Roblox's is degrees.
		//
		// That looks like an inconsistency and it is Roblox's, reproduced
		// deliberately. An author's fingers already type `CFrame.Angles(0,
		// math.rad(90), 0)`; a binding that quietly took degrees here would
		// turn every one of those into a rotation 57 times too small, and the
		// scene would look wrong rather than fail.
		int CFrameAngles(lua_State *state) {
			const auto pitch = static_cast<float>(luaL_checknumber(state, 1));
			const auto yaw = static_cast<float>(luaL_checknumber(state, 2));
			const auto roll = static_cast<float>(luaL_checknumber(state, 3));

			*PushCFrame(state) = CFrame::Angles(pitch, yaw, roll);
			return 1;
		}

		// `CFrame.lookAt(from, to, up)` — Roblox's, and the one a camera needs.
		//
		// **Without it a camera can only be placed, not aimed.** `CFrame.new`
		// carries identity rotation, which in this engine's convention looks
		// down -Z — so a camera positioned behind a mirror to reflect it faced
		// away from the mirror and rendered empty space. That was a real bug in
		// `Mirrors-1-world.luau`, and it is the sort a binding gap produces:
		// the script was correct about *where* and had no way to say *which
		// way*.
		int CFrameLookAt(lua_State *state) {
			const Vector3 from = CheckVector3(state, 1);
			const Vector3 to = CheckVector3(state, 2);

			const Vector3 up = lua_touserdatatagged(state, 3, TAG_VECTOR3) != nullptr ? CheckVector3(state, 3)
																					  : Vector3::YAxis;

			*PushCFrame(state) = CFrame::LookAt(from, to, up);
			return 1;
		}

		// `a * b`, which is how a Roblox author composes a transform: an orbit
		// is `CFrame.Angles(0, angle, 0) * CFrame.new(radius, 0, 0)` and a spin
		// is `part.CFrame *= CFrame.Angles(...)`. Without this the idiom that
		// every rotation tutorial opens with does not exist.
		int CFrameMultiply(lua_State *state) {
			const CFrame &left = CheckCFrame(state, 1);

			// `CFrame * Vector3` transforms a point into the frame's space,
			// which is the other half of the same idiom.
			if (lua_touserdatatagged(state, 2, TAG_VECTOR3) != nullptr) {
				*PushVector3(state) = left.PointToWorldSpace(CheckVector3(state, 2));
				return 1;
			}

			*PushCFrame(state) = left * CheckCFrame(state, 2);
			return 1;
		}

		int CFrameIndex(lua_State *state) {
			const CFrame &value = CheckCFrame(state, 1);
			const char *field = luaL_checkstring(state, 2);

			if (std::string_view(field) == "Position" || std::string_view(field) == "p") {
				*PushVector3(state) = value.Position;
				return 1;
			}

			luaL_errorL(state, "CFrame has no member '%s'", field);
		}

		// **Value equality, not identity.** `Vector3.new(1, 2, 3) ==
		// Vector3.new(1, 2, 3)` is true in Roblox and was false here, because
		// two userdata are two objects — so every comparison an author wrote
		// against a constructed value silently failed. Roblox's semantics are
		// what a script expects, and a value type whose equality is identity is
		// not a value type.
		int Vector3Equal(lua_State *state) {
			lua_pushboolean(state, CheckVector3(state, 1) == CheckVector3(state, 2));
			return 1;
		}

		int Color3Equal(lua_State *state) {
			const Color3 &left = CheckColor3(state, 1);
			const Color3 &right = CheckColor3(state, 2);
			lua_pushboolean(state, left.R == right.R && left.G == right.G && left.B == right.B);
			return 1;
		}

		// Exact, component by component. **No epsilon**, and that is the honest
		// choice: two frames built by different arithmetic are not the same
		// frame, and a comparison that pretended otherwise would hide drift
		// rather than reveal it. An author who wants a tolerance writes one.
		int CFrameEqual(lua_State *state) {
			const CFrame &left = CheckCFrame(state, 1);
			const CFrame &right = CheckCFrame(state, 2);
			lua_pushboolean(
				state,
				left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
					left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
					left.QuaternionW == right.QuaternionW
			);
			return 1;
		}

		int Vector3Add(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1) + CheckVector3(state, 2);
			return 1;
		}

		int Vector3Sub(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1) - CheckVector3(state, 2);
			return 1;
		}

		int Vector3Mul(lua_State *state) {
			if (lua_isnumber(state, 2)) {
				*PushVector3(state) = CheckVector3(state, 1) * static_cast<float>(lua_tonumber(state, 2));
				return 1;
			}
			if (lua_isnumber(state, 1)) {
				*PushVector3(state) = CheckVector3(state, 2) * static_cast<float>(lua_tonumber(state, 1));
				return 1;
			}
			*PushVector3(state) = CheckVector3(state, 1) * CheckVector3(state, 2);
			return 1;
		}

		int Vector3Unm(lua_State *state) {
			*PushVector3(state) = -CheckVector3(state, 1);
			return 1;
		}

		// Registers one value type: a metatable carrying `__index` and a global
		// table carrying the constructors.
		void Install(
			lua_State *state,
			const char *name,
			lua_CFunction index,
			lua_CFunction toString,
			const luaL_Reg *constructors,
			lua_CFunction multiply = nullptr,
			lua_CFunction equal = nullptr,
			lua_CFunction add = nullptr,
			lua_CFunction subtract = nullptr,
			lua_CFunction negate = nullptr
		) {
			luaL_newmetatable(state, name);

			lua_pushcfunction(state, index, "__index");
			lua_setfield(state, -2, "__index");

			// **What `typeof` actually reads.** Luau's `typeof` is a fastcall
			// builtin, so a global of that name is never consulted — the VM
			// reaches `luaB_typeof` directly, and that function returns this
			// field when a metatable carries one. One string beside the type's
			// own name, rather than a table of userdata tags somewhere else.
			lua_pushstring(state, name);
			lua_setfield(state, -2, "__type");

			const struct {
				const char *Field;
				lua_CFunction Function;
			} METAMETHODS[] = {
				{"__eq", equal},
				{"__add", add},
				{"__sub", subtract},
				{"__unm", negate},
			};

			for (const auto &entry : METAMETHODS) {
				if (entry.Function != nullptr) {
					lua_pushcfunction(state, entry.Function, entry.Field);
					lua_setfield(state, -2, entry.Field);
				}
			}

			if (multiply != nullptr) {
				lua_pushcfunction(state, multiply, "__mul");
				lua_setfield(state, -2, "__mul");
			}

			if (toString != nullptr) {
				lua_pushcfunction(state, toString, "__tostring");
				lua_setfield(state, -2, "__tostring");
			}

			// A metatable a script can reach is a metatable a script can
			// rewrite, and then every value of that type changes underneath
			// everything else holding one.
			lua_pushstring(state, name);
			lua_setfield(state, -2, "__metatable");

			lua_pop(state, 1);

			lua_newtable(state);
			luaL_register(state, nullptr, constructors);
			lua_setglobal(state, name);
		}
	}

	core::Vector3 *PushVector3(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::Vector3), TAG_VECTOR3);
		auto *value = new (memory) core::Vector3();
		luaL_getmetatable(state, "Vector3");
		lua_setmetatable(state, -2);
		return value;
	}

	core::Color3 *PushColor3(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::Color3), TAG_COLOR3);
		auto *value = new (memory) core::Color3();
		luaL_getmetatable(state, "Color3");
		lua_setmetatable(state, -2);
		return value;
	}

	core::CFrame *PushCFrame(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::CFrame), TAG_CFRAME);
		auto *value = new (memory) core::CFrame();
		luaL_getmetatable(state, "CFrame");
		lua_setmetatable(state, -2);
		return value;
	}

	core::Vector3 &CheckVector3(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_VECTOR3);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "Vector3");
		}
		return *static_cast<core::Vector3 *>(value);
	}

	core::Color3 &CheckColor3(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_COLOR3);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "Color3");
		}
		return *static_cast<core::Color3 *>(value);
	}

	core::CFrame &CheckCFrame(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_CFRAME);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "CFrame");
		}
		return *static_cast<core::CFrame *>(value);
	}

	// --- the four the 2D tree is authored in ---------------------------------
	//
	// **The metatables are `LuauDatatypes.cpp`'s and only the accessors are
	// here**, which is the same split the three above already have: that file
	// owns what a `UDim2` *is* to a script, and this one owns handing a
	// property's bytes across. Two definitions of the metatable would be two
	// answers to `typeof(value)`.
	//
	// The tag is what makes the check safe rather than the size. `Vector2`,
	// `UDim` and the two halves of a `UDim2` are all pairs of floats, so a check
	// on shape would accept any of them for any other — and `frame.Position =
	// Vector2.new(0, 0)` would silently mean an offset of zero at scale zero.

	core::Vector2 *PushVector2(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::Vector2), TAG_VECTOR2);
		auto *value = new (memory) core::Vector2();
		luaL_getmetatable(state, "Vector2");
		lua_setmetatable(state, -2);
		return value;
	}

	core::UDim *PushUDim(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::UDim), TAG_UDIM);
		auto *value = new (memory) core::UDim();
		luaL_getmetatable(state, "UDim");
		lua_setmetatable(state, -2);
		return value;
	}

	core::UDim2 *PushUDim2(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::UDim2), TAG_UDIM2);
		auto *value = new (memory) core::UDim2();
		luaL_getmetatable(state, "UDim2");
		lua_setmetatable(state, -2);
		return value;
	}

	core::Rect *PushRect(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::Rect), TAG_RECT);
		auto *value = new (memory) core::Rect();
		luaL_getmetatable(state, "Rect");
		lua_setmetatable(state, -2);
		return value;
	}

	core::Vector2 &CheckVector2Value(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_VECTOR2);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "Vector2");
		}
		return *static_cast<core::Vector2 *>(value);
	}

	core::UDim &CheckUDim(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_UDIM);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "UDim");
		}
		return *static_cast<core::UDim *>(value);
	}

	core::UDim2 &CheckUDim2(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_UDIM2);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "UDim2");
		}
		return *static_cast<core::UDim2 *>(value);
	}

	core::Rect &CheckRect(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_RECT);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "Rect");
		}
		return *static_cast<core::Rect *>(value);
	}

	// --- the three a curve is authored in ------------------------------------
	//
	// **Accessors only, for the reason the four above give**: `LuauDatatypes.cpp`
	// owns what a `NumberSequence` *is* to a script, and this file owns handing a
	// property's bytes across. Two definitions of the metatable would be two
	// answers to `typeof(value)`.
	//
	// **These three are the first values here that are not a handful of floats.**
	// A `ColorSequence` is 408 bytes, so `lua_newuserdatatagged` allocates that
	// much per push — which is why the property path reads into a stack buffer
	// once and pushes once, rather than pushing a temporary per keypoint the way
	// the `Keypoints` getter does. The getter is a script asking for the stops;
	// this is a property read, and it happens on every `.Changed` fan-out.

	core::NumberRange *PushNumberRange(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::NumberRange), TAG_NUMBER_RANGE);
		auto *value = new (memory) core::NumberRange();
		luaL_getmetatable(state, "NumberRange");
		lua_setmetatable(state, -2);
		return value;
	}

	core::NumberSequence *PushNumberSequence(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::NumberSequence), TAG_NUMBER_SEQUENCE);
		auto *value = new (memory) core::NumberSequence();
		luaL_getmetatable(state, "NumberSequence");
		lua_setmetatable(state, -2);
		return value;
	}

	core::ColorSequence *PushColorSequence(lua_State *state) {
		void *memory = lua_newuserdatatagged(state, sizeof(core::ColorSequence), TAG_COLOR_SEQUENCE);
		auto *value = new (memory) core::ColorSequence();
		luaL_getmetatable(state, "ColorSequence");
		lua_setmetatable(state, -2);
		return value;
	}

	core::NumberRange &CheckNumberRange(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_NUMBER_RANGE);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "NumberRange");
		}
		return *static_cast<core::NumberRange *>(value);
	}

	core::NumberSequence &CheckNumberSequence(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_NUMBER_SEQUENCE);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "NumberSequence");
		}
		return *static_cast<core::NumberSequence *>(value);
	}

	core::ColorSequence &CheckColorSequence(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_COLOR_SEQUENCE);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "ColorSequence");
		}
		return *static_cast<core::ColorSequence *>(value);
	}

	namespace {
		// Puts a constant vector on a constructor table that `Install` has just
		// set as a global.
		//
		// **A field on the global table rather than a case in `Vector3Index`**,
		// and the difference is which side of the dot it is on. `Vector3Index`
		// answers `someVector.X` — a member of a *value* — and Roblox's `zero` and
		// `one` are members of the *library*, the same shelf `Vector3.new` sits on.
		// Putting them in the index would make `part.Position.zero` answer, which
		// is a member nobody wrote.
		//
		// Pushed as an ordinary userdata, so it carries the same metatable and the
		// same tag as anything `Vector3.new` returns — a constant that failed
		// `CheckVector3` would be a constant no property could be assigned from.
		//
		// **A fresh copy per read is not what this gives**, and that is worth
		// stating because it looks like a hazard and is not: `Vector3` has no
		// mutating member and no field a script can assign, so the one userdata
		// behind `Vector3.zero` cannot be written through. `Install` seals the
		// metatable for the same class of reason.
		void SetVectorConstant(
			lua_State *state, const char *global, const char *field, const core::Vector3 &value
		) {
			lua_getglobal(state, global);
			*PushVector3(state) = value;
			lua_setfield(state, -2, field);
			lua_pop(state, 1);
		}
	}

	void OpenValues(lua_State *state) {
		static const luaL_Reg vectorConstructors[] = {{"new", Vector3New}, {nullptr, nullptr}};
		static const luaL_Reg colorConstructors[] = {
			{"new", Color3New}, {"fromRGB", Color3FromRgb}, {nullptr, nullptr}
		};
		static const luaL_Reg frameConstructors[] = {
			{"new", CFrameNew}, {"Angles", CFrameAngles}, {"lookAt", CFrameLookAt}, {nullptr, nullptr}
		};

		Install(
			state,
			"Vector3",
			Vector3Index,
			Vector3ToString,
			vectorConstructors,
			Vector3Mul,
			Vector3Equal,
			Vector3Add,
			Vector3Sub,
			Vector3Unm
		);
		Install(state, "Color3", Color3Index, nullptr, colorConstructors, nullptr, Color3Equal);
		Install(state, "CFrame", CFrameIndex, nullptr, frameConstructors, CFrameMultiply, CFrameEqual);

		// **Lowercase, because Roblox's are.** Every other member of this
		// vocabulary is capitalised and these two are not, which reads as a
		// mistake until you try to run a script written elsewhere. The rule
		// `scene/Part.cpp` states for the class tree is the rule here: a second
		// spelling of one constant is the duplicate `AGENTS.md` calls the most
		// expensive kind of debt, and `Vector3.Zero` would be exactly that.
		//
		// After both `Install` calls, because the global table they set is what
		// these attach to.
		SetVectorConstant(state, "Vector3", "zero", core::Vector3::Zero);
		SetVectorConstant(state, "Vector3", "one", core::Vector3::One);
	}

	// --- the host seam's builders ---------------------------------------------
	//
	// Here rather than in the header, so `Host.hpp` stays a description of what
	// crosses and carries no code a consumer compiles.

	HostValue HostValue::Of(bool value) {
		HostValue out(HostTag::Boolean);
		out.Boolean = value;
		return out;
	}

	HostValue HostValue::Of(double value) {
		HostValue out(HostTag::Number);
		out.Number = value;
		return out;
	}

	HostValue HostValue::Of(std::string_view value) {
		HostValue out(HostTag::String);
		out.Text = value;
		return out;
	}

	HostValue HostValue::Of(ecs::Entity value) {
		HostValue out(HostTag::Instance);
		out.Instance = value;
		return out;
	}

	HostValue HostValue::List(std::vector<HostValue> items) {
		HostValue out(HostTag::Array);
		out.Items = std::move(items);
		return out;
	}
}
