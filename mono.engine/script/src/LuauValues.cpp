#include "LuauBindings.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/script/Datatypes.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <lualib.h>
#include <numbers>
#include <string_view>

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

		// `Vector3.FromNormalId(Enum.NormalId.Top)` - a face of a box as the
		// direction it points.
		//
		// **Capitalised, because Roblox capitalises these two and not `new`.**
		// It reads as an inconsistency and is one, but it is Roblox's, and a
		// second spelling of one constructor is the duplicate `AGENTS.md` calls
		// the most expensive kind of debt.
		//
		// The face-to-direction mapping is `script::DirectionOfNormalId`, shared
		// with the JavaScript surface so the two cannot drift.
		int Vector3FromNormalId(lua_State *state) {
			core::Name member;
			Vector3 direction;

			if (!ReadEnumValue(state, 1, core::Name("NormalId"), member) ||
				!DirectionOfNormalId(member, direction)) {
				luaL_errorL(state, "FromNormalId needs an Enum.NormalId");
			}

			*PushVector3(state) = direction;
			return 1;
		}

		// `Vector3.FromAxis(Enum.Axis.Y)`.
		int Vector3FromAxis(lua_State *state) {
			core::Name member;
			Vector3 direction;

			if (!ReadEnumValue(state, 1, core::Name("Axis"), member) || !DirectionOfAxis(member, direction)) {
				luaL_errorL(state, "FromAxis needs an Enum.Axis");
			}

			*PushVector3(state) = direction;
			return 1;
		}

		// --- the methods a script calls with a colon -------------------------
		//
		// The vector is argument one, because `v:Dot(other)` passes it there.
		// Each one is `core::Vector3`'s own arithmetic and none of it is
		// implemented here: the JavaScript surface calls the same functions, so
		// the two languages cannot answer differently about the same vector.

		int Vector3Abs(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Abs();
			return 1;
		}

		int Vector3Ceil(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Ceil();
			return 1;
		}

		int Vector3Floor(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Floor();
			return 1;
		}

		int Vector3Sign(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Sign();
			return 1;
		}

		int Vector3Cross(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Cross(CheckVector3(state, 2));
			return 1;
		}

		int Vector3Dot(lua_State *state) {
			lua_pushnumber(state, CheckVector3(state, 1).Dot(CheckVector3(state, 2)));
			return 1;
		}

		// `v:Angle(other)` is unsigned, from zero to pi. `v:Angle(other, axis)`
		// is signed by which side of `axis` the turn goes, which is what a
		// steering routine actually needs - the unsigned form cannot tell left
		// from right.
		int Vector3Angle(lua_State *state) {
			const Vector3 self = CheckVector3(state, 1);
			const Vector3 other = CheckVector3(state, 2);

			if (lua_touserdatatagged(state, 3, TAG_VECTOR3) != nullptr) {
				lua_pushnumber(state, self.Angle(other, CheckVector3(state, 3)));
				return 1;
			}

			lua_pushnumber(state, self.Angle(other));
			return 1;
		}

		// The epsilon is left to `core::Vector3::FuzzyEq` when a script omits
		// it, rather than repeated here: a default written down twice is a
		// default that means two things the first time one of them moves.
		int Vector3FuzzyEq(lua_State *state) {
			const Vector3 self = CheckVector3(state, 1);
			const Vector3 other = CheckVector3(state, 2);

			if (lua_isnoneornil(state, 3)) {
				lua_pushboolean(state, self.FuzzyEq(other));
				return 1;
			}

			lua_pushboolean(state, self.FuzzyEq(other, static_cast<float>(luaL_checknumber(state, 3))));
			return 1;
		}

		int Vector3Lerp(lua_State *state) {
			const Vector3 self = CheckVector3(state, 1);
			const Vector3 goal = CheckVector3(state, 2);
			const auto alpha = static_cast<float>(luaL_checknumber(state, 3));

			*PushVector3(state) = self.Lerp(goal, alpha);
			return 1;
		}

		int Vector3Max(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Max(CheckVector3(state, 2));
			return 1;
		}

		int Vector3Min(lua_State *state) {
			*PushVector3(state) = CheckVector3(state, 1).Min(CheckVector3(state, 2));
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

			// **`__index` is a function, so a method has to be handed back as a
			// closure** - there is no table behind it for Luau to find one in.
			// `RayIndex` does the same for `PointAt`. A fresh closure per access
			// is what Luau's `lua_pushcfunction` costs, and a table cached in the
			// registry would trade that for a second place the member list
			// lives; this list is short and read once per call site in practice.
			static const luaL_Reg METHODS[] = {
				{"Abs", Vector3Abs},
				{"Ceil", Vector3Ceil},
				{"Floor", Vector3Floor},
				{"Sign", Vector3Sign},
				{"Cross", Vector3Cross},
				{"Dot", Vector3Dot},
				{"Angle", Vector3Angle},
				{"FuzzyEq", Vector3FuzzyEq},
				{"Lerp", Vector3Lerp},
				{"Max", Vector3Max},
				{"Min", Vector3Min},
			};

			for (const luaL_Reg &method : METHODS) {
				if (std::strcmp(field, method.name) == 0) {
					lua_pushcfunction(state, method.func, method.name);
					return 1;
				}
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
			const Vector3 position{x, y, z};

			// **Roblox's other two arities, told apart by how many numbers
			// arrived.** Seven is a position and a quaternion; twelve is a
			// position and a row-major rotation matrix. Counting arguments
			// rather than asking for a mode is what Roblox does, and a script
			// that serialised a CFrame with `GetComponents` hands the twelve
			// straight back here.
			// **Twelve tested before seven**, because `>= 7` matches a
			// twelve-argument call too and would swallow it: the rotation matrix
			// form would silently read its first four numbers as a quaternion
			// and produce a frame nothing complained about.
			const int given = lua_gettop(state);

			if (given >= 12) {
				// Row-major, as `GetComponents` reports them, so the columns
				// `FromMatrix` wants are the strided reads below.
				const auto at = [state](int index) {
					return static_cast<float>(luaL_checknumber(state, index));
				};
				*PushCFrame(state) = CFrame::FromMatrix(
					position, Vector3{at(4), at(7), at(10)}, Vector3{at(5), at(8), at(11)}
				);
				return 1;
			}

			if (given >= 7) {
				const auto qx = static_cast<float>(luaL_checknumber(state, 4));
				const auto qy = static_cast<float>(luaL_checknumber(state, 5));
				const auto qz = static_cast<float>(luaL_checknumber(state, 6));
				const auto qw = static_cast<float>(luaL_checknumber(state, 7));

				CFrame frame;
				frame.Position = position;
				frame.QuaternionX = qx;
				frame.QuaternionY = qy;
				frame.QuaternionZ = qz;
				frame.QuaternionW = qw;

				// **Normalised on the way in.** A caller writing four numbers by
				// hand almost never writes a unit quaternion, and every function
				// on this type documents that it expects one - an unnormalised
				// rotation silently scales whatever it transforms.
				*PushCFrame(state) = frame.Orthonormalize();
				return 1;
			}

			*PushCFrame(state) = CFrame{position};
			return 1;
		}

		// **Radians, because Roblox's `CFrame.Angles` is radians** - and the
		// `Orientation` property is degrees, because Roblox's is degrees.
		//
		// That looks like an inconsistency and it is Roblox's, reproduced
		// deliberately. An author's fingers already type `CFrame.Angles(0,
		// math.rad(90), 0)`; a binding that quietly took degrees here would
		// turn every one of those into a rotation 57 times too small, and the
		// scene would look wrong rather than fail.
		// `CFrame.Angles(rx, ry, rz)` and `CFrame.fromEulerAnglesXYZ(rx, ry, rz)`.
		//
		// **X, then Y, then Z, which is what these two names mean in Roblox and
		// is not what this bound until v0.18.** It called `CFrame::Angles`,
		// which composes Y-X-Z - Roblox's `fromOrientation` - so a script pasted
		// from Roblox turned the wrong way whenever it named two axes at once.
		// One axis at a time is identical under either order, which is why it
		// went unnoticed.
		int CFrameEulerXYZ(lua_State *state) {
			const auto rx = static_cast<float>(luaL_checknumber(state, 1));
			const auto ry = static_cast<float>(luaL_checknumber(state, 2));
			const auto rz = static_cast<float>(luaL_checknumber(state, 3));

			*PushCFrame(state) = CFrame::FromEulerAnglesXYZ(rx, ry, rz);
			return 1;
		}

		// `CFrame.fromEulerAnglesYXZ` and `CFrame.fromOrientation`, which Roblox
		// spells two ways for one function. This is the order `BasePart.
		// Orientation` round-trips through, in Roblox and here.
		int CFrameEulerYXZ(lua_State *state) {
			const auto rx = static_cast<float>(luaL_checknumber(state, 1));
			const auto ry = static_cast<float>(luaL_checknumber(state, 2));
			const auto rz = static_cast<float>(luaL_checknumber(state, 3));

			*PushCFrame(state) = CFrame::Angles(rx, ry, rz);
			return 1;
		}

		int CFrameFromAxisAngle(lua_State *state) {
			const Vector3 axis = CheckVector3(state, 1);
			const auto angle = static_cast<float>(luaL_checknumber(state, 2));

			*PushCFrame(state) = CFrame::FromAxisAngle(axis, angle);
			return 1;
		}

		// `CFrame.fromMatrix(position, vX, vY, vZ)`.
		//
		// The fourth argument is accepted and ignored, as Roblox's is optional:
		// a third basis vector is fully determined by the first two, and honouring
		// a caller's disagreeing one would be honouring a basis that is not a
		// rotation. `CFrame::FromMatrix` derives it.
		int CFrameFromMatrix(lua_State *state) {
			const Vector3 position = CheckVector3(state, 1);
			const Vector3 right = CheckVector3(state, 2);
			const Vector3 up = CheckVector3(state, 3);

			*PushCFrame(state) = CFrame::FromMatrix(position, right, up);
			return 1;
		}

		int CFrameFromRotationBetween(lua_State *state) {
			const Vector3 from = CheckVector3(state, 1);
			const Vector3 to = CheckVector3(state, 2);

			*PushCFrame(state) = CFrame::FromRotationBetweenVectors(from, to);
			return 1;
		}

		// `CFrame.lookAlong(at, direction, up)` - `lookAt` given a direction
		// rather than a target, which is the form a caller holding a velocity
		// already has.
		int CFrameLookAlong(lua_State *state) {
			const Vector3 at = CheckVector3(state, 1);
			const Vector3 direction = CheckVector3(state, 2);
			const Vector3 up = lua_touserdatatagged(state, 3, TAG_VECTOR3) != nullptr ? CheckVector3(state, 3)
																					  : Vector3::YAxis;

			*PushCFrame(state) = CFrame::LookAt(at, at + direction, up);
			return 1;
		}

		// `CFrame.lookAt(from, to, up)` - Roblox's, and the one a camera needs.
		//
		// **Without it a camera can only be placed, not aimed.** `CFrame.new`
		// carries identity rotation, which in this engine's convention looks
		// down -Z - so a camera positioned behind a mirror to reflect it faced
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

		// `CFrame + Vector3` and `CFrame - Vector3`, which Roblox has and which
		// move a frame without turning it.
		//
		// **Translation only, and that is the distinction from `*`.** `frame *
		// CFrame.new(offset)` moves by the offset expressed in the frame's own
		// space; this moves by it in world space. The two agree only for an
		// unrotated frame, which is exactly the case a test would use if nobody
		// had thought about it.
		int CFrameAdd(lua_State *state) {
			const CFrame &frame = CheckCFrame(state, 1);
			CFrame moved = frame;
			moved.Position = frame.Position + CheckVector3(state, 2);

			*PushCFrame(state) = moved;
			return 1;
		}

		int CFrameSubtract(lua_State *state) {
			const CFrame &frame = CheckCFrame(state, 1);
			CFrame moved = frame;
			moved.Position = frame.Position - CheckVector3(state, 2);

			*PushCFrame(state) = moved;
			return 1;
		}

		// --- CFrame methods ---------------------------------------------------
		//
		// **Held in one table in the registry rather than pushed as a fresh
		// closure per lookup.** `frame:Inverse()` in a per-frame loop would
		// otherwise allocate a C closure on every call, and CFrame maths is
		// exactly the thing scripts do in loops. `CFrameIndex` reads the table;
		// `OpenValues` builds it once.

		int CFrameInverse(lua_State *state) {
			*PushCFrame(state) = CheckCFrame(state, 1).Inverse();
			return 1;
		}

		int CFrameOrthonormalize(lua_State *state) {
			*PushCFrame(state) = CheckCFrame(state, 1).Orthonormalize();
			return 1;
		}

		int CFrameLerpMethod(lua_State *state) {
			const CFrame &from = CheckCFrame(state, 1);
			const CFrame &to = CheckCFrame(state, 2);
			const auto alpha = static_cast<float>(luaL_checknumber(state, 3));

			// `Lerp` and not `NLerp`, because Roblox's is constant angular speed
			// and a script asking to interpolate over a second wants that. The
			// cheap one is for the interpolator between two ticks, which is C++.
			*PushCFrame(state) = from.Lerp(to, alpha);
			return 1;
		}

		int CFrameToWorldSpaceMethod(lua_State *state) {
			*PushCFrame(state) = CheckCFrame(state, 1).ToWorldSpace(CheckCFrame(state, 2));
			return 1;
		}

		int CFrameToObjectSpaceMethod(lua_State *state) {
			*PushCFrame(state) = CheckCFrame(state, 1).ToObjectSpace(CheckCFrame(state, 2));
			return 1;
		}

		int CFramePointToWorld(lua_State *state) {
			*PushVector3(state) = CheckCFrame(state, 1).PointToWorldSpace(CheckVector3(state, 2));
			return 1;
		}

		int CFramePointToObject(lua_State *state) {
			*PushVector3(state) = CheckCFrame(state, 1).PointToObjectSpace(CheckVector3(state, 2));
			return 1;
		}

		int CFrameVectorToWorld(lua_State *state) {
			*PushVector3(state) = CheckCFrame(state, 1).VectorToWorldSpace(CheckVector3(state, 2));
			return 1;
		}

		int CFrameVectorToObject(lua_State *state) {
			*PushVector3(state) = CheckCFrame(state, 1).VectorToObjectSpace(CheckVector3(state, 2));
			return 1;
		}

		// Twelve return values, as Roblox's does. Not a table: the call site is
		// `local x, y, z, r00 = frame:GetComponents()`, and a table would be an
		// allocation plus twelve index operations to reach the same numbers.
		int CFrameGetComponents(lua_State *state) {
			const std::array<float, 12> parts = CheckCFrame(state, 1).GetComponents();
			for (const float part : parts) {
				lua_pushnumber(state, part);
			}
			return static_cast<int>(parts.size());
		}

		int CFrameToEulerXYZ(lua_State *state) {
			const Vector3 angles = CheckCFrame(state, 1).ToEulerAnglesXYZ();
			lua_pushnumber(state, angles.X);
			lua_pushnumber(state, angles.Y);
			lua_pushnumber(state, angles.Z);
			return 3;
		}

		// `ToEulerAnglesYXZ` and `ToOrientation`, one function under two Roblox
		// names. This is the order `BasePart.Orientation` uses.
		int CFrameToEulerYXZ(lua_State *state) {
			const Vector3 angles = CheckCFrame(state, 1).ToAngles();
			lua_pushnumber(state, angles.X);
			lua_pushnumber(state, angles.Y);
			lua_pushnumber(state, angles.Z);
			return 3;
		}

		int CFrameToAxisAngleMethod(lua_State *state) {
			Vector3 axis;
			float angle = 0.0f;
			CheckCFrame(state, 1).ToAxisAngle(axis, angle);

			*PushVector3(state) = axis;
			lua_pushnumber(state, angle);
			return 2;
		}

		int CFrameFuzzyEqMethod(lua_State *state) {
			const CFrame &frame = CheckCFrame(state, 1);
			const CFrame &other = CheckCFrame(state, 2);

			// Roblox's default epsilon. Named rather than spelled inline so the
			// two runtimes cannot drift apart on it.
			constexpr float DEFAULT_EPSILON = 1.0e-5f;
			const auto epsilon = static_cast<float>(luaL_optnumber(state, 3, DEFAULT_EPSILON));

			lua_pushboolean(state, frame.FuzzyEq(other, epsilon) ? 1 : 0);
			return 1;
		}

		int CFrameAngleBetweenMethod(lua_State *state) {
			lua_pushnumber(state, CheckCFrame(state, 1).AngleBetween(CheckCFrame(state, 2)));
			return 1;
		}

		const luaL_Reg CFRAME_METHODS[] = {
			{"Inverse", CFrameInverse},
			{"Orthonormalize", CFrameOrthonormalize},
			{"Lerp", CFrameLerpMethod},
			{"ToWorldSpace", CFrameToWorldSpaceMethod},
			{"ToObjectSpace", CFrameToObjectSpaceMethod},
			{"PointToWorldSpace", CFramePointToWorld},
			{"PointToObjectSpace", CFramePointToObject},
			{"VectorToWorldSpace", CFrameVectorToWorld},
			{"VectorToObjectSpace", CFrameVectorToObject},
			{"GetComponents", CFrameGetComponents},
			// Roblox's lower-case alias for the same twelve numbers.
			{"components", CFrameGetComponents},
			{"ToEulerAnglesXYZ", CFrameToEulerXYZ},
			{"ToEulerAnglesYXZ", CFrameToEulerYXZ},
			{"ToOrientation", CFrameToEulerYXZ},
			{"ToAxisAngle", CFrameToAxisAngleMethod},
			{"FuzzyEq", CFrameFuzzyEqMethod},
			{"AngleBetween", CFrameAngleBetweenMethod},
			{nullptr, nullptr},
		};

		// The registry key the table above is parked under.
		constexpr const char *CFRAME_METHOD_TABLE = "engine.cframe.methods";

		int CFrameIndex(lua_State *state) {
			const CFrame &value = CheckCFrame(state, 1);
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			// `p` alongside `Position` for the reason `Vector3` accepts `x`: it
			// is the spelling a long-standing Roblox habit reaches for, and a
			// silent nil is a worse answer than an alias.
			if (name == "Position" || name == "p") {
				*PushVector3(state) = value.Position;
				return 1;
			}

			// The rotation alone, at the origin. Roblox's `CFrame.Rotation`.
			if (name == "Rotation") {
				*PushCFrame(state) = value.RotationOnly();
				return 1;
			}

			if (name == "X" || name == "x") {
				lua_pushnumber(state, value.Position.X);
				return 1;
			}
			if (name == "Y" || name == "y") {
				lua_pushnumber(state, value.Position.Y);
				return 1;
			}
			if (name == "Z" || name == "z") {
				lua_pushnumber(state, value.Position.Z);
				return 1;
			}

			// **Six names for three columns, because Roblox has six.**
			// `RightVector`/`UpVector` and `XVector`/`YVector` are the same
			// directions under two spellings; `LookVector` and `ZVector` are the
			// pair that differ, one being the negation of the other. A script
			// reading all three columns wants the `*Vector` set and should not
			// have to know which one to negate.
			if (name == "RightVector" || name == "XVector") {
				*PushVector3(state) = value.RightVector();
				return 1;
			}
			if (name == "UpVector" || name == "YVector") {
				*PushVector3(state) = value.UpVector();
				return 1;
			}
			if (name == "LookVector") {
				*PushVector3(state) = value.LookVector();
				return 1;
			}
			if (name == "ZVector") {
				*PushVector3(state) = value.ZVector();
				return 1;
			}

			// A method, from the one table built at open time. Missing keys fall
			// through to the error below rather than returning nil, which is
			// what turns `frame:Inverze()` into a line number instead of
			// "attempt to call a nil value".
			lua_getfield(state, LUA_REGISTRYINDEX, CFRAME_METHOD_TABLE);
			lua_getfield(state, -1, field);
			if (!lua_isnil(state, -1)) {
				lua_remove(state, -2);
				return 1;
			}
			lua_pop(state, 2);

			luaL_errorL(state, "CFrame has no member '%s'", field);
		}

		// **Value equality, not identity.** `Vector3.new(1, 2, 3) ==
		// Vector3.new(1, 2, 3)` is true in Roblox and was false here, because
		// two userdata are two objects - so every comparison an author wrote
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

		// **The vector has to be on the left, and that is not an oversight.**
		// Luau calls `__div` whichever operand carried the metatable, so `2 / v`
		// arrives here too - and Roblox's operator table has no `number /
		// Vector3` row. Answering one would be inventing arithmetic rather than
		// matching it, so `CheckVector3` refuses the first argument and names the
		// type. `2 * v` is different: Roblox does define it, and `Vector3Mul`
		// handles it.
		int Vector3Div(lua_State *state) {
			const Vector3 left = CheckVector3(state, 1);

			if (lua_isnumber(state, 2)) {
				*PushVector3(state) = left / static_cast<float>(lua_tonumber(state, 2));
				return 1;
			}

			*PushVector3(state) = left / CheckVector3(state, 2);
			return 1;
		}

		// `//`, which floors each quotient - the same thing Luau's `//` does to
		// two numbers, applied per component.
		int Vector3Idiv(lua_State *state) {
			const Vector3 left = CheckVector3(state, 1);

			if (lua_isnumber(state, 2)) {
				*PushVector3(state) = (left / static_cast<float>(lua_tonumber(state, 2))).Floor();
				return 1;
			}

			*PushVector3(state) = (left / CheckVector3(state, 2)).Floor();
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
			lua_CFunction negate = nullptr,
			lua_CFunction divide = nullptr,
			lua_CFunction floorDivide = nullptr
		) {
			luaL_newmetatable(state, name);

			lua_pushcfunction(state, index, "__index");
			lua_setfield(state, -2, "__index");

			// **What `typeof` actually reads.** Luau's `typeof` is a fastcall
			// builtin, so a global of that name is never consulted - the VM
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
				{"__div", divide},
				{"__idiv", floorDivide},
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
	// on shape would accept any of them for any other - and `frame.Position =
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

	// **Here rather than in `TweenService.cpp`, where it used to be a
	// file-local.** `LuauCall::AsTweenInfo` is the second caller, and a checker
	// written twice is the shape `CheckVector2Value`'s comment already regrets
	// one entry up.
	core::TweenInfo &CheckTweenInfoValue(lua_State *state, int index) {
		void *value = lua_touserdatatagged(state, index, TAG_TWEEN_INFO);
		if (value == nullptr) {
			luaL_typeerrorL(state, index, "TweenInfo");
		}
		return *static_cast<core::TweenInfo *>(value);
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
	// much per push - which is why the property path reads into a stack buffer
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
		// answers `someVector.X` - a member of a *value* - and Roblox's `zero` and
		// `one` are members of the *library*, the same shelf `Vector3.new` sits on.
		// Putting them in the index would make `part.Position.zero` answer, which
		// is a member nobody wrote.
		//
		// Pushed as an ordinary userdata, so it carries the same metatable and the
		// same tag as anything `Vector3.new` returns - a constant that failed
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
		static const luaL_Reg vectorConstructors[] = {
			{"new", Vector3New},
			{"FromNormalId", Vector3FromNormalId},
			{"FromAxis", Vector3FromAxis},
			{nullptr, nullptr}
		};
		static const luaL_Reg colorConstructors[] = {
			{"new", Color3New}, {"fromRGB", Color3FromRgb}, {nullptr, nullptr}
		};
		static const luaL_Reg frameConstructors[] = {
			{"new", CFrameNew},

			// **`Angles` is X-Y-Z, which is what Roblox means by it.** Until
			// v0.18 this name was bound to `CFrame::Angles`, the Y-X-Z
			// composition, so a pasted script turned the wrong way whenever it
			// named two axes at once. The Y-X-Z one is still here under the two
			// names Roblox gives it.
			{"Angles", CFrameEulerXYZ},
			{"fromEulerAnglesXYZ", CFrameEulerXYZ},
			{"fromEulerAnglesYXZ", CFrameEulerYXZ},
			{"fromOrientation", CFrameEulerYXZ},

			{"lookAt", CFrameLookAt},
			{"lookAlong", CFrameLookAlong},
			{"fromAxisAngle", CFrameFromAxisAngle},
			{"fromMatrix", CFrameFromMatrix},
			{"fromRotationBetweenVectors", CFrameFromRotationBetween},
			{nullptr, nullptr}
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
			Vector3Unm,
			Vector3Div,
			Vector3Idiv
		);
		Install(state, "Color3", Color3Index, nullptr, colorConstructors, nullptr, Color3Equal);
		// The method table, parked in the registry before anything can index a
		// CFrame. `CFrameIndex` reads it; building it once is what keeps
		// `frame:Inverse()` in a loop from allocating a closure per call.
		lua_newtable(state);
		for (const luaL_Reg *entry = CFRAME_METHODS; entry->name != nullptr; entry++) {
			lua_pushcfunction(state, entry->func, entry->name);
			lua_setfield(state, -2, entry->name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, CFRAME_METHOD_TABLE);

		Install(
			state,
			"CFrame",
			CFrameIndex,
			nullptr,
			frameConstructors,
			CFrameMultiply,
			CFrameEqual,
			CFrameAdd,
			CFrameSubtract
		);

		// `CFrame.identity`, a value on the constructor table rather than a
		// function. Roblox has it, and `CFrame.new()` is not the same thing to
		// read: one says "no transform" and the other says "a transform I am
		// about to fill in".
		lua_getglobal(state, "CFrame");
		*PushCFrame(state) = CFrame();
		lua_setfield(state, -2, "identity");
		lua_pop(state, 1);

		// **Lowercase, because Roblox's are.** Every other member of this
		// vocabulary is capitalised and these five are not, which reads as a
		// mistake until you try to run a script written elsewhere. The rule
		// `scene/Part.cpp` states for the class tree is the rule here: a second
		// spelling of one constant is the duplicate `AGENTS.md` calls the most
		// expensive kind of debt, and `Vector3.Zero` would be exactly that.
		//
		// After both `Install` calls, because the global table they set is what
		// these attach to.
		SetVectorConstant(state, "Vector3", "zero", core::Vector3::Zero);
		SetVectorConstant(state, "Vector3", "one", core::Vector3::One);
		SetVectorConstant(state, "Vector3", "xAxis", core::Vector3::XAxis);
		SetVectorConstant(state, "Vector3", "yAxis", core::Vector3::YAxis);
		SetVectorConstant(state, "Vector3", "zAxis", core::Vector3::ZAxis);
	}

	const char *Describe(HostTag tag) {
		switch (tag) {
		case HostTag::Nil:
			return "nil";
		case HostTag::Boolean:
			return "a boolean";
		case HostTag::Number:
			return "a number";
		case HostTag::String:
			return "a string";
		case HostTag::Array:
			return "an array";
		case HostTag::Map:
			return "a table";
		case HostTag::Instance:
			return "an Instance";
		case HostTag::Callback:
			return "a function";
		case HostTag::Vector3:
			return "a Vector3";
		case HostTag::Color3:
			return "a Color3";
		case HostTag::CFrame:
			return "a CFrame";
		}
		return "a value";
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
