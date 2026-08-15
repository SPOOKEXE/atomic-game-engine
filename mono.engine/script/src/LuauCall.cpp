// `ScriptCall` in Luau's currency.
//
// **One of the two files that has met a VM on behalf of the neutral methods.**
// Everything in `ScriptMethods.cpp` and in the five neutral service files is
// written against the interface; this is what a stack slot, a tagged userdata and
// a raised error look like on this side, and `JsCall.cpp` is the same thing said
// in the other language.
//
// **Two trampolines rather than one closure per method**, and they differ only
// in where the row comes from. An instance method's table is one table, so its
// closure carries a row *index*; a service has a table of its own, so its closure
// carries the row's *address* - which is safe because a `ServiceMethod` lives in
// a `static constexpr` array for the life of the program. Adding a method of
// either kind costs a row in its own file and nothing here.
//
// **Neither checks the same thing.** An instance method may assume `Subject()`
// names a real row, because the receiver is checked in the adapter's
// constructor; a service method's receiver is its own table and is not checked
// at all, because there is nothing useful to say about it.
//
// **`LuauServiceIndex` and `LuauServiceNewIndex` are a third and a fourth**,
// added when a service property became data. They are here for the trampolines'
// reason rather than beside `InstallService`: a getter and a setter are
// `ScriptMethod`s, so reaching one means building a `LuauCall`, and this file is
// where the neutral layer meets the VM. What is different about them is that the
// key sits in the slot the first argument would - see `DropPropertyKey`.
//
// @tier L9 · shared

#include "LuauBindings.hpp"
#include "ScriptCall.hpp"
#include "Subtree.hpp"

#include <engine/ecs/Attributes.hpp>

#include <cstdint>
#include <cstring>
#include <lualib.h>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::AttributeValue;
		using ecs::Entity;

		// Turns a Luau value into an attribute, by what it is rather than by what
		// a descriptor said it should be.
		//
		// **Userdata is checked by tag, in the order the vocabulary was added.** A
		// tag check is exact - `Vector2` and `UDim` are both two floats and only
		// the tag tells them apart - so the order here is legibility and not
		// correctness.
		//
		// **The scalars are checked by exact type and not by `lua_isnumber`.**
		// That predicate is true for a numeric *string*, so `SetAttribute("n",
		// "5")` used to store a `Double` where JavaScript stores a `String` and
		// where Roblox stores a string - a silent divergence between the two VMs
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
				// a game computes - a title, a state, a message - and `core::Name`
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
		// value this binding stored and can for one a future build wrote - so it
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

			// The same call on a service, whose receiver is its own table.
			//
			// **A tag rather than a flag**, so the two are told apart at the call
			// site: a service method's `Subject()` is `NULL_ENTITY` and nothing
			// checks slot one, because `ContentService:GetMeshes()` passes a
			// table there and `ContentService.GetMeshes()` passes nothing - and
			// neither is a mistake this layer can report better than the method
			// reading its first argument will.
			struct OnService {};

			LuauCall(lua_State *state, OnService)
				: State(state), Context(UpvalueContext(state)), Self(ecs::NULL_ENTITY) {}

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

			ActionStack &Actions() override {
				return Context.Actions;
			}

			uint64_t NextGuid() override {
				return Context.NextGuid++;
			}

			const HostRole &Role() const override {
				return Context.Role;
			}

			TweenTable &Tweens() override {
				return Context.Tweens;
			}

			DebrisQueue &Debris() override {
				return Context.Debris;
			}

			TopicSubscriptions &Subscriptions() override {
				return Context.Subscriptions;
			}

			ChildWaiters &Waiters() override {
				return Context.Waiters;
			}

			Entity Subject() const override {
				return Self;
			}

			// Whether the method suspended the thread rather than answering.
			//
			// **Read by both trampolines and by nothing else.** `lua_yield` takes
			// its results from the stack top at the moment the C function returns,
			// so the yield has to *be* the return - see `Await`. The service
			// trampoline has asked since the store services suspended; the
			// instance one asks because `WaitForChild` does.
			bool Suspended() const {
				return Yielded;
			}

			bool IsNil(size_t index) const override {
				return lua_isnoneornil(State, Slot(index));
			}

			size_t Arguments() const override {
				// The receiver's slot is not an argument, and a stack shorter
				// than it cannot happen - the trampolines are what put the
				// receiver there.
				const int top = lua_gettop(State);
				return top <= RECEIVER ? 0u : static_cast<size_t>(top - RECEIVER);
			}

			bool ReadEnum(size_t index, Name enumName, Name &member) override {
				return ReadEnumValue(State, Slot(index), enumName, member);
			}

			std::string AsString(size_t index) override {
				size_t length = 0;
				const char *text = luaL_checklstring(State, Slot(index), &length);

				// `luaL_checklstring` rather than `luaL_checkstring`, so an
				// embedded zero in a string a script built survives instead of
				// truncating the value at it.
				return std::string(text, length);
			}

			double AsNumber(size_t index) override {
				// **By exact type and not `luaL_checknumber`, which accepts a
				// numeric *string*.** That leniency is Lua's and the JavaScript
				// adapter has no equivalent - `JS_IsNumber` is false for `"5"` -
				// so leaving it in place would mean `BindActionAtPriority(...,
				// "5", ...)` binding at priority five in one language and
				// throwing in the other. The same call, twice, with two answers.
				//
				// This is `ReadLuauAttribute`'s rule one door along, and for its
				// reason: that reader stopped using `lua_isnumber` when
				// `SetAttribute("n", "5")` was found storing a number where
				// JavaScript and Roblox both store a string.
				if (lua_type(State, Slot(index)) != LUA_TNUMBER) {
					luaL_typeerrorL(State, Slot(index), "number");
				}
				return lua_tonumber(State, Slot(index));
			}

			bool OptionalBoolean(size_t index, bool fallback) override {
				// `lua_toboolean` and not a type check, which is Luau's own
				// truthiness - see the interface for why the two languages are
				// allowed to disagree about `0` here.
				return lua_isnoneornil(State, Slot(index)) ? fallback
														   : lua_toboolean(State, Slot(index)) != 0;
			}

			core::CFrame AsCFrame(size_t index) override {
				return CheckCFrame(State, Slot(index));
			}

			const core::TweenInfo &AsTweenInfo(size_t index) override {
				return CheckTweenInfoValue(State, Slot(index));
			}

			ecs::Entity AsInstance(size_t index) override {
				return CheckInstanceArgument(State, Slot(index));
			}

			void ReadAttribute(size_t index, AttributeValue &out) override {
				out = ReadLuauAttribute(State, Slot(index));
			}

			bool ReadValue(size_t index, ScriptValue &out, CodecStatus &why) override {
				// **The shared walker and never a second one.** `Codec.hpp`
				// decides which tables are arrays, what a cycle is and what a key
				// becomes, so a value published on a bus and a value written as
				// JSON are one shape rather than two that agree until somebody
				// edits one.
				return ReadScriptValue(State, Slot(index), out, 0, why);
			}

			void ReadFieldNames(size_t index, std::vector<std::string> &out) override {
				const int table = Slot(index);
				luaL_checktype(State, table, LUA_TTABLE);

				// **The key is tested rather than converted.** `lua_tostring` on a
				// number key rewrites the value on the stack, which is exactly
				// what `lua_next` is documented to break on - a record with a
				// numeric key would otherwise end the walk somewhere arbitrary.
				lua_pushnil(State);
				while (lua_next(State, table) != 0) {
					if (lua_type(State, -2) != LUA_TSTRING) {
						// The value and the key both go, so the stack is clean
						// before the raise unwinds out of here.
						lua_pop(State, 2);
						Raise("every field of this record is named by a string");
					}

					size_t length = 0;
					const char *text = lua_tolstring(State, -2, &length);
					out.emplace_back(text, length);
					lua_pop(State, 1);
				}
			}

			bool ReadFieldProperty(
				size_t index, const std::string &field, ecs::PropertyType type, Name enumName, void *out
			) override {
				lua_getfield(State, Slot(index), field.c_str());

				// An absolute index, because `ReadPropertyValue` pushes for some
				// types and a relative one would be wrong the moment it did.
				const bool read = ReadPropertyValue(State, lua_gettop(State), type, enumName, out);
				lua_pop(State, 1);
				return read;
			}

			bool ReadProperty(size_t index, ecs::PropertyType type, Name enumName, void *out) override {
				return ReadPropertyValue(State, Slot(index), type, enumName, out);
			}

			CallbackRef RetainCallback(size_t index) override {
				luaL_checktype(State, Slot(index), LUA_TFUNCTION);

				lua_pushvalue(State, Slot(index));
				const int reference = lua_ref(State, -1);
				lua_pop(State, 1);
				return reference;
			}

			void ReleaseCallback(CallbackRef callback) override {
				lua_unref(State, callback);
			}

			void ConnectOnce(SignalKind kind, ecs::Entity subject, CallbackRef callback) override {
				Context.Signals.MarkOnce(Context.Signals.Connect(kind, subject, callback));
			}

			void ReturnNil() override {
				lua_pushnil(State);
				Pushed++;
			}

			void ReturnBoolean(bool value) override {
				lua_pushboolean(State, value ? 1 : 0);
				Pushed++;
			}

			void ReturnNumber(double value) override {
				lua_pushnumber(State, value);
				Pushed++;
			}

			void ReturnString(std::string_view value) override {
				// Length-carrying, so a byte string a script built keeps its
				// zeroes instead of truncating at the first one.
				//
				// **Never a null pointer, even for a length of zero.**
				// `lua_pushlstring` traps on one rather than treating it as
				// empty, and a default-constructed `string_view` is exactly that
				// - which is what an invalid `core::Name::Text()` hands back.
				lua_pushlstring(State, value.empty() ? "" : value.data(), value.size());
				Pushed++;
			}

			void ReturnStrings(std::span<const std::string_view> values) override {
				lua_createtable(State, static_cast<int>(values.size()), 0);
				for (size_t index = 0; index < values.size(); index++) {
					const std::string_view text = values[index];
					lua_pushlstring(State, text.empty() ? "" : text.data(), text.size());
					lua_rawseti(State, -2, static_cast<int>(index) + 1);
				}
				Pushed++;
			}

			void ReturnInstances(std::span<const Entity> values) override {
				lua_createtable(State, static_cast<int>(values.size()), 0);
				for (size_t index = 0; index < values.size(); index++) {
					PushInstanceValue(State, values[index]);
					lua_rawseti(State, -2, static_cast<int>(index) + 1);
				}
				Pushed++;
			}

			void ReturnValue(const ScriptValue &value) override {
				PushScriptValue(State, value);
				Pushed++;
			}

			void ReturnCFrame(const core::CFrame &value) override {
				*PushCFrame(State) = value;
				Pushed++;
			}

			void ReturnVector2(const core::Vector2 &value) override {
				*PushVector2(State) = value;
				Pushed++;
			}

			void ReturnEnum(Name enumName, Name member) override {
				PushEnumItem(State, enumName, member);
				Pushed++;
			}

			void ReturnEnums(Name enumName, std::span<const Name> members) override {
				lua_createtable(State, static_cast<int>(members.size()), 0);
				for (size_t index = 0; index < members.size(); index++) {
					PushEnumItem(State, enumName, members[index]);
					lua_rawseti(State, -2, static_cast<int>(index) + 1);
				}
				Pushed++;
			}

			void ReturnInputObjects(std::span<const InputReport> reports) override {
				lua_createtable(State, static_cast<int>(reports.size()), 0);
				for (size_t index = 0; index < reports.size(); index++) {
					PushInputObject(State, reports[index]);
					lua_rawseti(State, -2, static_cast<int>(index) + 1);
				}
				Pushed++;
			}

			void ReturnBoundAction(const BoundActionReport &report) override {
				PushBoundAction(report);
				Pushed++;
			}

			void ReturnBoundActions(std::span<const BoundActionReport> reports) override {
				// **Keyed by name rather than an array**, which is Roblox's shape
				// and the one a caller wants: the question this answers is "what
				// has claimed E", and the name is how anything is unbound
				// afterwards.
				lua_createtable(State, 0, static_cast<int>(reports.size()));
				for (const BoundActionReport &report : reports) {
					PushBoundAction(report);
					lua_setfield(State, -2, std::string(report.Name).c_str());
				}
				Pushed++;
			}

			void ReturnInstance(ecs::Entity value) override {
				// **Nil for a null, and `PushInstanceValue` does not do that.**
				// It is the raw pusher: it makes a userdata whatever it is
				// handed, so a null entity came out as an instance handle to
				// nothing and `if player then` was true for a player who does
				// not exist. `LuauInstances.cpp` has always had a separate
				// `PushFound` for exactly this, and every Luau lookup uses it -
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

			void ReturnTween(ecs::Entity tween) override {
				PushTween(State, tween);
				Pushed++;
			}

			void ForgetSubject(ecs::Entity subject) override {
				std::vector<CallbackRef> released;
				Context.Signals.DropSubject(subject, released);
				for (const CallbackRef reference : released) {
					lua_unref(State, reference);
				}
			}

			void Forget(ecs::Entity instance) override {
				lua_State *state = State;
				ForgetSubtree(
					*Context.World,
					Context.Signals,
					Context.Changes,
					instance,
					[state](CallbackRef reference) { lua_unref(state, reference); }
				);
			}

			void Await(uint64_t ticket) override {
				Suspend(Context.AwaitedTickets, ticket);
			}

			void AwaitChild(uint64_t waiter) override {
				// **The same suspension under a different key, which is the whole
				// of what a second resume source costs this adapter.** What
				// differs is only who comes back for the thread -
				// `PumpChildWaiters` rather than `PumpDeliveries` - and what it
				// resumes with.
				Suspend(Context.AwaitedChildren, waiter);
			}

			[[noreturn]] void Raise(const char *message) override {
				// `"%s"` rather than the message as the format, because a message
				// this file did not write may contain a percent and `luaL_errorL`
				// would read it as a conversion.
				luaL_errorL(State, "%s", message);
			}

		  private:
			// Parks the running thread until something keyed on `key` comes back
			// for it.
			//
			// **The thread is kept alive by a registry ref**, because nothing else
			// holds a suspended coroutine: the script that started it has returned
			// into this call and the barrier is what resumes it.
			//
			// **`insert_or_assign`, not `emplace`.** A resumed script that
			// suspends again does so on the same thread, and `emplace` would keep
			// the *old* reference - so the fresh one would leak and the next
			// resume would find a thread nothing was holding.
			//
			// @param waiting Which table the resume will look in.
			// @param key     What that table keys this thread on.
			void Suspend(std::unordered_map<uint64_t, lua_State *> &waiting, uint64_t key) {
				lua_pushthread(State);
				lua_xmove(State, Context.State, 1);
				const int reference = lua_ref(Context.State, -1);
				lua_pop(Context.State, 1);

				Context.Threads.insert_or_assign(State, reference);
				waiting.insert_or_assign(key, State);
				Yielded = true;
			}

			// One action's record, left on the stack.
			//
			// Roblox's four fields, and the two it does not have are absent from
			// the *binding* rather than from the record: `title` and
			// `description` come from `SetTitle` and `SetDescription`, which
			// decorate a touch button, and there is no touch surface - so
			// reporting them as empty strings would claim they had been set to
			// nothing.
			void PushBoundAction(const BoundActionReport &report) {
				lua_createtable(State, 0, 4);

				lua_createtable(State, static_cast<int>(report.Keys.size()), 0);
				for (size_t index = 0; index < report.Keys.size(); index++) {
					PushEnumItem(State, Name("KeyCode"), report.Keys[index]);
					lua_rawseti(State, -2, static_cast<int>(index) + 1);
				}
				lua_setfield(State, -2, "inputTypes");

				lua_pushinteger(State, report.Priority);
				lua_setfield(State, -2, "priorityLevel");

				lua_pushinteger(State, report.StackOrder);
				lua_setfield(State, -2, "stackOrder");

				lua_pushboolean(State, report.CreateTouchButton ? 1 : 0);
				lua_setfield(State, -2, "createTouchButton");
			}

			// Where the receiver sits, and therefore where the arguments start.
			static constexpr int RECEIVER = 1;

			int Slot(size_t index) const {
				return static_cast<int>(index) + RECEIVER + 1;
			}

			lua_State *State;
			LuauContext &Context;
			Entity Self;
			int Pushed = 0;
			bool Yielded = false;
		};

		// The one bound function every neutral method is installed as.
		//
		// Upvalue 1 is the context, which is what `UpvalueContext` reads and what
		// every other bound function in this module carries. Upvalue 2 is the row.
		int NeutralLuauMethod(lua_State *state) {
			const int row = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));

			LuauCall call(state);
			NeutralInstanceMethods()[static_cast<size_t>(row)].Function(call);

			// **A yield is the return and cannot be anything else**, which is the
			// service trampoline's rule arriving here with the first instance
			// method that suspends. `WaitForChild` is the only caller; every other
			// row leaves this false, and a method that answered *and* suspended
			// would be a bug this branch hides rather than one it makes.
			if (call.Suspended()) {
				return lua_yield(state, 0);
			}
			return call.Results();
		}

		// The one bound function every neutral *service* method is installed as.
		//
		// **Upvalue 2 is the row itself rather than an index into a table**,
		// which is where this differs from the instance trampoline above and
		// from `JsCall.cpp`'s: there is one instance method table and a service
		// has one of its own, so an index would need a table to index into and
		// that table would be state. A `ServiceMethod` row lives in a
		// `static constexpr` array for the life of the program, so a light
		// userdata pointing at one is a pointer that cannot dangle.
		int NeutralLuauServiceMethod(lua_State *state) {
			const auto *row =
				static_cast<const ServiceMethod *>(lua_tolightuserdata(state, lua_upvalueindex(2)));

			LuauCall call(state, LuauCall::OnService{});
			row->Function(call);

			// **A yield is the return and cannot be anything else.** `lua_yield`
			// takes the results from the stack top as it is called and hands back
			// the sentinel a C function must return, so the branch is here rather
			// than inside `Await` - which returns to a method body that then
			// returns to this line. The store services are the callers; every
			// other method leaves this false.
			if (call.Suspended()) {
				return lua_yield(state, 0);
			}
			return call.Results();
		}

		// The service a metamethod's second upvalue names.
		//
		// A light userdata for `NeutralLuauServiceMethod`'s reason one row up: a
		// `ServiceSurface` handed to `InstallService` has static storage duration,
		// so its address is a pointer that cannot dangle.
		const ServiceSurface &UpvalueSurface(lua_State *state) {
			return *static_cast<const ServiceSurface *>(lua_tolightuserdata(state, lua_upvalueindex(2)));
		}

		// The row one field names, or null.
		//
		// A linear walk, because the longest list is nine and a service property
		// is read by a script rather than by a loop over a scene.
		const ServiceProperty *PropertyNamed(const ServiceSurface &surface, const char *field) {
			for (const ServiceProperty &property : surface.Properties) {
				if (std::strcmp(property.Name, field) == 0) {
					return &property;
				}
			}
			return nullptr;
		}

		// Leaves the receiver and the arguments where `LuauCall` expects them.
		//
		// **The key is not an argument, and a metamethod puts it in slot two** -
		// which is where the first argument lives. Dropping it makes a getter's
		// call a zero-argument one and a setter's a one-argument one, so a
		// `ServiceProperty` reads argument **zero** the way every other neutral
		// body does rather than counting past a slot only this path has.
		//
		// **After the row is found and never before**: the string `field` points
		// into is on that slot, and removing it drops the last reference.
		void DropPropertyKey(lua_State *state) {
			lua_remove(state, 2);
		}
	}

	int LuauServiceIndex(lua_State *state) {
		const ServiceSurface &surface = UpvalueSurface(state);
		const char *field = luaL_checkstring(state, 2);

		if (const ServiceProperty *property = PropertyNamed(surface, field); property != nullptr) {
			DropPropertyKey(state);

			LuauCall call(state, LuauCall::OnService{});
			property->Get(call);
			return call.Results();
		}

		// The methods and the signals, from the shared table.
		//
		// **A userdata has no fields**, so `InstallService` stashes the table it
		// built - signals first, then methods - in the registry under
		// `MethodsKey`, and this reads it back. One key on the surface, two
		// readers, so the install and the lookup cannot name different ones.
		lua_getfield(state, LUA_REGISTRYINDEX, surface.MethodsKey);
		lua_pushvalue(state, 2);
		lua_rawget(state, -2);
		if (!lua_isnil(state, -1)) {
			return 1;
		}

		// **Raised rather than nil, which is the one place a userdata service is
		// stricter than a table one.** `__index` has to answer something for a
		// name nothing declares, and nil would make every typo silent.
		luaL_errorL(state, "%s has no member '%s'", surface.Name, field);
	}

	int LuauServiceNewIndex(lua_State *state) {
		const ServiceSurface &surface = UpvalueSurface(state);
		const char *field = luaL_checkstring(state, 2);

		// **One message for "no such member" and for "not writable"**, because
		// the two are the same fact from a script's side: this name cannot be
		// assigned. Naming the service as well as the field is what tells a
		// misspelling from a genuinely read-only property.
		const ServiceProperty *property = PropertyNamed(surface, field);
		if (property == nullptr || property->Set == nullptr) {
			luaL_errorL(state, "%s.%s is read-only", surface.Name, field);
		}

		DropPropertyKey(state);

		LuauCall call(state, LuauCall::OnService{});
		property->Set(call);
		return 0;
	}

	void InstallLuauServiceMethods(lua_State *state, std::span<const ServiceMethod> methods) {
		LuauContext &context = ContextOf(state);

		for (const ServiceMethod &method : methods) {
			// **The context as upvalue 1, on every method, without exception** -
			// `InstallService` states why, and this loop is bound by the same
			// rule because `UpvalueContext` reads index 1 either way.
			lua_pushlightuserdata(state, &context);
			lua_pushlightuserdata(state, const_cast<ServiceMethod *>(&method));
			lua_pushcclosure(state, NeutralLuauServiceMethod, method.Name, 2);
			lua_setfield(state, -2, method.Name);
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
