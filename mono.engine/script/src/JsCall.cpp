// `ScriptCall` in JavaScript's currency.
//
// The twin of `LuauCall.cpp`, and deliberately the same shape: one adapter, one
// trampoline, and a method table walked at install time so a neutral method
// costs a row in `ScriptMethods.cpp` and nothing here.
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
#include "ScriptCall.hpp"

#include <engine/ecs/Attributes.hpp>

#include <span>
#include <string>
#include <utility>

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
		// this binding stored and can for one a future build wrote — so it is a
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
		// arithmetic — and the reason the neutral methods count from zero.
		class JsCall final : public ScriptCall {
		  public:
			JsCall(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
				: Context(context), Argv(argv), Argc(argc < 0 ? 0u : static_cast<size_t>(argc)),
				  Self(JsEntityOf(context, self)) {}

			~JsCall() override {
				// A result the trampoline never took, because something raised
				// after it was set. Freeing `JS_UNDEFINED` is a no-op, so the
				// ordinary path costs nothing.
				JS_FreeValue(Context, Result);
			}

			// Hands the result over. `undefined` for a method that answered
			// nothing, which is what a JavaScript function without a `return`
			// evaluates to.
			JSValue Take() {
				JSValue value = Result;
				Result = JS_UNDEFINED;
				return value;
			}

			ecs::Store &World() override {
				return *JsOf(Context).World;
			}

			ChangeQueue &Changes() override {
				return JsOf(Context).Changes;
			}

			Entity Subject() const override {
				return Self;
			}

			bool IsNil(size_t index) const override {
				return index >= Argc || JS_IsNull(Argv[index]) || JS_IsUndefined(Argv[index]);
			}

			std::string AsString(size_t index) override {
				// **A string or a number and nothing else**, which is what
				// `luaL_checklstring` accepts on the other side. Letting
				// `JS_ToCString` decide would turn `AddTag({})` into the tag
				// `"[object Object]"` where Luau raises — a wrong argument
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

			core::CFrame AsCFrame(size_t index) override {
				// Qualified, because this member shares the free function's name.
				const core::CFrame *frame = index < Argc ? script::AsCFrame(Context, Argv[index]) : nullptr;
				if (frame == nullptr) {
					Raise("expected a CFrame");
				}
				return *frame;
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
				// added.** A class check is exact — `Vector2` and `UDim` are both
				// two floats — so the order is legibility and not correctness.
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

			void ReturnCFrame(const core::CFrame &value) override {
				Set(MakeCFrame(Context, value));
			}

			void ReturnInstance(ecs::Entity value) override {
				// Nil for a null, which this language spells `null` — see the
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

			[[noreturn]] void Raise(const char *message) override {
				// The value is discarded because the exception is on the context
				// now; the trampoline turns the throw below back into
				// `JS_EXCEPTION`.
				JS_FreeValue(Context, JS_ThrowTypeError(Context, "%s", message));
				throw JsRaised{};
			}

		  private:
			void Set(JSValue value) {
				JS_FreeValue(Context, Result);
				Result = value;
			}

			JSContext *Context;
			JSValueConst *Argv;
			size_t Argc;
			Entity Self;
			JSValue Result = JS_UNDEFINED;
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
				// which is where the Luau adapter checks it too — a neutral method
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
}
