// `ScriptCall` in Luau's currency.
//
// **One of the two files that has met a VM on behalf of the neutral methods.**
// Everything in `ScriptMethods.cpp` is written against the interface; this is
// what a stack slot, a tagged userdata and a raised error look like on this side,
// and `JsCall.cpp` is the same thing said in the other language.
//
// **One trampoline rather than one closure per method.** The method table is
// walked at install time and every row gets the same `lua_CFunction` with its
// index on an upvalue, so adding a neutral method costs a row in
// `ScriptMethods.cpp` and nothing here.
//
// @tier L9 · shared

#include "Bindings.hpp"
#include "ScriptCall.hpp"

#include <engine/ecs/Attributes.hpp>

#include <lualib.h>
#include <span>
#include <string>
#include <utility>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::AttributeValue;
		using ecs::Entity;

		// Turns a Luau value into an attribute, by what it is rather than by what
		// a descriptor said it should be.
		//
		// **Userdata is checked by tag, in the order the vocabulary was added.** A
		// tag check is exact — `Vector2` and `UDim` are both two floats and only
		// the tag tells them apart — so the order here is legibility and not
		// correctness.
		//
		// **The scalars are checked by exact type and not by `lua_isnumber`.**
		// That predicate is true for a numeric *string*, so `SetAttribute("n",
		// "5")` used to store a `Double` where JavaScript stores a `String` and
		// where Roblox stores a string — a silent divergence between the two VMs
		// in the one method whose whole job is to hold what an author gave it.
		//
		// Leaves `Opaque` for anything with no attribute form, which the caller
		// turns into a refusal.
		AttributeValue ReadLuauAttribute(lua_State *state, int index) {
			AttributeValue value;

			switch (lua_type(state, index)) {
			case LUA_TBOOLEAN:
				value.Type = ecs::PropertyType::Bool;
				value.Bool = lua_toboolean(state, index) != 0;
				return value;
			case LUA_TNUMBER:
				// **A double and not an int, even for a whole number.** Luau has
				// one number type; guessing at an integer here would make
				// `SetAttribute("n", 1)` read back as an `Int32` and
				// `SetAttribute("n", 1.5)` as a `Double`, so a script that
				// incremented an attribute would change its own type halfway.
				value.Type = ecs::PropertyType::Double;
				value.Double = lua_tonumber(state, index);
				return value;
			case LUA_TSTRING: {
				// **`String` and never `Name`.** An attribute's value is something
				// a game computes — a title, a state, a message — and `core::Name`
				// is a registry that never releases. `ecs::PropertyType::String`
				// carries the whole argument, and this is the surface it was added
				// for.
				size_t length = 0;
				const char *text = lua_tolstring(state, index, &length);
				value.Type = ecs::PropertyType::String;
				value.String.assign(text, length);
				return value;
			}
			default:
				break;
			}

			if (lua_touserdatatagged(state, index, TAG_VECTOR3) != nullptr) {
				value.Type = ecs::PropertyType::Vector3;
				value.Vector3 = CheckVector3(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_COLOR3) != nullptr) {
				value.Type = ecs::PropertyType::Color3;
				value.Color3 = CheckColor3(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_CFRAME) != nullptr) {
				value.Type = ecs::PropertyType::CFrame;
				value.CFrame = CheckCFrame(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_VECTOR2) != nullptr) {
				value.Type = ecs::PropertyType::Vector2;
				value.Vector2 = CheckVector2Value(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_UDIM) != nullptr) {
				value.Type = ecs::PropertyType::UDim;
				value.UDim = CheckUDim(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_UDIM2) != nullptr) {
				value.Type = ecs::PropertyType::UDim2;
				value.UDim2 = CheckUDim2(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_RECT) != nullptr) {
				value.Type = ecs::PropertyType::Rect;
				value.Rect = CheckRect(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_NUMBER_RANGE) != nullptr) {
				value.Type = ecs::PropertyType::NumberRange;
				value.NumberRange = CheckNumberRange(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_NUMBER_SEQUENCE) != nullptr) {
				value.Type = ecs::PropertyType::NumberSequence;
				value.NumberSequence = CheckNumberSequence(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_COLOR_SEQUENCE) != nullptr) {
				value.Type = ecs::PropertyType::ColorSequence;
				value.ColorSequence = CheckColorSequence(state, index);
			}

			return value;
		}

		// Pushes a stored attribute.
		//
		// Returns `false` for a type with no Luau form, which cannot happen for a
		// value this binding stored and can for one a future build wrote — so it
		// is a refusal rather than an assert.
		bool PushLuauAttribute(lua_State *state, const AttributeValue &value) {
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				lua_pushboolean(state, value.Bool);
				return true;
			case ecs::PropertyType::Int32:
				lua_pushinteger(state, value.Int32);
				return true;
			case ecs::PropertyType::Int64:
				lua_pushnumber(state, static_cast<double>(value.Int64));
				return true;
			case ecs::PropertyType::Float:
				lua_pushnumber(state, value.Float);
				return true;
			case ecs::PropertyType::Double:
				lua_pushnumber(state, value.Double);
				return true;
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				lua_pushstring(state, value.Name.Text().data());
				return true;
			case ecs::PropertyType::String:
				lua_pushlstring(state, value.String.data(), value.String.size());
				return true;
			case ecs::PropertyType::Vector3:
				*PushVector3(state) = value.Vector3;
				return true;
			case ecs::PropertyType::Color3:
				*PushColor3(state) = value.Color3;
				return true;
			case ecs::PropertyType::CFrame:
				*PushCFrame(state) = value.CFrame;
				return true;
			case ecs::PropertyType::Vector2:
				*PushVector2(state) = value.Vector2;
				return true;
			case ecs::PropertyType::UDim:
				*PushUDim(state) = value.UDim;
				return true;
			case ecs::PropertyType::UDim2:
				*PushUDim2(state) = value.UDim2;
				return true;
			case ecs::PropertyType::Rect:
				*PushRect(state) = value.Rect;
				return true;
			case ecs::PropertyType::NumberRange:
				*PushNumberRange(state) = value.NumberRange;
				return true;
			case ecs::PropertyType::NumberSequence:
				*PushNumberSequence(state) = value.NumberSequence;
				return true;
			case ecs::PropertyType::ColorSequence:
				*PushColorSequence(state) = value.ColorSequence;
				return true;
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				break;
			}
			return false;
		}

		// One call, on the Luau stack.
		//
		// **`self` is slot one and the first argument is slot two**, which is what
		// `Slot` below is for: a neutral method counts from the first thing the
		// author wrote, and only this file knows the receiver takes a slot.
		class LuauCall final : public ScriptCall {
		  public:
			// **The receiver is checked in the constructor**, so a method body
			// never sees a call that is not on an instance. `CheckInstanceArgument`
			// raises for anything else, which unwinds out of here before the
			// object exists.
			explicit LuauCall(lua_State *state)
				: State(state), Context(UpvalueContext(state)), Self(CheckInstanceArgument(state, RECEIVER)) {
			}

			// How many values the method left on the stack.
			int Results() const {
				return Pushed;
			}

			ecs::Store &World() override {
				return *Context.World;
			}

			ChangeQueue &Changes() override {
				return Context.Changes;
			}

			Entity Subject() const override {
				return Self;
			}

			bool IsNil(size_t index) const override {
				return lua_isnoneornil(State, Slot(index));
			}

			std::string AsString(size_t index) override {
				size_t length = 0;
				const char *text = luaL_checklstring(State, Slot(index), &length);

				// `luaL_checklstring` rather than `luaL_checkstring`, so an
				// embedded zero in a string a script built survives instead of
				// truncating the value at it.
				return std::string(text, length);
			}

			core::CFrame AsCFrame(size_t index) override {
				return CheckCFrame(State, Slot(index));
			}

			ecs::Entity AsInstance(size_t index) override {
				return CheckInstanceArgument(State, Slot(index));
			}

			void ReadAttribute(size_t index, AttributeValue &out) override {
				out = ReadLuauAttribute(State, Slot(index));
			}

			void ReturnNil() override {
				lua_pushnil(State);
				Pushed++;
			}

			void ReturnBoolean(bool value) override {
				lua_pushboolean(State, value ? 1 : 0);
				Pushed++;
			}

			void ReturnCFrame(const core::CFrame &value) override {
				*PushCFrame(State) = value;
				Pushed++;
			}

			void ReturnInstance(ecs::Entity value) override {
				// **Nil for a null, and `PushInstanceValue` does not do that.**
				// It is the raw pusher: it makes a userdata whatever it is
				// handed, so a null entity came out as an instance handle to
				// nothing and `if player then` was true for a player who does
				// not exist. `Instances.cpp` has always had a separate
				// `PushFound` for exactly this, and every Luau lookup uses it —
				// which is precisely why the first version of this line looked
				// right and was not.
				//
				// Caught by the parity case: JavaScript's `MakeJsInstance`
				// answers `JS_NULL` for a null and the two languages disagreed.
				if (value == ecs::NULL_ENTITY) {
					lua_pushnil(State);
				} else {
					PushInstanceValue(State, value);
				}
				Pushed++;
			}

			void ReturnAttribute(const AttributeValue &value) override {
				if (!PushLuauAttribute(State, value)) {
					lua_pushnil(State);
				}
				Pushed++;
			}

			void ReturnAttributes(std::span<const std::pair<Name, AttributeValue>> values) override {
				lua_newtable(State);
				for (const auto &entry : values) {
					if (!PushLuauAttribute(State, entry.second)) {
						continue;
					}
					lua_setfield(State, -2, entry.first.Text().data());
				}
				Pushed++;
			}

			void ReturnSignal(SignalKind kind, Name property) override {
				PushSignal(State, kind, Self, property);
				Pushed++;
			}

			[[noreturn]] void Raise(const char *message) override {
				// `"%s"` rather than the message as the format, because a message
				// this file did not write may contain a percent and `luaL_errorL`
				// would read it as a conversion.
				luaL_errorL(State, "%s", message);
			}

		  private:
			// Where the receiver sits, and therefore where the arguments start.
			static constexpr int RECEIVER = 1;

			int Slot(size_t index) const {
				return static_cast<int>(index) + RECEIVER + 1;
			}

			lua_State *State;
			LuauContext &Context;
			Entity Self;
			int Pushed = 0;
		};

		// The one bound function every neutral method is installed as.
		//
		// Upvalue 1 is the context, which is what `UpvalueContext` reads and what
		// every other bound function in this module carries. Upvalue 2 is the row.
		int NeutralLuauMethod(lua_State *state) {
			const int row = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));

			LuauCall call(state);
			NeutralInstanceMethods()[static_cast<size_t>(row)].Function(call);
			return call.Results();
		}
	}

	void InstallLuauNeutralMethods(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// Into the table `OpenInstances` built, rather than a second one: the
		// editor's vocabulary walks that registry entry, so a neutral method is
		// offered in the script editor with nothing else changing.
		lua_getfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");

		int row = 0;
		for (const InstanceMethod &method : NeutralInstanceMethods()) {
			lua_pushlightuserdata(state, &context);
			lua_pushinteger(state, row++);
			lua_pushcclosure(state, NeutralLuauMethod, method.Name, 2);
			lua_setfield(state, -2, method.Name);
		}

		lua_pop(state, 1);
	}
}
