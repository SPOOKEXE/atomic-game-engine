// `ScriptCall` in JavaScript's currency.
//
// The twin of `LuauCall.cpp`, and deliberately the same shape: one adapter, two
// trampolines - one for an instance method and one for a service's - and a table
// walked at install time, so a neutral method costs a row in its own file and
// nothing here.
//
// ## The one genuinely different mechanism
//
// **A refusal has to unwind, and QuickJS has no way to unwind.** Luau raises by
// throwing out of `luaL_errorL`, so a Luau method body can read an argument and
// never check; QuickJS reports an error by *returning* `JS_EXCEPTION`, which a
// shared method body cannot do because it returns nothing.
//
// So `Raise` records the error with the context in the ordinary way and then
// throws a private type that only the trampoline below catches, which is what
// makes `[[noreturn]]` true in both languages and lets one method body be
// written against it. Nothing escapes into QuickJS: the throw and the catch are
// both in C++ frames of this file, and the C function QuickJS actually called is
// the one doing the catching.
//
// @tier L9 · shared

#include "JsBindings.hpp"

#include <engine/ecs/Attributes.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/Subtree.hpp>

#include <cstdint>
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

		// What `JsCall::Raise` throws, so the trampoline can tell a refusal it
		// made from a fault it did not.
		//
		// Carries nothing: the message is already on the context, where every
		// other JavaScript error in this module puts it.
		struct JsRaised {};

		// One stored attribute as the JavaScript value it holds.
		//
		// `JS_NULL` for a type with no form here, which cannot happen for a value
		// this binding stored and can for one a future build wrote - so it is a
		// fallback rather than an assert.
		//
		// **`Name` and `Enum` land as plain strings**, matching the Luau half:
		// neither is an `EnumItem`, because an attribute carries no enum name to
		// say which set a member belongs to.
		JSValue MakeJsAttribute(JSContext *context, const AttributeValue &value) {
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				return JS_NewBool(context, value.Bool);
			case ecs::PropertyType::Int32:
				return JS_NewInt32(context, value.Int32);
			case ecs::PropertyType::Int64:
				return JS_NewFloat64(context, static_cast<double>(value.Int64));
			case ecs::PropertyType::Float:
				return JS_NewFloat64(context, value.Float);
			case ecs::PropertyType::Double:
				return JS_NewFloat64(context, value.Double);
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				return JS_NewString(context, value.Name.Text().data());
			case ecs::PropertyType::String:
				return JS_NewStringLen(context, value.String.data(), value.String.size());
			case ecs::PropertyType::Vector3:
				return MakeVector3(context, value.Vector3);
			case ecs::PropertyType::Color3:
				return MakeColor3(context, value.Color3);
			case ecs::PropertyType::CFrame:
				return MakeCFrame(context, value.CFrame);
			case ecs::PropertyType::Vector2:
				return MakeVector2(context, value.Vector2);
			case ecs::PropertyType::UDim:
				return MakeUDim(context, value.UDim);
			case ecs::PropertyType::UDim2:
				return MakeUDim2(context, value.UDim2);
			case ecs::PropertyType::Rect:
				return MakeRect(context, value.Rect);
			case ecs::PropertyType::NumberRange:
				return MakeNumberRange(context, value.NumberRange);
			case ecs::PropertyType::NumberSequence:
				return MakeNumberSequence(context, value.NumberSequence);
			case ecs::PropertyType::ColorSequence:
				return MakeColorSequence(context, value.ColorSequence);
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				break;
			}
			return JS_NULL;
		}

		// One call, on a QuickJS argument vector.
		//
		// **`this` is the receiver and `argv` starts at the first argument**,
		// which is the whole of what differs from the Luau adapter's slot
		// arithmetic - and the reason the neutral methods count from zero.
		class JsCall final : public ScriptCall {
		  public:
			JsCall(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
				: Context(context), Argv(argv), Argc(argc < 0 ? 0u : static_cast<size_t>(argc)),
				  Self(JsEntityOf(context, self)) {}

			// The same call on a service, whose receiver stands for nothing.
			//
			// **A tag rather than a flag**, matching `LuauCall::OnService`: a
			// service method's `Subject()` is `NULL_ENTITY`, and asking
			// `JsEntityOf` about a plain object would answer that anyway - the
			// tag is what says it was meant.
			struct OnService {};

			JsCall(JSContext *context, OnService, int argc, JSValueConst *argv)
				: Context(context), Argv(argv), Argc(argc < 0 ? 0u : static_cast<size_t>(argc)),
				  Self(ecs::NULL_ENTITY) {}

			~JsCall() override {
				// Results the trampoline never took, because something raised
				// after they were set. Empty on the ordinary path, so this costs
				// nothing.
				for (JSValue &value : Results) {
					JS_FreeValue(Context, value);
				}
			}

			// Hands the result over.
			//
			// **`undefined` for nothing, the value itself for one, and an
			// `Array` for several.** A method with more than one answer is a Luau
			// shape - `ScriptCall` states the trade at the `Return` block - and an
			// array is what a JavaScript author destructures. Nothing packs a
			// single answer, so `GetPivot()` is a `CFrame` here exactly as it is
			// there.
			JSValue Take() {
				if (Results.empty()) {
					return JS_UNDEFINED;
				}

				if (Results.size() == 1) {
					const JSValue only = Results[0];
					Results.clear();
					return only;
				}

				JSValue array = JS_NewArray(Context);
				for (size_t index = 0; index < Results.size(); index++) {
					JS_SetPropertyUint32(Context, array, static_cast<uint32_t>(index), Results[index]);
				}
				Results.clear();
				return array;
			}

			ecs::Store &World() override {
				return *JsOf(Context).World;
			}

			ChangeQueue &Changes() override {
				return JsOf(Context).Changes;
			}

			ActionStack &Actions() override {
				return JsOf(Context).Actions;
			}

			uint64_t NextGuid() override {
				return JsOf(Context).NextGuid++;
			}

			const HostRole &Role() const override {
				return JsOf(Context).Role;
			}

			TweenTable &Tweens() override {
				return JsOf(Context).Tweens;
			}

			DebrisQueue &Debris() override {
				return JsOf(Context).Debris;
			}

			TopicSubscriptions &Subscriptions() override {
				return JsOf(Context).Subscriptions;
			}

			ChildWaiters &Waiters() override {
				return JsOf(Context).Waiters;
			}

			Entity Subject() const override {
				return Self;
			}

			bool IsNil(size_t index) const override {
				return index >= Argc || JS_IsNull(Argv[index]) || JS_IsUndefined(Argv[index]);
			}

			size_t Arguments() const override {
				return Argc;
			}

			bool ReadEnum(size_t index, Name enumName, Name &member) override {
				return index < Argc && ReadJsEnumValue(Context, Argv[index], enumName, member);
			}

			std::string AsString(size_t index) override {
				// **A string or a number and nothing else**, which is what
				// `luaL_checklstring` accepts on the other side. Letting
				// `JS_ToCString` decide would turn `AddTag({})` into the tag
				// `"[object Object]"` where Luau raises - a wrong argument
				// answered in one language and refused in the other.
				if (index >= Argc || !(JS_IsString(Argv[index]) || JS_IsNumber(Argv[index]))) {
					Raise("expected a string");
				}

				size_t length = 0;
				const char *text = JS_ToCStringLen(Context, &length, Argv[index]);
				if (text == nullptr) {
					Raise("expected a string");
				}

				std::string value(text, length);
				JS_FreeCString(Context, text);
				return value;
			}

			double AsNumber(size_t index) override {
				// **By exact type, matching `luaL_checknumber`'s refusal of a
				// table and deliberately *not* matching its acceptance of a
				// numeric string.** `JS_ToFloat64` would take `"5"` and would
				// also take `[]` as zero, which is the class of coercion
				// `AsString` already refuses one member up.
				double value = 0.0;
				if (index >= Argc || !JS_IsNumber(Argv[index]) ||
					JS_ToFloat64(Context, &value, Argv[index]) != 0) {
					Raise("expected a number");
				}
				return value;
			}

			bool OptionalBoolean(size_t index, bool fallback) override {
				// `JS_ToBool` and not a type check, which is this language's own
				// truthiness - see the interface for why the two are allowed to
				// disagree about `0` here.
				return IsNil(index) ? fallback : JS_ToBool(Context, Argv[index]) == 1;
			}

			core::CFrame AsCFrame(size_t index) override {
				// Qualified, because this member shares the free function's name.
				const core::CFrame *frame = index < Argc ? script::AsCFrame(Context, Argv[index]) : nullptr;
				if (frame == nullptr) {
					Raise("expected a CFrame");
				}
				return *frame;
			}

			const core::TweenInfo &AsTweenInfo(size_t index) override {
				// Qualified, because this member shares the free function's name.
				const core::TweenInfo *info =
					index < Argc ? script::AsTweenInfo(Context, Argv[index]) : nullptr;
				if (info == nullptr) {
					Raise("expected a TweenInfo");
				}
				return *info;
			}

			ecs::Entity AsInstance(size_t index) override {
				// **Checked rather than taken**, unlike `JsEntityOf` on its own:
				// that answers a null entity for anything at all, so a script
				// passing a string would have been reading whatever row zero is
				// rather than being told it passed the wrong thing. The Luau side
				// has always raised here and the two must agree.
				const ecs::Entity entity = index < Argc ? JsEntityOf(Context, Argv[index]) : ecs::NULL_ENTITY;
				if (entity == ecs::NULL_ENTITY) {
					Raise("expected an Instance");
				}
				return entity;
			}

			bool ReadPlacements(
				size_t index, std::vector<ecs::Entity> &instances, std::vector<core::CFrame> &frames
			) override {
				instances.clear();
				frames.clear();

				if (index + 1 >= Argc) {
					Raise("expected two arrays");
				}

				// **Every value fetched is freed**, which is the half of this
				// the Luau adapter has no equivalent of: `JS_GetPropertyUint32`
				// hands back a reference and QuickJS counts them, so a walk that
				// dropped one leaks a part handle per element for the life of
				// the runtime.
				const auto lengthOf = [&](JSValueConst array) {
					JSValue held = JS_GetPropertyStr(Context, array, "length");
					uint32_t length = 0;
					JS_ToUint32(Context, &length, held);
					JS_FreeValue(Context, held);
					return length;
				};

				const uint32_t count = lengthOf(Argv[index]);
				const uint32_t placed = lengthOf(Argv[index + 1]);

				instances.reserve(count);
				frames.reserve(count);

				for (uint32_t at = 0; at < count; at++) {
					JSValue item = JS_GetPropertyUint32(Context, Argv[index], at);
					const ecs::Entity instance = JsEntityOf(Context, item);
					JS_FreeValue(Context, item);
					if (instance == ecs::NULL_ENTITY) {
						Raise("expected an Instance");
					}
					instances.push_back(instance);
				}

				for (uint32_t at = 0; at < placed; at++) {
					JSValue item = JS_GetPropertyUint32(Context, Argv[index + 1], at);
					const core::CFrame *frame = script::AsCFrame(Context, item);
					const core::CFrame copied = frame == nullptr ? core::CFrame{} : *frame;
					JS_FreeValue(Context, item);
					if (frame == nullptr) {
						Raise("expected a CFrame");
					}
					frames.push_back(copied);
				}

				return count == placed;
			}

			void ReadAttribute(size_t index, AttributeValue &out) override {
				out = AttributeValue{};
				if (index >= Argc) {
					return;
				}

				const JSValueConst value = Argv[index];

				// The scalars by exact type, matching the Luau adapter: a
				// numeric string is a string in both languages.
				if (JS_IsBool(value)) {
					out.Type = ecs::PropertyType::Bool;
					out.Bool = JS_ToBool(Context, value) == 1;
					return;
				}
				if (JS_IsNumber(value)) {
					// **A double and not an int, even for a whole number.**
					// JavaScript has one number type; guessing at an integer
					// would make a script that incremented an attribute change
					// its own type halfway.
					out.Type = ecs::PropertyType::Double;
					JS_ToFloat64(Context, &out.Double, value);
					return;
				}
				if (JS_IsString(value)) {
					size_t length = 0;
					if (const char *text = JS_ToCStringLen(Context, &length, value); text != nullptr) {
						out.Type = ecs::PropertyType::String;
						out.String.assign(text, length);
						JS_FreeCString(Context, text);
					}
					return;
				}

				// **The datatypes by class id, in the order the vocabulary was
				// added.** A class check is exact - `Vector2` and `UDim` are both
				// two floats - so the order is legibility and not correctness.
				if (const core::Vector3 *held = AsVector3(Context, value); held != nullptr) {
					out.Type = ecs::PropertyType::Vector3;
					out.Vector3 = *held;
				} else if (const core::Color3 *colour = AsColor3(Context, value); colour != nullptr) {
					out.Type = ecs::PropertyType::Color3;
					out.Color3 = *colour;
				} else if (const core::CFrame *frame = script::AsCFrame(Context, value); frame != nullptr) {
					out.Type = ecs::PropertyType::CFrame;
					out.CFrame = *frame;
				} else if (const core::Vector2 *flat = AsVector2(Context, value); flat != nullptr) {
					out.Type = ecs::PropertyType::Vector2;
					out.Vector2 = *flat;
				} else if (const core::UDim *udim = AsUDim(Context, value); udim != nullptr) {
					out.Type = ecs::PropertyType::UDim;
					out.UDim = *udim;
				} else if (const core::UDim2 *udim2 = AsUDim2(Context, value); udim2 != nullptr) {
					out.Type = ecs::PropertyType::UDim2;
					out.UDim2 = *udim2;
				} else if (const core::Rect *rect = AsRect(Context, value); rect != nullptr) {
					out.Type = ecs::PropertyType::Rect;
					out.Rect = *rect;
				} else if (const core::NumberRange *range = AsNumberRange(Context, value); range != nullptr) {
					out.Type = ecs::PropertyType::NumberRange;
					out.NumberRange = *range;
				} else if (const core::NumberSequence *curve = AsNumberSequence(Context, value);
						   curve != nullptr) {
					out.Type = ecs::PropertyType::NumberSequence;
					out.NumberSequence = *curve;
				} else if (const core::ColorSequence *ramp = AsColorSequence(Context, value);
						   ramp != nullptr) {
					out.Type = ecs::PropertyType::ColorSequence;
					out.ColorSequence = *ramp;
				}
			}

			bool ReadValue(size_t index, ScriptValue &out, CodecStatus &why) override {
				// **The shared walker and never a second one** - see the Luau
				// adapter, which says why one table rule cannot be written twice.
				if (index >= Argc) {
					why = CodecStatus::Unsupported;
					return false;
				}
				return ToScriptValue(Context, Argv[index], out, 0, why);
			}

			void ReadFieldNames(size_t index, std::vector<std::string> &out) override {
				if (index >= Argc || !JS_IsObject(Argv[index])) {
					Raise("expected an object of named fields");
				}

				JSPropertyEnum *properties = nullptr;
				uint32_t count = 0;
				if (JS_GetOwnPropertyNames(
						Context, &properties, &count, Argv[index], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
					) != 0) {
					Raise("could not read the fields of this record");
				}

				// **Copied out and every atom freed before anything else runs.**
				// `JS_GetOwnPropertyNames` hands back reference-counted atoms, and
				// the caller reads a value per name - which may raise - so a walk
				// that held them would strand one per field it had taken.
				for (uint32_t next = 0; next < count; next++) {
					if (const char *text = JS_AtomToCString(Context, properties[next].atom);
						text != nullptr) {
						out.emplace_back(text);
						JS_FreeCString(Context, text);
					}
					JS_FreeAtom(Context, properties[next].atom);
				}
				js_free(Context, properties);
			}

			bool ReadFieldProperty(
				size_t index, const std::string &field, ecs::PropertyType type, Name enumName, void *out
			) override {
				JSValue value = JS_GetPropertyStr(Context, Argv[index], field.c_str());
				const bool read = FromJsValue(Context, value, type, enumName, out);
				JS_FreeValue(Context, value);
				return read;
			}

			bool ReadProperty(size_t index, ecs::PropertyType type, Name enumName, void *out) override {
				return index < Argc && FromJsValue(Context, Argv[index], type, enumName, out);
			}

			CallbackRef RetainCallback(size_t index) override {
				if (index >= Argc || !JS_IsFunction(Context, Argv[index])) {
					Raise("expected a function");
				}
				return Retain(Context, Argv[index]);
			}

			void ReleaseCallback(CallbackRef callback) override {
				Release(Context, callback);
			}

			void ConnectOnce(SignalKind kind, ecs::Entity subject, CallbackRef callback) override {
				SignalTable &signals = JsOf(Context).Signals;
				signals.MarkOnce(signals.Connect(kind, subject, callback));
			}

			void ReturnNil() override {
				// **`null` rather than `undefined`**, matching every other
				// JavaScript instance method: `FindFirstChild` answers `null` for
				// nothing found, and a method that answered `undefined` instead
				// would make `=== null` wrong for half the surface.
				Set(JS_NULL);
			}

			void ReturnBoolean(bool value) override {
				Set(JS_NewBool(Context, value));
			}

			void ReturnNumber(double value) override {
				Set(JS_NewFloat64(Context, value));
			}

			void ReturnString(std::string_view value) override {
				Set(JS_NewStringLen(Context, value.data(), value.size()));
			}

			void ReturnStrings(std::span<const std::string_view> values) override {
				// **A real `Array` and not an object with numeric keys**, which
				// is what the Luau half's one-based table means here: `map`,
				// `filter` and `length` are how a JavaScript author reads a list,
				// and none of them work on a plain object.
				JSValue array = JS_NewArray(Context);
				for (size_t index = 0; index < values.size(); index++) {
					JS_SetPropertyUint32(
						Context,
						array,
						static_cast<uint32_t>(index),
						JS_NewStringLen(Context, values[index].data(), values[index].size())
					);
				}
				Set(array);
			}

			void ReturnInstances(std::span<const Entity> values) override {
				JSValue array = JS_NewArray(Context);
				for (size_t index = 0; index < values.size(); index++) {
					JS_SetPropertyUint32(
						Context, array, static_cast<uint32_t>(index), MakeJsInstance(Context, values[index])
					);
				}
				Set(array);
			}

			void ReturnValue(const ScriptValue &value) override {
				Set(FromScriptValue(Context, value));
			}

			void ReturnCFrame(const core::CFrame &value) override {
				Set(MakeCFrame(Context, value));
			}

			void ReturnVector2(const core::Vector2 &value) override {
				Set(MakeVector2(Context, value));
			}

			void ReturnEnum(Name enumName, Name member) override {
				Set(MakeJsEnumItem(Context, enumName, member));
			}

			void ReturnEnums(Name enumName, std::span<const Name> members) override {
				// **A real `Array`**, for `ReturnStrings`' reason: `map`,
				// `filter` and `length` are how a JavaScript author reads a list.
				JSValue array = JS_NewArray(Context);
				for (size_t index = 0; index < members.size(); index++) {
					JS_SetPropertyUint32(
						Context,
						array,
						static_cast<uint32_t>(index),
						MakeJsEnumItem(Context, enumName, members[index])
					);
				}
				Set(array);
			}

			void ReturnInputObjects(std::span<const InputReport> reports) override {
				JSValue array = JS_NewArray(Context);
				for (size_t index = 0; index < reports.size(); index++) {
					JS_SetPropertyUint32(
						Context,
						array,
						static_cast<uint32_t>(index),
						MakeJsInputObject(Context, reports[index])
					);
				}
				Set(array);
			}

			void ReturnBoundAction(const BoundActionReport &report) override {
				Set(MakeBoundAction(report));
			}

			void ReturnBoundActions(std::span<const BoundActionReport> reports) override {
				// **Keyed by name rather than an array**, which is Roblox's shape
				// and the one a caller wants: the question this answers is "what
				// has claimed E", and the name is how anything is unbound
				// afterwards.
				JSValue object = JS_NewObject(Context);
				for (const BoundActionReport &report : reports) {
					JS_SetPropertyStr(
						Context, object, std::string(report.Name).c_str(), MakeBoundAction(report)
					);
				}
				Set(object);
			}

			void ReturnInstance(ecs::Entity value) override {
				// Nil for a null, which this language spells `null` - see the
				// interface. `MakeJsInstance` makes the same call.
				Set(MakeJsInstance(Context, value));
			}

			void ReturnAttribute(const AttributeValue &value) override {
				Set(MakeJsAttribute(Context, value));
			}

			void ReturnAttributes(std::span<const std::pair<Name, AttributeValue>> values) override {
				JSValue object = JS_NewObject(Context);
				for (const auto &entry : values) {
					JSValue held = MakeJsAttribute(Context, entry.second);
					if (JS_IsNull(held)) {
						// A value with no form here is left out rather than
						// stored as null, so `Object.keys` and Luau's `pairs`
						// answer the same set.
						JS_FreeValue(Context, held);
						continue;
					}
					JS_SetPropertyStr(Context, object, entry.first.Text().data(), held);
				}
				Set(object);
			}

			void ReturnSignal(SignalKind kind, Name property) override {
				Set(MakeJsSignal(Context, kind, Self, property));
			}

			void ReturnTween(ecs::Entity tween) override {
				Set(MakeJsTween(Context, tween));
			}

			void ForgetSubject(ecs::Entity subject) override {
				std::vector<CallbackRef> released;
				JsOf(Context).Signals.DropSubject(subject, released);
				for (const CallbackRef reference : released) {
					Release(Context, reference);
				}
			}

			void Forget(ecs::Entity instance) override {
				JsContext &bound = JsOf(Context);
				JSContext *context = Context;
				ForgetSubtree(
					*bound.World, bound.Signals, bound.Changes, instance, [context](CallbackRef reference) {
						Release(context, reference);
					}
				);
			}

			void Await(uint64_t ticket) override {
				// **A promise rather than a suspended thread**, which is the
				// language's own idiom and the whole reason `Await` exists as one
				// member: `PumpJsDeliveries` resolves it with the same three
				// values the Luau barrier resumes a coroutine with.
				Suspend(JsOf(Context).AwaitedTickets, ticket);
			}

			void AwaitChild(uint64_t waiter) override {
				// **The same promise under a different key**, resolved by
				// `PumpJsChildWaiters` with one instance or `null` - which is
				// what the Luau half resumes its coroutine with, so a script
				// reads the same answer in either language and only the syntax
				// for waiting on it differs.
				Suspend(JsOf(Context).AwaitedChildren, waiter);
			}

			[[noreturn]] void Raise(const char *message) override {
				// The value is discarded because the exception is on the context
				// now; the trampoline turns the throw below back into
				// `JS_EXCEPTION`.
				JS_FreeValue(Context, JS_ThrowTypeError(Context, "%s", message));
				throw JsRaised{};
			}

		  private:
			// Hands back a promise and keeps its resolver under `key`.
			//
			// **`insert_or_assign` rather than `emplace`**, for the Luau half's
			// reason: a chained call reuses the slot and the stale resolver would
			// leak.
			//
			// @param waiting Which table the resume will look in.
			// @param key     What that table keys this promise on.
			void Suspend(std::unordered_map<uint64_t, CallbackRef> &waiting, uint64_t key) {
				JSValue settle[2];
				JSValue promise = JS_NewPromiseCapability(Context, settle);
				if (JS_IsException(promise)) {
					// The exception is already on the context. Raising here would
					// replace it with a less specific one.
					throw JsRaised{};
				}

				waiting.insert_or_assign(key, Retain(Context, settle[0]));
				JS_FreeValue(Context, settle[0]);
				JS_FreeValue(Context, settle[1]);
				Set(promise);
			}

			// One action's record as a plain object.
			//
			// Roblox's four fields, and the two it does not have are absent from
			// the *binding* rather than from the record - see the Luau twin.
			JSValue MakeBoundAction(const BoundActionReport &report) {
				JSValue record = JS_NewObject(Context);

				JSValue keys = JS_NewArray(Context);
				for (size_t index = 0; index < report.Keys.size(); index++) {
					JS_SetPropertyUint32(
						Context,
						keys,
						static_cast<uint32_t>(index),
						MakeJsEnumItem(Context, Name("KeyCode"), report.Keys[index])
					);
				}

				JS_SetPropertyStr(Context, record, "inputTypes", keys);
				JS_SetPropertyStr(Context, record, "priorityLevel", JS_NewInt32(Context, report.Priority));
				JS_SetPropertyStr(Context, record, "stackOrder", JS_NewInt32(Context, report.StackOrder));
				JS_SetPropertyStr(
					Context, record, "createTouchButton", JS_NewBool(Context, report.CreateTouchButton)
				);
				return record;
			}

			void Set(JSValue value) {
				// **Appended rather than replacing**, so a method with more than
				// one answer keeps them all - see `Take`. Every method but
				// `SoundService::GetListener` calls this once, and one value is
				// handed back unpacked.
				Results.push_back(value);
			}

			JSContext *Context;
			JSValueConst *Argv;
			size_t Argc;
			Entity Self;
			std::vector<JSValue> Results;
		};

		// The one C function every neutral method is installed as.
		//
		// The row is the magic number, which is QuickJS's own way of sharing one
		// implementation across a list of names.
		JSValue
		NeutralJsMethod(JSContext *context, JSValueConst self, int argc, JSValueConst *argv, int magic) {
			JsCall call(context, self, argc, argv);

			try {
				// **The receiver is checked here rather than in every method**,
				// which is where the Luau adapter checks it too - a neutral method
				// body may assume `Subject()` names a real row.
				if (call.Subject() == ecs::NULL_ENTITY) {
					call.Raise("not an instance");
				}
				NeutralInstanceMethods()[static_cast<size_t>(magic)].Function(call);
			} catch (const JsRaised &) {
				return JS_EXCEPTION;
			} catch (...) {
				// **Nothing may unwind past here.** QuickJS is C, so an exception
				// escaping this frame would unwind through frames with no landing
				// pad. A fault that is not a refusal becomes an ordinary script
				// error rather than a process that ends without a line number.
				return JS_ThrowInternalError(context, "an instance method failed");
			}

			return call.Take();
		}

		// The one C function every neutral *service* method is installed as.
		//
		// **The magic indexes `JsContext::ServiceMethods` rather than one shared
		// table**, which is where this differs from the instance trampoline
		// above: there is one instance method table and each service has a table
		// of its own, so the rows are flattened into the context as they are
		// installed. The Luau twin puts the row's address on an upvalue instead -
		// `lua_pushcclosure` takes any number of values and
		// `JS_NewCFunctionMagic` takes one integer, which is the whole of the
		// difference.
		JSValue
		NeutralJsServiceMethod(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic) {
			JsCall call(context, JsCall::OnService{}, argc, argv);

			try {
				JsOf(context).ServiceMethods[static_cast<size_t>(magic)](call);
			} catch (const JsRaised &) {
				return JS_EXCEPTION;
			} catch (...) {
				// **Nothing may unwind past here**, for the reason the instance
				// trampoline gives: QuickJS is C and the frame below this one has
				// no landing pad.
				return JS_ThrowInternalError(context, "a service method failed");
			}

			return call.Take();
		}

		// The two C functions every neutral service *property* is installed as.
		//
		// **An accessor per name, which is the whole of why a property list had
		// to exist.** The Luau half can string-compare a field inside one
		// `__index`; `JS_DefinePropertyGetSet` registers a getter and a setter
		// against one atom, so the names are needed at install time and a
		// catch-all cannot supply them. There is no `Proxy` to fall back on -
		// `JsBindings.cpp` excludes it deliberately, because a script could wrap
		// an instance and intercept the property surface.
		//
		// The magic indexes `JsContext::ServiceProperties`, exactly as the
		// service method trampoline indexes `ServiceMethods` and for its reason:
		// `JS_NewCFunctionMagic` takes one integer where `lua_pushcclosure` takes
		// an address.
		JSValue NeutralJsServiceGet(JSContext *context, JSValueConst, int, JSValueConst *, int magic) {
			const JsServiceProperty &row = JsOf(context).ServiceProperties[static_cast<size_t>(magic)];

			JsCall call(context, JsCall::OnService{}, 0, nullptr);
			try {
				row.Row->Get(call);
			} catch (const JsRaised &) {
				return JS_EXCEPTION;
			} catch (...) {
				// **Nothing may unwind past here**, for the reason the two
				// trampolines above give: QuickJS is C and the frame below this
				// one has no landing pad.
				return JS_ThrowInternalError(context, "a service property failed");
			}

			return call.Take();
		}

		JSValue
		NeutralJsServiceSet(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic) {
			const JsServiceProperty &row = JsOf(context).ServiceProperties[static_cast<size_t>(magic)];

			JsCall call(context, JsCall::OnService{}, argc, argv);
			try {
				// **A read-only property refuses by name rather than silently
				// dropping the write**, which is what leaving the setter
				// undefined would do in sloppy mode. The Luau half says the same
				// sentence from `LuauServiceNewIndex`.
				if (row.Row->Set == nullptr) {
					const std::string message =
						std::string(row.Service) + "." + row.Row->Name + " is read-only";
					call.Raise(message.c_str());
				}
				row.Row->Set(call);
			} catch (const JsRaised &) {
				return JS_EXCEPTION;
			} catch (...) {
				return JS_ThrowInternalError(context, "a service property failed");
			}

			return JS_UNDEFINED;
		}
	}

	void InstallJsNeutralMethods(JSContext *context, JSValueConst methods) {
		int row = 0;
		for (const InstanceMethod &method : NeutralInstanceMethods()) {
			JS_SetPropertyStr(
				context,
				methods,
				method.Name,
				JS_NewCFunctionMagic(context, NeutralJsMethod, method.Name, 1, JS_CFUNC_generic_magic, row++)
			);
		}
	}

	void InstallJsServiceMethods(
		JSContext *context, JSValueConst service, std::span<const ServiceMethod> methods
	) {
		std::vector<ScriptMethod> &rows = JsOf(context).ServiceMethods;

		for (const ServiceMethod &method : methods) {
			const auto row = static_cast<int>(rows.size());
			rows.push_back(method.Function);

			JS_SetPropertyStr(
				context,
				service,
				method.Name,
				JS_NewCFunctionMagic(
					context, NeutralJsServiceMethod, method.Name, 1, JS_CFUNC_generic_magic, row
				)
			);
		}
	}

	void InstallJsServiceProperties(
		JSContext *context,
		JSValueConst service,
		const char *name,
		std::span<const ServiceProperty> properties
	) {
		std::vector<JsServiceProperty> &rows = JsOf(context).ServiceProperties;

		for (const ServiceProperty &property : properties) {
			const auto row = static_cast<int>(rows.size());

			// The row's address rather than a copy: a `ServiceProperty` lives in
			// a `static constexpr` array for the life of the program, exactly as
			// a `ServiceMethod` does. The service's name rides along because a
			// refusal has to say which service it is refusing for.
			rows.push_back({&property, name});

			JSValue getter = JS_NewCFunctionMagic(
				context, NeutralJsServiceGet, property.Name, 0, JS_CFUNC_generic_magic, row
			);
			JSValue setter = JS_NewCFunctionMagic(
				context, NeutralJsServiceSet, property.Name, 1, JS_CFUNC_generic_magic, row
			);

			// **The object stays a plain object, unlike the Luau half's
			// userdata.** An accessor runs on every read here, so there is no
			// `GETIMPORT` to defeat and nothing to tag - which is the asymmetry
			// `ServiceSurface::Properties` describes.
			const JSAtom atom = JS_NewAtom(context, property.Name);
			JS_DefinePropertyGetSet(context, service, atom, getter, setter, JS_PROP_C_W_E);
			JS_FreeAtom(context, atom);
		}
	}
}
