#include "JsBindings.hpp"

#include "JsContext.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// The entity an instance object stands for.
	//
	// Free rather than file-local because `JsSurface.cpp` binds `:Destroy` and
	// friends, and every one of them starts here.
	ecs::Entity JsEntityOf(JSContext *context, JSValueConst object) {
		void *opaque = JS_GetOpaque2(context, object, JsOf(context).InstanceClass);
		if (opaque == nullptr) {
			// The class check throws when the object is of another class.
			// Cleared: a caller asking "is this an instance" is entitled to a
			// no rather than a pending exception.
			JS_FreeValue(context, JS_GetException(context));
			return ecs::NULL_ENTITY;
		}
		return *static_cast<ecs::Entity *>(opaque);
	}

	namespace {
		using core::Name;
		using ecs::Entity;
		JSValue PrototypeFor(JSContext *context, ecs::ClassId id, Entity sample);
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		// --- value types -----------------------------------------------------

		template <class T> JSValue MakeValue(JSContext *context, JSClassID id, const T &value) {
			JSValue object = JS_NewObjectClass(context, static_cast<int>(id));
			if (JS_IsException(object)) {
				return object;
			}
			JS_SetOpaque(object, new T(value));
			return object;
		}

		template <class T> T *ValueOf(JSContext *context, JSValueConst object, JSClassID id) {
			return static_cast<T *>(JS_GetOpaque2(context, object, id));
		}

	}

	// --- the pieces the runtime needs ---------------------------------------

	JSValue MakeVector3(JSContext *context, const core::Vector3 &value) {
		return MakeValue(context, JsOf(context).Vector3Class, value);
	}

	JSValue MakeColor3(JSContext *context, const core::Color3 &value) {
		return MakeValue(context, JsOf(context).Color3Class, value);
	}

	JSValue MakeCFrame(JSContext *context, const core::CFrame &value) {
		return MakeValue(context, JsOf(context).CFrameClass, value);
	}

	core::Vector3 *AsVector3(JSContext *context, JSValueConst value) {
		return ValueOf<core::Vector3>(context, value, JsOf(context).Vector3Class);
	}

	core::Color3 *AsColor3(JSContext *context, JSValueConst value) {
		return ValueOf<core::Color3>(context, value, JsOf(context).Color3Class);
	}

	core::CFrame *AsCFrame(JSContext *context, JSValueConst value) {
		return ValueOf<core::CFrame>(context, value, JsOf(context).CFrameClass);
	}

	// The four `gui` is authored in. `JsDatatypes.cpp` already installs all
	// four constructors and their class ids; what was missing was the pair that
	// carries a property's bytes across, which is why a `UDim2` could be built
	// in a script and not assigned to anything.
	//
	// The class id is the check, exactly as the Luau tag is: `Vector2` and
	// `UDim` are both two floats, and `JS_GetOpaque2` against the wrong id
	// returns null rather than reinterpreting the pair.

	JSValue MakeVector2(JSContext *context, const core::Vector2 &value) {
		return MakeValue(context, JsOf(context).Vector2Class, value);
	}

	JSValue MakeUDim(JSContext *context, const core::UDim &value) {
		return MakeValue(context, JsOf(context).UDimClass, value);
	}

	JSValue MakeUDim2(JSContext *context, const core::UDim2 &value) {
		return MakeValue(context, JsOf(context).UDim2Class, value);
	}

	JSValue MakeRect(JSContext *context, const core::Rect &value) {
		return MakeValue(context, JsOf(context).RectClass, value);
	}

	core::Vector2 *AsVector2(JSContext *context, JSValueConst value) {
		return ValueOf<core::Vector2>(context, value, JsOf(context).Vector2Class);
	}

	core::UDim *AsUDim(JSContext *context, JSValueConst value) {
		return ValueOf<core::UDim>(context, value, JsOf(context).UDimClass);
	}

	core::UDim2 *AsUDim2(JSContext *context, JSValueConst value) {
		return ValueOf<core::UDim2>(context, value, JsOf(context).UDim2Class);
	}

	core::Rect *AsRect(JSContext *context, JSValueConst value) {
		return ValueOf<core::Rect>(context, value, JsOf(context).RectClass);
	}

	// The three `effects` is authored in, and the same gap again: `JsDatatypes.cpp`
	// has installed the constructors and the class ids since v0.6, and what was
	// missing was the pair that carries a property's bytes — which is why a
	// `NumberSequence` could be built in a script and assigned to nothing.
	//
	// **`MakeValue` allocates and copies, and here that is a 408-byte copy** where
	// every other type on this page is sixteen. Acceptable for the same reason the
	// Luau side's is: this is the path a caller reached through a *name*, once per
	// property access, and never the path a draw list walks.

	JSValue MakeNumberRange(JSContext *context, const core::NumberRange &value) {
		return MakeValue(context, JsOf(context).NumberRangeClass, value);
	}

	JSValue MakeNumberSequence(JSContext *context, const core::NumberSequence &value) {
		return MakeValue(context, JsOf(context).NumberSequenceClass, value);
	}

	JSValue MakeColorSequence(JSContext *context, const core::ColorSequence &value) {
		return MakeValue(context, JsOf(context).ColorSequenceClass, value);
	}

	core::NumberRange *AsNumberRange(JSContext *context, JSValueConst value) {
		return ValueOf<core::NumberRange>(context, value, JsOf(context).NumberRangeClass);
	}

	core::NumberSequence *AsNumberSequence(JSContext *context, JSValueConst value) {
		return ValueOf<core::NumberSequence>(context, value, JsOf(context).NumberSequenceClass);
	}

	core::ColorSequence *AsColorSequence(JSContext *context, JSValueConst value) {
		return ValueOf<core::ColorSequence>(context, value, JsOf(context).ColorSequenceClass);
	}

	namespace {
		JSValue MakeEnumItem(JSContext *context, const Name &enumName, const Name &member);
		bool ReadEnumValueImpl(JSContext *context, JSValueConst value, const Name &enumName, Name &out);

		// --- marshalling -----------------------------------------------------
		//
		// **A switch over `PropertyType` and nothing else**, exactly as the Luau
		// side is. No property is named in either file, which is what makes one
		// property declaration reach both languages.

		// How wide a property value can be. See `Instances.cpp`, which carries
		// the whole argument — this is the same constant on the other language's
		// side, and the two being spelled from the same two `sizeof`s is what
		// stops a property that reads in Luau failing in JavaScript.
		constexpr size_t WIDEST_PROPERTY =
			std::max(sizeof(core::ColorSequence), sizeof(core::NumberSequence));

	}

	JSValue ToJsValue(JSContext *context, PropertyType type, core::Name enumName, const void *bytes) {
		switch (type) {
		case PropertyType::Bool:
			return JS_NewBool(context, *static_cast<const bool *>(bytes));
		case PropertyType::Float:
			return JS_NewFloat64(context, *static_cast<const float *>(bytes));
		case PropertyType::Double:
			return JS_NewFloat64(context, *static_cast<const double *>(bytes));
		case PropertyType::Int32:
			return JS_NewInt32(context, *static_cast<const int32_t *>(bytes));
		case PropertyType::Int64:
			return JS_NewInt64(context, *static_cast<const int64_t *>(bytes));
		case PropertyType::Name:
			// Text, never the interned id — the number means a different
			// string in the next process.
			return JS_NewString(context, static_cast<const Name *>(bytes)->Text().data());
		case PropertyType::String:
			// **Never reached, and refused rather than handled**, exactly as
			// the Luau side refuses it: the caller takes a `std::string`
			// down its own path before it gets here, and these `bytes` are
			// uninitialised storage. Throwing makes a future caller that
			// forgot the branch fail loudly instead of corrupting a heap.
			return JS_ThrowTypeError(context, "a string property is read through its own path");
		case PropertyType::Enum:
			// An `EnumItem`, not a string — the same value the Luau side
			// hands back, so a property declared once behaves the same in
			// both languages. The storage is an interned `Name` either way;
			// what the type buys is that a wrong member is refused.
			return MakeEnumItem(context, enumName, *static_cast<const Name *>(bytes));
		case PropertyType::Vector3:
			return MakeVector3(context, *static_cast<const core::Vector3 *>(bytes));
		case PropertyType::Color3:
			return MakeColor3(context, *static_cast<const core::Color3 *>(bytes));
		case PropertyType::CFrame:
			return MakeCFrame(context, *static_cast<const core::CFrame *>(bytes));
		case PropertyType::Vector2:
			return MakeVector2(context, *static_cast<const core::Vector2 *>(bytes));
		case PropertyType::UDim:
			return MakeUDim(context, *static_cast<const core::UDim *>(bytes));
		case PropertyType::UDim2:
			return MakeUDim2(context, *static_cast<const core::UDim2 *>(bytes));
		case PropertyType::Rect:
			return MakeRect(context, *static_cast<const core::Rect *>(bytes));
		case PropertyType::NumberRange:
			return MakeNumberRange(context, *static_cast<const core::NumberRange *>(bytes));
		case PropertyType::NumberSequence:
			return MakeNumberSequence(context, *static_cast<const core::NumberSequence *>(bytes));
		case PropertyType::ColorSequence:
			return MakeColorSequence(context, *static_cast<const core::ColorSequence *>(bytes));
		case PropertyType::Reference: {
			// **Null, and it means nil rather than "a root".** This handed
			// back `workspace` before v0.7, because `workspace` *was* the
			// world and a root therefore belonged to it. `workspace` is now
			// the `Workspace` instance, so having no parent is an ordinary
			// state a script can produce and read back — and an instance in
			// that state is drawn by nothing and listed by nothing. The Luau
			// side says the same thing in `PushValue`; one property
			// declaration, two languages, one answer.
			const Entity referenced = *static_cast<const Entity *>(bytes);
			if (referenced == ecs::NULL_ENTITY) {
				return JS_NULL;
			}

			// **The stored object for the Workspace, not a fresh one.**
			// `MakeJsInstance` mints a new JS object per call, and
			// JavaScript's `===` is object identity with no `__eq` to
			// override — so `part.Parent === workspace` would be false for
			// the one comparison every script makes. Luau has no such
			// problem: its `Instance` metatable carries `__eq` and compares
			// entities.
			//
			// Narrow on purpose. This does not give instances identity in
			// general — `child.Parent === model` is still false — and
			// pretending otherwise would need every live instance interned
			// in the context. What it does is keep the one object a script
			// is handed as a global comparable with itself.
			if (JsContext &bound = JsOf(context); referenced == JsEntityOf(context, bound.Workspace)) {
				return JS_DupValue(context, bound.Workspace);
			}
			return MakeJsInstance(context, referenced);
		}
		case PropertyType::Opaque:
			break;
		}
		// **The type rather than the property's name**, because the second
		// caller has no property: an ECS component field carries exactly these
		// values and is not one.
		return JS_ThrowTypeError(context, "a %s has no script representation", ecs::Describe(type));
	}

	bool
	FromJsValue(JSContext *context, JSValueConst value, PropertyType type, core::Name enumName, void *out) {
		switch (type) {
		case PropertyType::Bool:
			*static_cast<bool *>(out) = JS_ToBool(context, value) == 1;
			return true;
		case PropertyType::Float: {
			double number = 0.0;
			if (JS_ToFloat64(context, &number, value) != 0) {
				return false;
			}
			*static_cast<float *>(out) = static_cast<float>(number);
			return true;
		}
		case PropertyType::Double:
			return JS_ToFloat64(context, static_cast<double *>(out), value) == 0;
		case PropertyType::Int32:
			return JS_ToInt32(context, static_cast<int32_t *>(out), value) == 0;
		case PropertyType::Int64:
			return JS_ToInt64(context, static_cast<int64_t *>(out), value) == 0;
		case PropertyType::String:
			// Refused here for `ToJs`'s reason; the caller's own branch is
			// what actually serves this type.
			return false;
		case PropertyType::Name: {
			const char *text = JS_ToCString(context, value);
			if (text == nullptr) {
				return false;
			}
			*static_cast<Name *>(out) = Name(text);
			JS_FreeCString(context, text);
			return true;
		}
		case PropertyType::Enum:
			// A string is accepted as well as an `EnumItem`, because
			// `part.AlphaMode = "Clip"` is what a migrating script
			// already contains. A member of the *wrong* enum is refused,
			// which is the error a bare string could never have caught.
			return ReadEnumValueImpl(context, value, enumName, *static_cast<Name *>(out));
		case PropertyType::Vector3: {
			const core::Vector3 *vector = AsVector3(context, value);
			if (vector == nullptr) {
				return false;
			}
			*static_cast<core::Vector3 *>(out) = *vector;
			return true;
		}
		case PropertyType::Color3: {
			const core::Color3 *colour = AsColor3(context, value);
			if (colour == nullptr) {
				return false;
			}
			*static_cast<core::Color3 *>(out) = *colour;
			return true;
		}
		case PropertyType::CFrame: {
			const core::CFrame *frame = AsCFrame(context, value);
			if (frame == nullptr) {
				return false;
			}
			*static_cast<core::CFrame *>(out) = *frame;
			return true;
		}
		case PropertyType::Vector2: {
			const core::Vector2 *vector = AsVector2(context, value);
			if (vector == nullptr) {
				return false;
			}
			*static_cast<core::Vector2 *>(out) = *vector;
			return true;
		}
		case PropertyType::UDim: {
			const core::UDim *length = AsUDim(context, value);
			if (length == nullptr) {
				return false;
			}
			*static_cast<core::UDim *>(out) = *length;
			return true;
		}
		case PropertyType::UDim2: {
			const core::UDim2 *size = AsUDim2(context, value);
			if (size == nullptr) {
				return false;
			}
			*static_cast<core::UDim2 *>(out) = *size;
			return true;
		}
		case PropertyType::Rect: {
			const core::Rect *rect = AsRect(context, value);
			if (rect == nullptr) {
				return false;
			}
			*static_cast<core::Rect *>(out) = *rect;
			return true;
		}
		case PropertyType::NumberRange: {
			const core::NumberRange *range = AsNumberRange(context, value);
			if (range == nullptr) {
				return false;
			}
			*static_cast<core::NumberRange *>(out) = *range;
			return true;
		}
		case PropertyType::NumberSequence: {
			const core::NumberSequence *curve = AsNumberSequence(context, value);
			if (curve == nullptr) {
				return false;
			}
			*static_cast<core::NumberSequence *>(out) = *curve;
			return true;
		}
		case PropertyType::ColorSequence: {
			const core::ColorSequence *gradient = AsColorSequence(context, value);
			if (gradient == nullptr) {
				return false;
			}
			*static_cast<core::ColorSequence *>(out) = *gradient;
			return true;
		}
		case PropertyType::Reference: {
			// An instance arrives as its object; `null` detaches, which is
			// what Roblox's `Parent = nil` means. `workspace` needs no case
			// of its own any more — it is an instance object like any other,
			// which is the whole of what collapsing the two notions of "the
			// workspace" bought.
			if (JS_IsNull(value) || JS_IsUndefined(value)) {
				*static_cast<Entity *>(out) = ecs::NULL_ENTITY;
				return true;
			}

			void *opaque = JS_GetOpaque2(context, value, JsOf(context).InstanceClass);
			if (opaque == nullptr) {
				return false;
			}
			*static_cast<Entity *>(out) = *static_cast<Entity *>(opaque);
			return true;
		}
		case PropertyType::Opaque:
			break;
		}
		return false;
	}

	namespace {

		// --- instances -------------------------------------------------------

		// Text, not an interned id, for the reason `Instances.cpp`'s twin gives
		// at length: building the id takes a lock on the process-wide registry,
		// and this is on the path of every property a script reads or writes.
		// The two surfaces share rules rather than code, and this is one of the
		// rules.
		// **A non-scriptable property is not found here either.** The two
		// surfaces share rules rather than code — `Instances.cpp`'s twin
		// carries why the answer is "no such member" rather than a refusal.
		const PropertyDescriptor *Find(const Store &store, Entity instance, const char *name) {
			const std::string_view key(name);
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Name.Text() == key) {
					return property.Scriptable ? &property : nullptr;
				}
			}
			return nullptr;
		}

		// The accessor pair every property on a prototype is made of. The
		// property's name travels as closure data, so one function serves all of
		// them and none of them is written by hand.
		JSValue
		PropertyGet(JSContext *context, JSValueConst self, int, JSValueConst *, int, JSValueConst *data) {
			JsContext &bound = JsOf(context);
			const Entity instance = JsEntityOf(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			const char *name = JS_ToCString(context, data[0]);
			const PropertyDescriptor *property = Find(*bound.World, instance, name);
			JS_FreeCString(context, name);

			if (property == nullptr) {
				return JS_ThrowTypeError(context, "no such property");
			}

			// **The one type that cannot ride the shared byte buffer**, and the
			// Luau side takes the same exception for the same reason: a
			// `PropertyType::String` getter *assigns* into its destination, and
			// assigning a `std::string` into uninitialised bytes is undefined
			// behaviour rather than a fast path. So it gets a real object.
			if (property->Type == PropertyType::String) {
				std::string text;
				if (!bound.World->GetProperty(instance, *property, &text, sizeof(text))) {
					return JS_ThrowTypeError(context, "could not read '%s'", property->Name.Text().data());
				}
				return JS_NewStringLen(context, text.data(), text.size());
			}

			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property->Size > sizeof(bytes) ||
				!bound.World->GetProperty(instance, *property, bytes, property->Size)) {
				return JS_ThrowTypeError(context, "could not read '%s'", property->Name.Text().data());
			}
			return ToJsValue(context, property->Type, property->EnumName, bytes);
		}

		JSValue PropertySet(
			JSContext *context, JSValueConst self, int argc, JSValueConst *argv, int, JSValueConst *data
		) {
			JsContext &bound = JsOf(context);
			const Entity instance = JsEntityOf(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			const char *name = JS_ToCString(context, data[0]);
			const PropertyDescriptor *property = Find(*bound.World, instance, name);
			JS_FreeCString(context, name);

			if (property == nullptr) {
				return JS_ThrowTypeError(context, "no such property");
			}
			if (!property->Writable) {
				return JS_ThrowTypeError(context, "'%s' is read-only", property->Name.Text().data());
			}

			// The write half of the same exception — see the getter above.
			if (property->Type == PropertyType::String) {
				size_t length = 0;
				const char *text = JS_ToCStringLen(context, &length, argv[0]);
				if (text == nullptr) {
					return JS_ThrowTypeError(
						context, "'%s' cannot take that value", property->Name.Text().data()
					);
				}

				const std::string value(text, length);
				JS_FreeCString(context, text);

				if (!bound.World->SetProperty(instance, *property, &value, sizeof(value))) {
					return JS_ThrowTypeError(context, "could not set '%s'", property->Name.Text().data());
				}
				return JS_UNDEFINED;
			}

			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property->Size > sizeof(bytes) ||
				!FromJsValue(context, argv[0], property->Type, property->EnumName, bytes)) {
				return JS_ThrowTypeError(
					context, "'%s' cannot take that value", property->Name.Text().data()
				);
			}

			// Refused loudly. A replica rejecting the write is the case that
			// matters: a script author cannot tell "rejected" from "applied and
			// overwritten by the next delta" without being told.
			if (!bound.World->SetProperty(instance, *property, bytes, property->Size)) {
				return JS_ThrowTypeError(context, "could not set '%s'", property->Name.Text().data());
			}
			return JS_UNDEFINED;
		}

		// The prototype for one ECS class, built once and cached.
		JSValue PrototypeFor(JSContext *context, ecs::ClassId id, Entity sample) {
			JsContext &bound = JsOf(context);

			const auto cached = bound.Prototypes.find(id.Index);
			if (cached != bound.Prototypes.end()) {
				return JS_DupValue(context, cached->second);
			}

			// **Behind the shared method prototype**, so `part.Destroy()`
			// resolves up the chain rather than being copied onto every class.
			// Installed by `OpenJsSurface`, which runs after this file's own
			// `Open` — so a prototype built before it falls back to a plain
			// object rather than failing.
			JSValue global = JS_GetGlobalObject(context);
			JSValue methods = JS_GetPropertyStr(context, global, "__instanceMethods");
			JS_FreeValue(context, global);

			JSValue proto =
				JS_IsObject(methods) ? JS_NewObjectProto(context, methods) : JS_NewObject(context);
			JS_FreeValue(context, methods);

			for (const PropertyDescriptor &property : bound.World->PropertiesOf(sample)) {
				JSValue name = JS_NewString(context, property.Name.Text().data());

				JSValue getter = JS_NewCFunctionData(context, PropertyGet, 0, 0, 1, &name);
				JSValue setter = JS_NewCFunctionData(context, PropertySet, 1, 0, 1, &name);

				const JSAtom atom = JS_NewAtom(context, property.Name.Text().data());
				JS_DefinePropertyGetSet(context, proto, atom, getter, setter, JS_PROP_C_W_E);
				JS_FreeAtom(context, atom);
				JS_FreeValue(context, name);
			}

			bound.Prototypes.emplace(id.Index, JS_DupValue(context, proto));
			bound.Owned.push_back(JS_DupValue(context, proto));
			return proto;
		}

		// --- enums -----------------------------------------------------------
		//
		// **`Enum` is a getter on the global rather than a table built at
		// open.** A game registers its own materials at load time, so a table
		// snapshotted when the VM opened would have missed every one of them —
		// and the Luau side solves this with an `__index` metamethod, which
		// JavaScript has no equivalent of without `Proxy`. `Proxy` is
		// deliberately excluded (a script could wrap an instance and intercept
		// the property surface), so a getter that rebuilds from the registry is
		// what remains. It costs a walk of the enum table per access to `Enum`,
		// which is a setup-time operation.

		// One member of one enum: two interned names and nothing else.
		//
		// No `Value`, exactly as the Luau side has none: **the number is not the
		// format**, and an author who reads one will eventually write it into a
		// save file.
		struct EnumItemPayload {
			Name Enum;
			Name Member;
		};

		JSValue EnumItemGet(JSContext *context, JSValueConst self, int magic) {
			const auto *item =
				static_cast<EnumItemPayload *>(JS_GetOpaque2(context, self, JsOf(context).EnumItemClass));
			if (item == nullptr) {
				return JS_ThrowTypeError(context, "not an EnumItem");
			}
			return JS_NewString(context, (magic == 0 ? item->Member : item->Enum).Text().data());
		}

		JSValue EnumItemToString(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const auto *item =
				static_cast<EnumItemPayload *>(JS_GetOpaque2(context, self, JsOf(context).EnumItemClass));
			if (item == nullptr) {
				return JS_ThrowTypeError(context, "not an EnumItem");
			}

			const std::string text =
				std::string("Enum.") + item->Enum.Text().data() + "." + item->Member.Text().data();
			return JS_NewString(context, text.c_str());
		}

		// `a.Equals(b)`, because JavaScript's `===` compares object identity and
		// cannot be overloaded. The Luau side spells this `a == b` through
		// `__eq`; this is the same comparison under the name the language leaves
		// available, and it is the `a.mul(b)` situation exactly.
		JSValue EnumItemEquals(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const auto *left =
				static_cast<EnumItemPayload *>(JS_GetOpaque2(context, self, bound.EnumItemClass));
			if (left == nullptr || argc < 1) {
				return JS_ThrowTypeError(context, "Equals needs an EnumItem");
			}

			const auto *right =
				static_cast<EnumItemPayload *>(JS_GetOpaque2(context, argv[0], bound.EnumItemClass));
			if (right == nullptr) {
				JS_FreeValue(context, JS_GetException(context));
				return JS_NewBool(context, 0);
			}
			return JS_NewBool(context, left->Enum == right->Enum && left->Member == right->Member);
		}

		JSValue MakeEnumItem(JSContext *context, const Name &enumName, const Name &member) {
			JsContext &bound = JsOf(context);

			JSValue object = JS_NewObjectClass(context, static_cast<int>(bound.EnumItemClass));
			if (JS_IsException(object)) {
				return object;
			}

			JS_SetOpaque(object, new EnumItemPayload{enumName, member});
			JS_PreventExtensions(context, object);
			return object;
		}

		bool ReadEnumValueImpl(JSContext *context, JSValueConst value, const Name &enumName, Name &out) {
			JsContext &bound = JsOf(context);

			if (auto *item =
					static_cast<EnumItemPayload *>(JS_GetOpaque2(context, value, bound.EnumItemClass));
				item != nullptr) {
				if (item->Enum != enumName) {
					return false;
				}
				out = item->Member;
				return true;
			}

			// The class check above throws when the object is of another class.
			// Cleared, because a bare string is a legitimate second form rather
			// than a failure.
			JS_FreeValue(context, JS_GetException(context));

			const char *text = JS_ToCString(context, value);
			if (text == nullptr) {
				return false;
			}
			out = Name(text);
			JS_FreeCString(context, text);
			return true;
		}

		// `Enum` — rebuilt from the registry on every read.
		JSValue EnumGet(JSContext *context, JSValueConst) {
			JSValue table = JS_NewObject(context);

			for (const Name enumName : ecs::EnumTable::Names()) {
				JSValue set = JS_NewObject(context);
				for (const Name member : ecs::EnumTable::MembersOf(enumName)) {
					JS_SetPropertyStr(
						context, set, member.Text().data(), MakeEnumItem(context, enumName, member)
					);
				}

				// Sealed, so a script cannot add a member that no property would
				// ever accept — the storage checks against `EnumTable`, and a
				// value only userland knew about would be refused on write with
				// nothing explaining where it came from.
				JS_PreventExtensions(context, set);
				JS_SetPropertyStr(context, table, enumName.Text().data(), set);
			}

			JS_PreventExtensions(context, table);
			return table;
		}

		JSValue InstanceNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			if (argc < 1) {
				return JS_ThrowTypeError(context, "Instance.new needs a class name");
			}

			const char *className = JS_ToCString(context, argv[0]);
			if (className == nullptr) {
				return JS_EXCEPTION;
			}

			const ecs::ClassId id = ecs::Classes::Find(Name(className));
			if (!id.IsValid()) {
				JSValue error = JS_ThrowTypeError(context, "'%s' is not a registered class", className);
				JS_FreeCString(context, className);
				return error;
			}

			const Entity instance = bound.World->CreateInstance(id, className);
			JS_FreeCString(context, className);

			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "could not create the instance");
			}

			// **The second argument, which Roblox has and v0.5 did not.**
			// `Instance.new('Part', workspace)` is one call rather than two, and
			// the difference is not only brevity: a part created and parented in
			// one statement is never briefly an orphan, so nothing that walks
			// the tree can observe the half-built state.
			//
			// Omitting it leaves the instance parented to nothing, which is now
			// a real state: fully formed, drawn by nothing, reached by no walk
			// of the tree until a script says where it goes.
			if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
				if (!bound.World->SetParent(instance, JsEntityOf(context, argv[1]))) {
					return JS_ThrowTypeError(context, "could not parent the new instance");
				}
			}

			JSValue proto = PrototypeFor(context, id, instance);
			JSValue object = JS_NewObjectProtoClass(context, proto, static_cast<int>(bound.InstanceClass));
			JS_FreeValue(context, proto);

			if (JS_IsException(object)) {
				return object;
			}
			JS_SetOpaque(object, new Entity(instance));

			// Sealed, so a script cannot add a field of its own. An instance is
			// an entity and its properties are what the class table declares —
			// a JavaScript object that quietly accepted `part.Transparency`
			// would be a second, invisible place for state to live, which is
			// exactly the "no scripting-only view" rule.
			JS_PreventExtensions(context, object);
			return object;
		}

		// --- constructors ----------------------------------------------------

		JSValue Vector3New(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			double x = 0.0;
			double y = 0.0;
			double z = 0.0;
			if (argc > 0) {
				JS_ToFloat64(context, &x, argv[0]);
			}
			if (argc > 1) {
				JS_ToFloat64(context, &y, argv[1]);
			}
			if (argc > 2) {
				JS_ToFloat64(context, &z, argv[2]);
			}
			return MakeVector3(
				context, core::Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}
			);
		}

		JSValue Color3FromRgb(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			if (argc > 0) {
				JS_ToFloat64(context, &r, argv[0]);
			}
			if (argc > 1) {
				JS_ToFloat64(context, &g, argv[1]);
			}
			if (argc > 2) {
				JS_ToFloat64(context, &b, argv[2]);
			}
			return MakeColor3(
				context,
				core::Color3::FromLinear(
					static_cast<float>(r / 255.0),
					static_cast<float>(g / 255.0),
					static_cast<float>(b / 255.0)
				)
			);
		}

		JSValue CFrameNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			// A `Vector3` or three numbers, for the reason the Luau side gives.
			if (argc > 0) {
				if (const core::Vector3 *position = AsVector3(context, argv[0]); position != nullptr) {
					return MakeCFrame(context, core::CFrame{*position});
				}
			}

			double x = 0.0;
			double y = 0.0;
			double z = 0.0;
			if (argc > 0) {
				JS_ToFloat64(context, &x, argv[0]);
			}
			if (argc > 1) {
				JS_ToFloat64(context, &y, argv[1]);
			}
			if (argc > 2) {
				JS_ToFloat64(context, &z, argv[2]);
			}
			return MakeCFrame(
				context,
				core::CFrame{
					core::Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}
				}
			);
		}

		// Radians, matching Roblox and matching the Luau binding.
		JSValue CFrameAngles(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			double pitch = 0.0;
			double yaw = 0.0;
			double roll = 0.0;
			if (argc > 0) {
				JS_ToFloat64(context, &pitch, argv[0]);
			}
			if (argc > 1) {
				JS_ToFloat64(context, &yaw, argv[1]);
			}
			if (argc > 2) {
				JS_ToFloat64(context, &roll, argv[2]);
			}
			return MakeCFrame(
				context,
				core::CFrame::Angles(
					static_cast<float>(pitch), static_cast<float>(yaw), static_cast<float>(roll)
				)
			);
		}

		// `CFrame.lookAt(from, to, up)` — see the Luau side for why a camera
		// needs it and what its absence cost.
		JSValue CFrameLookAt(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "lookAt needs a from and a to");
			}

			const core::Vector3 *from = AsVector3(context, argv[0]);
			const core::Vector3 *to = AsVector3(context, argv[1]);
			if (from == nullptr || to == nullptr) {
				return JS_ThrowTypeError(context, "lookAt needs two Vector3s");
			}

			const core::Vector3 *up = argc > 2 ? AsVector3(context, argv[2]) : nullptr;
			return MakeCFrame(
				context, core::CFrame::LookAt(*from, *to, up != nullptr ? *up : core::Vector3::YAxis)
			);
		}

		// **A method rather than an operator**, because JavaScript has no
		// operator overloading. `a.mul(b)` is the honest spelling: Luau writes
		// `a * b` because Luau can, and neither language is being made to
		// pretend it is the other.
		JSValue CFrameMul(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::CFrame *left = AsCFrame(context, self);
			if (left == nullptr || argc < 1) {
				return JS_ThrowTypeError(context, "mul needs a CFrame");
			}

			if (const core::Vector3 *point = AsVector3(context, argv[0]); point != nullptr) {
				return MakeVector3(context, left->PointToWorldSpace(*point));
			}

			const core::CFrame *right = AsCFrame(context, argv[0]);
			if (right == nullptr) {
				return JS_ThrowTypeError(context, "mul needs a CFrame or a Vector3");
			}
			return MakeCFrame(context, *left * *right);
		}

		JSValue CFramePosition(JSContext *context, JSValueConst self, int) {
			const core::CFrame *value = AsCFrame(context, self);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a CFrame");
			}
			return MakeVector3(context, value->Position);
		}

		JSValue GetService(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1) {
				return JS_ThrowTypeError(context, "GetService needs a name");
			}

			const char *name = JS_ToCString(context, argv[0]);
			JSValue global = JS_GetGlobalObject(context);
			JSValue service = JS_GetPropertyStr(context, global, name);
			JS_FreeValue(context, global);

			if (JS_IsUndefined(service)) {
				JS_FreeValue(context, service);

				// **Then the tree, which the Luau half has always done and this
				// one did not.** `Players`, `StarterGui` and the rest are
				// ordinary instances `InstallServices` puts at the root, so
				// looking them up by name is looking them up the way everything
				// else in the world is looked up.
				//
				// Without this the two languages disagreed about what a service
				// *is*: a Luau script reached `StarterGui` and a JavaScript one
				// was told the engine does not provide it — which is the parity
				// the roadmap's gate exists to refuse, and it was found by the
				// first TypeScript panel that tried to parent a `ScreenGui`.
				const ecs::Entity found = JsOf(context).World->FindFirstRoot(name);
				if (found != ecs::NULL_ENTITY) {
					JS_FreeCString(context, name);
					return MakeJsInstance(context, found);
				}

				// **Which refusal, from the catalogue**, for the reason the Luau
				// half gives: a service the *other* language binds is a different
				// failure from one the engine does not have. Every surface service
				// is in both languages since v0.16, so the narrow sentence is now
				// `BreakpointService`'s alone — which is a debugger feature this
				// language does not have rather than a binding nobody wrote.
				const ServiceDefinition *known = FindService(name);
				const bool elsewhere =
					known != nullptr && !Binds(known->Languages, ServiceLanguages::JavaScript);

				JSValue error =
					elsewhere
						? JS_ThrowTypeError(context, "'%s' is not bound for JavaScript in this engine", name)
						: JS_ThrowTypeError(context, "'%s' is not a service this engine provides", name);
				JS_FreeCString(context, name);
				return error;
			}

			JS_FreeCString(context, name);
			return service;
		}

		JSValue Color3New(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			if (argc > 0) {
				JS_ToFloat64(context, &r, argv[0]);
			}
			if (argc > 1) {
				JS_ToFloat64(context, &g, argv[1]);
			}
			if (argc > 2) {
				JS_ToFloat64(context, &b, argv[2]);
			}
			return MakeColor3(
				context,
				core::Color3::FromLinear(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b))
			);
		}

		// MessagingService.PublishAsync(topic, message)
		//
		// The only way out of a world, for the reason the Luau side gives:
		// a script holds one `Store` and no binding hands it another.
		JSValue PublishAsync(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "PublishAsync needs a topic and a message");
			}

			const char *topic = JS_ToCString(context, argv[0]);
			if (topic == nullptr) {
				return JS_EXCEPTION;
			}

			// **Through the codec, not `JS_ToCString`.** v0.5 stringified the
			// message because the codec did not exist, which meant an object
			// crossed as `"[object Object]"` and a function crossed as its own
			// source — both accepted, both meaningless on the far side. The
			// codec refuses each by name instead, and an object crosses as the
			// map a Luau subscriber receives.
			ScriptValue tree;
			CodecStatus why = CodecStatus::Ok;

			if (!ToScriptValue(context, argv[1], tree, 0, why)) {
				JSValue error =
					JS_ThrowTypeError(context, "the value cannot cross a world boundary: %s", Describe(why));
				JS_FreeCString(context, topic);
				return error;
			}

			std::vector<std::byte> payload;
			if (const CodecStatus status = Encode(tree, payload); status != CodecStatus::Ok) {
				JSValue error = JS_ThrowTypeError(
					context, "the value cannot cross a world boundary: %s", Describe(status)
				);
				JS_FreeCString(context, topic);
				return error;
			}

			world::Postbox box(*JsOf(context).World);
			const bool sent = box.Publish(topic, payload);

			JSValue error = JS_UNDEFINED;
			if (!sent) {
				// Over budget. Named rather than silent: each bus gives a world
				// an allowance per tick, and a publish that vanished would look
				// like a subscriber that never fired.
				error = JS_ThrowTypeError(context, "PublishAsync: over this world's budget for '%s'", topic);
			}

			JS_FreeCString(context, topic);
			return sent ? JS_UNDEFINED : error;
		}

		// TeleportService.Teleport(placeName, player, data?)
		//
		// The Luau side in `Services.cpp` carries the argument for every line of
		// this; what is here is the same operation through QuickJS's calling
		// convention. **Both languages or neither**: the engine's own record
		// says a surface promised in a declaration file and missing from one run
		// time survives two versions before anybody notices, so a
		// `TeleportService` a Luau script could reach and a JavaScript one could
		// not is the exact shape of that bug.
		JSValue Teleport(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "Teleport needs a place name and a player");
			}

			const char *place = JS_ToCString(context, argv[0]);
			if (place == nullptr) {
				return JS_EXCEPTION;
			}

			ecs::Store &store = *JsOf(context).World;
			const ecs::Entity player = JsEntityOf(context, argv[1]);

			if (!store.Alive(player) || !store.IsA(player, scene::PlayerClass())) {
				JS_FreeCString(context, place);
				return JS_ThrowTypeError(context, "Teleport: the second argument must be a Player");
			}

			world::Postbox box(store);
			if (box.IsReplica() || store.AdoptOnly()) {
				JS_FreeCString(context, place);
				return JS_ThrowTypeError(
					context, "Teleport: this world is a replica and does not decide who is in it"
				);
			}

			ScriptValue envelope{ValueTag::Map};

			ScriptValue label{ValueTag::String};
			const core::Name name = store.InstanceNameOf(player);
			label.Text = name.IsValid() ? std::string(name.Text()) : std::string("Player");
			envelope.Entries.emplace_back("Player", std::move(label));

			if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
				ScriptValue data;
				CodecStatus why = CodecStatus::Ok;
				if (!ToScriptValue(context, argv[2], data, 0, why)) {
					JS_FreeCString(context, place);
					return JS_ThrowTypeError(
						context, "Teleport: the data cannot cross a world boundary: %s", Describe(why)
					);
				}
				envelope.Entries.emplace_back("Data", std::move(data));
			}

			std::vector<std::byte> payload;
			if (const CodecStatus status = Encode(envelope, payload); status != CodecStatus::Ok) {
				JS_FreeCString(context, place);
				return JS_ThrowTypeError(
					context, "Teleport: the data cannot cross a world boundary: %s", Describe(status)
				);
			}

			const bool queued = box.Teleport(place, payload).Value != world::Ticket::NONE;
			if (!queued) {
				JSValue error =
					JS_ThrowTypeError(context, "Teleport: over this world's budget for '%s'", place);
				JS_FreeCString(context, place);
				return error;
			}
			JS_FreeCString(context, place);

			// Removed here for the reason the Luau side gives: the two worlds
			// cannot reach each other, so only this one can stop the player
			// being in both.
			(void)scene::RemoveCharacter(store, player);
			store.DestroyInstance(player);
			return JS_UNDEFINED;
		}

		// TeleportService.GetLocalPlayerTeleportData()
		JSValue GetLocalPlayerTeleportData(JSContext *context, JSValueConst, int, JSValueConst *) {
			ecs::Store &store = *JsOf(context).World;

			const auto *local = store.Resource<scene::LocalPlayer>();
			if (local == nullptr || !store.Alive(local->Instance)) {
				return JS_NULL;
			}

			const ecs::Entity held = store.FindFirstChild(local->Instance, "TeleportData");
			const auto *text = held == ecs::NULL_ENTITY ? nullptr : store.Get<scene::TextContent>(held);
			if (text == nullptr || text->Value.empty()) {
				return JS_NULL;
			}

			const auto *bytes = reinterpret_cast<const std::byte *>(text->Value.data());

			ScriptValue value;
			if (Decode({bytes, text->Value.size()}, value) != CodecStatus::Ok) {
				return JS_NULL;
			}

			return FromScriptValue(context, value);
		}

		JSValue SubscribeAsync(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2 || !JS_IsFunction(context, argv[1])) {
				return JS_ThrowTypeError(context, "SubscribeAsync needs a topic and a function");
			}

			const char *topic = JS_ToCString(context, argv[0]);
			if (topic == nullptr) {
				return JS_EXCEPTION;
			}

			JsContext &bound = JsOf(context);
			world::Postbox box(*bound.World);
			if (!box.Subscribe(topic)) {
				JSValue error =
					JS_ThrowTypeError(context, "SubscribeAsync: over this world's budget for '%s'", topic);
				JS_FreeCString(context, topic);
				return error;
			}

			bound.Subscriptions[topic].push_back(Retain(context, argv[1]));
			JS_FreeCString(context, topic);
			return JS_UNDEFINED;
		}

		JSValue Print(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			std::string line;
			for (int index = 0; index < argc; index++) {
				if (index > 0) {
					line += ' ';
				}
				const char *text = JS_ToCString(context, argv[index]);
				if (text != nullptr) {
					line += text;
					JS_FreeCString(context, text);
				}
			}
			ENGINE_INFO("[script] {}", line);
			return JS_UNDEFINED;
		}

		// Accessors for the value types, by field name.
		JSValue Vector3Get(JSContext *context, JSValueConst self, int magic) {
			const core::Vector3 *value = AsVector3(context, self);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Vector3");
			}
			return JS_NewFloat64(context, magic == 0 ? value->X : magic == 1 ? value->Y : value->Z);
		}

		// **Arithmetic as methods, because JavaScript has no operator
		// overloading.** Luau writes `a + b` through `__add`; this is the same
		// operation under the name the language leaves available, and it is the
		// `a.mul(b)` situation `CFrame` already had. Without these a JavaScript
		// author cannot move a part relative to where it is, which is most of
		// what a scene script does.
		JSValue Vector3Add(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::Vector3 *left = AsVector3(context, self);
			const core::Vector3 *right = argc > 0 ? AsVector3(context, argv[0]) : nullptr;

			if (left == nullptr || right == nullptr) {
				return JS_ThrowTypeError(context, "add needs a Vector3");
			}
			return MakeVector3(context, *left + *right);
		}

		JSValue Vector3Sub(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::Vector3 *left = AsVector3(context, self);
			const core::Vector3 *right = argc > 0 ? AsVector3(context, argv[0]) : nullptr;

			if (left == nullptr || right == nullptr) {
				return JS_ThrowTypeError(context, "sub needs a Vector3");
			}
			return MakeVector3(context, *left - *right);
		}

		JSValue Vector3Mul(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::Vector3 *left = AsVector3(context, self);
			if (left == nullptr || argc < 1) {
				return JS_ThrowTypeError(context, "mul needs a number or a Vector3");
			}

			// Component-wise against another vector, matching Roblox — and not
			// a dot product, which is the confusion `Vector3::operator*`
			// already carries a comment about.
			if (const core::Vector3 *right = AsVector3(context, argv[0]); right != nullptr) {
				return MakeVector3(context, *left * *right);
			}

			double scalar = 1.0;
			JS_ToFloat64(context, &scalar, argv[0]);
			return MakeVector3(context, *left * static_cast<float>(scalar));
		}

		JSValue Vector3Equals(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::Vector3 *left = AsVector3(context, self);
			const core::Vector3 *right = argc > 0 ? AsVector3(context, argv[0]) : nullptr;

			return JS_NewBool(context, left != nullptr && right != nullptr && *left == *right);
		}

		JSValue Vector3Derived(JSContext *context, JSValueConst self, int magic) {
			const core::Vector3 *value = AsVector3(context, self);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Vector3");
			}

			if (magic == 0) {
				return JS_NewFloat64(context, value->Magnitude());
			}
			return MakeVector3(context, value->Unit());
		}

		JSValue Color3Equals(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const core::Color3 *left = AsColor3(context, self);
			const core::Color3 *right = argc > 0 ? AsColor3(context, argv[0]) : nullptr;

			return JS_NewBool(
				context,
				left != nullptr && right != nullptr && left->R == right->R && left->G == right->G &&
					left->B == right->B
			);
		}

		JSValue Color3Get(JSContext *context, JSValueConst self, int magic) {
			const core::Color3 *value = AsColor3(context, self);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Color3");
			}
			return JS_NewFloat64(context, magic == 0 ? value->R : magic == 1 ? value->G : value->B);
		}

	}

	bool ReadJsEnumValue(JSContext *context, JSValueConst value, core::Name enumName, core::Name &out) {
		return ReadEnumValueImpl(context, value, enumName, out);
	}

	JsContext &JsOf(JSContext *context) {
		return *static_cast<JsContext *>(JS_GetContextOpaque(context));
	}

	CallbackRef Retain(JSContext *context, JSValueConst value) {
		JsContext &bound = JsOf(context);

		// A recycled slot when there is one. Without this a game that connects
		// and disconnects every frame grows `Callables` by one index per frame
		// for the life of the world — which is not a leak of the *values*, but
		// is an unbounded vector nobody would think to look at.
		if (!bound.FreeRefs.empty()) {
			const CallbackRef reference = bound.FreeRefs.back();
			bound.FreeRefs.pop_back();
			bound.Callables[static_cast<size_t>(reference)] = JS_DupValue(context, value);
			return reference;
		}

		bound.Callables.push_back(JS_DupValue(context, value));
		return static_cast<CallbackRef>(bound.Callables.size() - 1);
	}

	void Release(JSContext *context, CallbackRef reference) {
		JsContext &bound = JsOf(context);
		const auto slot = static_cast<size_t>(reference);

		if (slot >= bound.Callables.size() || JS_IsUndefined(bound.Callables[slot])) {
			return;
		}

		JS_FreeValue(context, bound.Callables[slot]);
		bound.Callables[slot] = JS_UNDEFINED;
		bound.FreeRefs.push_back(reference);
	}

	JSValueConst Held(JSContext *context, CallbackRef reference) {
		JsContext &bound = JsOf(context);
		const auto slot = static_cast<size_t>(reference);

		return slot < bound.Callables.size() ? bound.Callables[slot] : JS_UNDEFINED;
	}

	// One instance object for an entity, prototype and all.
	//
	// Shared by `Instance.new`, by `workspace` and by every `Reference`
	// property that hands one back — three call sites that had grown three
	// copies of the same six lines before this existed.
	JSValue MakeJsInstance(JSContext *context, Entity instance) {
		JsContext &bound = JsOf(context);
		if (instance == ecs::NULL_ENTITY) {
			return JS_NULL;
		}

		// **Alive, not "has a class", and the difference arrived with v0.12.**
		// This used to refuse anything `ClassOf` could not name, which conflated
		// two things: a handle naming nothing, and an entity that is simply not
		// an instance of a registered class. The second is an ordinary state —
		// `World.CreateEntity()` produces one — and refusing it meant the ECS
		// surface handed back `null` for every entity it created.
		//
		// A dead handle is still `null`, which is the check that was doing the
		// real work: an object standing for a row that no longer exists would
		// answer every question with a plausible default.
		//
		// `PrototypeFor` needs no case for it. An invalid class has no
		// properties, so it builds the one prototype carrying the shared methods
		// and nothing else, and caches it under the invalid id — which is what a
		// classless entity's members are.
		if (!bound.World->Alive(instance)) {
			return JS_NULL;
		}

		JSValue proto = PrototypeFor(context, bound.World->ClassOf(instance), instance);
		JSValue object = JS_NewObjectProtoClass(context, proto, static_cast<int>(bound.InstanceClass));
		JS_FreeValue(context, proto);

		if (JS_IsException(object)) {
			return object;
		}

		JS_SetOpaque(object, new Entity(instance));
		JS_PreventExtensions(context, object);
		return object;
	}

	namespace {
		// `workspace.CurrentCamera`, and the JavaScript half of `LuauCamera.cpp`.
		//
		// **It was missing, and missing in the worst available way.** The Luau
		// side special-cases this pair in `InstanceIndex`/`InstanceNewIndex`
		// because the property projects onto no component — it is a *resource*,
		// `scene::ActiveCamera`, holding which eye the renderer resolves. Nothing
		// did that here, and the world object is sealed with
		// `JS_PreventExtensions` before it reaches a script, so
		// `workspace.CurrentCamera = view` did not add a property, did not throw
		// outside strict mode, and did not aim the camera.
		// `mono.engine/examples/Mirrors-1-world.ts` has been writing it since it
		// was ported from the Luau file.
		//
		// Installed on the world object rather than on a prototype, for the
		// reason `Raycast` is: only the Workspace answers it, and offering it on
		// a `Folder` would be offering an answer that means nothing.
		JSValue CurrentCameraGet(JSContext *context, JSValueConst) {
			ecs::Store &store = *JsOf(context).World;

			const auto *active = store.Resource<scene::ActiveCamera>();
			if (active == nullptr || active->Entity == ecs::NULL_ENTITY || !store.Alive(active->Entity)) {
				// **Null rather than a camera made on demand**, which is the Luau
				// side's answer and for its reason: a headless world genuinely has
				// none, and minting a row so a property has something to point at
				// would put a phantom camera in every server world.
				return JS_NULL;
			}

			return MakeJsInstance(context, active->Entity);
		}

		JSValue CurrentCameraSet(JSContext *context, JSValueConst, JSValueConst value) {
			ecs::Store &store = *JsOf(context).World;

			// The aspect ratio is the *consumer's* — a window wrote it — so it
			// survives a camera change. Read first and kept, exactly as
			// `SetCurrentCamera` does; clearing it would make the next resolved
			// frame use a ratio of one and stretch every view.
			scene::ActiveCamera active;
			if (const auto *existing = store.Resource<scene::ActiveCamera>(); existing != nullptr) {
				active = *existing;
			}

			// `null` and `undefined` both detach. Detaching is a real operation:
			// a script tearing down a cutscene camera wants the world to have
			// none rather than to keep pointing at a row it is about to destroy.
			if (JS_IsNull(value) || JS_IsUndefined(value)) {
				active.Entity = ecs::NULL_ENTITY;
				store.SetResource(active);
				return JS_UNDEFINED;
			}

			const Entity camera = JsEntityOf(context, value);

			// Refused when the instance carries no `Camera`, rather than accepted
			// and quietly ignored by `ResolveActiveCamera` — which leaves the
			// matrices as they were, so the symptom would be a view that stopped
			// following anything with nothing reporting why.
			if (camera == ecs::NULL_ENTITY || store.Get<scene::Camera>(camera) == nullptr) {
				return JS_ThrowTypeError(context, "CurrentCamera must be an instance carrying a Camera");
			}

			active.Entity = camera;
			store.SetResource(active);
			return JS_UNDEFINED;
		}
	}

	JSValue MakeJsEnumItem(JSContext *context, core::Name enumName, core::Name member) {
		// **A wrapper rather than a move, because the class id, the payload and
		// the finaliser are all this translation unit's.** What is outside is one
		// caller — the input pump, which hands a bound action's handler
		// `Enum.UserInputState.Begin` — and a second way to build an `EnumItem`
		// is exactly the kind of duplicate `ReadEnumValueImpl` already exists to
		// avoid on the way in.
		return MakeEnumItem(context, enumName, member);
	}

	// --- the services, one installer each --------------------------------------
	//
	// **One function per service, and that is what `ServiceCatalogue.cpp` needs
	// to name.** These were three anonymous blocks inside `OpenJsBindings` and
	// two more inside `InstallJsServices`, which is a list of services written as
	// control flow — unnameable, so the catalogue could not point at them, and
	// invisible to anything asking what this language actually binds.
	//
	// They stay in this file rather than moving beside their Luau twins, because
	// the handlers they name are in the anonymous namespace above and are this
	// translation unit's. What the two halves share is the *catalogue*, not a
	// file.

	void OpenJsMessagingService(JSContext *context, JSValueConst global) {
		// The Universe's bus, not this world's.
		JSValue service = JS_NewObject(context);
		JS_SetPropertyStr(
			context, service, "PublishAsync", JS_NewCFunction(context, PublishAsync, "PublishAsync", 2)
		);
		JS_SetPropertyStr(
			context, service, "SubscribeAsync", JS_NewCFunction(context, SubscribeAsync, "SubscribeAsync", 2)
		);
		JS_SetPropertyStr(context, global, "MessagingService", service);
	}

	void OpenJsTeleportService(JSContext *context, JSValueConst global) {
		// The only bus operation that names a world.
		//
		// **`GetTeleportData` is missing here and present in Luau**, which is one
		// of the drifts the catalogue exists to make visible rather than to
		// excuse. It is the authority's half of the call — what arrived with a
		// player somebody else sent — and a JavaScript server cannot ask it.
		JSValue service = JS_NewObject(context);
		JS_SetPropertyStr(context, service, "Teleport", JS_NewCFunction(context, Teleport, "Teleport", 3));
		JS_SetPropertyStr(
			context,
			service,
			"GetLocalPlayerTeleportData",
			JS_NewCFunction(context, GetLocalPlayerTeleportData, "GetLocalPlayerTeleportData", 0)
		);
		JS_SetPropertyStr(context, global, "TeleportService", service);
	}

	void OpenJsBindings(JSContext *context, ecs::Store &store, const HostRole &role) {
		auto *bound = new JsContext();
		bound->World = &store;
		bound->Role = role;
		bound->Js = context;
		JS_SetContextOpaque(context, bound);

		// **Before anything builds a prototype.** `PrototypeFor` chains every
		// class prototype behind `__instanceMethods` and caches what it builds,
		// so a prototype made before that object existed kept a plain one for
		// the life of the VM — which is what left the `workspace` global below
		// with no `IsA`, no `GetChildren` and no signals.
		InstallJsInstanceMethods(context);

		JSRuntime *runtime = JS_GetRuntime(context);

		// Classes exist so an object of the wrong kind is caught by the VM
		// rather than by a cast that happens to line up — `Vector3` and
		// `Color3` are three floats each.
		static const JSClassDef instanceClass = {
			"Instance",
			[](JSRuntime *, JSValue value) {
				JSClassID id = 0;
				delete static_cast<Entity *>(JS_GetAnyOpaque(value, &id));
			},
			nullptr,
			nullptr,
			nullptr
		};
		// **`JS_GetAnyOpaque`, not `JS_GetOpaque(value, 0)`** — and that was a
		// real leak rather than a style point. A finalizer cannot capture, so
		// it has no way to name the class id it was registered under, and
		// `JS_GetOpaque` returns null whenever the id does not match. Passing
		// zero therefore freed nothing: every `Vector3` a script constructed
		// leaked its payload, silently, because the object itself was collected
		// correctly and only the twelve bytes behind it were not.
		static const JSClassDef vectorClass = {
			"Vector3",
			[](JSRuntime *, JSValue value) {
				JSClassID id = 0;
				delete static_cast<core::Vector3 *>(JS_GetAnyOpaque(value, &id));
			},
			nullptr,
			nullptr,
			nullptr
		};
		static const JSClassDef colourClass = {
			"Color3",
			[](JSRuntime *, JSValue value) {
				JSClassID id = 0;
				delete static_cast<core::Color3 *>(JS_GetAnyOpaque(value, &id));
			},
			nullptr,
			nullptr,
			nullptr
		};
		static const JSClassDef frameClass = {
			"CFrame",
			[](JSRuntime *, JSValue value) {
				JSClassID id = 0;
				delete static_cast<core::CFrame *>(JS_GetAnyOpaque(value, &id));
			},
			nullptr,
			nullptr,
			nullptr
		};

		static const JSClassDef enumItemClass = {
			"EnumItem",
			[](JSRuntime *, JSValue value) {
				JSClassID id = 0;
				delete static_cast<EnumItemPayload *>(JS_GetAnyOpaque(value, &id));
			},
			nullptr,
			nullptr,
			nullptr
		};

		JS_NewClassID(runtime, &bound->EnumItemClass);
		JS_NewClass(runtime, bound->EnumItemClass, &enumItemClass);

		JS_NewClassID(runtime, &bound->InstanceClass);
		JS_NewClassID(runtime, &bound->Vector3Class);
		JS_NewClassID(runtime, &bound->Color3Class);
		JS_NewClassID(runtime, &bound->CFrameClass);

		JS_NewClass(runtime, bound->InstanceClass, &instanceClass);
		JS_NewClass(runtime, bound->Vector3Class, &vectorClass);
		JS_NewClass(runtime, bound->Color3Class, &colourClass);
		JS_NewClass(runtime, bound->CFrameClass, &frameClass);

		JSValue global = JS_GetGlobalObject(context);

		// Vector3
		{
			JSValue proto = JS_NewObject(context);
			static const JSCFunctionListEntry fields[] = {
				JS_CGETSET_MAGIC_DEF("X", Vector3Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Y", Vector3Get, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("Z", Vector3Get, nullptr, 2),
				JS_CGETSET_MAGIC_DEF("Magnitude", Vector3Derived, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Unit", Vector3Derived, nullptr, 1),
				JS_CFUNC_DEF("add", 1, Vector3Add),
				JS_CFUNC_DEF("sub", 1, Vector3Sub),
				JS_CFUNC_DEF("mul", 1, Vector3Mul),
				JS_CFUNC_DEF("Equals", 1, Vector3Equals),
			};
			JS_SetPropertyFunctionList(context, proto, fields, 9);
			JS_SetClassProto(context, bound->Vector3Class, proto);

			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, Vector3New, "new", 3));

			// **The same two constants the Luau surface carries, spelled the same
			// way.** Two bindings over one engine is exactly the drift a declared
			// property prevents for components, and there is no such mechanism for
			// a library constant — so the only thing keeping `Vector3.zero`
			// meaning the same in both languages is that both are written here and
			// in `Values.cpp` from `core::Vector3`'s own members.
			//
			// Lowercase, because Roblox's are. `Values.cpp` carries the argument.
			JS_SetPropertyStr(context, table, "zero", MakeVector3(context, core::Vector3::Zero));
			JS_SetPropertyStr(context, table, "one", MakeVector3(context, core::Vector3::One));
			JS_SetPropertyStr(context, global, "Vector3", table);
		}

		// Color3
		{
			JSValue proto = JS_NewObject(context);
			static const JSCFunctionListEntry fields[] = {
				JS_CGETSET_MAGIC_DEF("R", Color3Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("G", Color3Get, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("B", Color3Get, nullptr, 2),
				JS_CFUNC_DEF("Equals", 1, Color3Equals),
			};
			JS_SetPropertyFunctionList(context, proto, fields, 4);
			JS_SetClassProto(context, bound->Color3Class, proto);

			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, Color3New, "new", 3));
			JS_SetPropertyStr(
				context, table, "fromRGB", JS_NewCFunction(context, Color3FromRgb, "fromRGB", 3)
			);
			JS_SetPropertyStr(context, global, "Color3", table);
		}

		// CFrame
		{
			JSValue proto = JS_NewObject(context);
			JS_SetPropertyStr(context, proto, "mul", JS_NewCFunction(context, CFrameMul, "mul", 1));
			static const JSCFunctionListEntry fields[] = {
				JS_CGETSET_MAGIC_DEF("Position", CFramePosition, nullptr, 0),
			};
			JS_SetPropertyFunctionList(context, proto, fields, 1);
			JS_SetClassProto(context, bound->CFrameClass, proto);

			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, CFrameNew, "new", 3));
			JS_SetPropertyStr(context, table, "Angles", JS_NewCFunction(context, CFrameAngles, "Angles", 3));
			JS_SetPropertyStr(context, table, "lookAt", JS_NewCFunction(context, CFrameLookAt, "lookAt", 3));
			JS_SetPropertyStr(context, global, "CFrame", table);
		}

		// Before anything constructs a `Postbox` — see `Services.cpp` for what
		// happens otherwise, which is an abort in an unrelated test.
		world::RegisterMailboxTypes();

		// **No services here any more.** They are installed from the catalogue in
		// `OpenJsSurface`, which is the later of this language's two install
		// halves — see `ServiceCatalogue.hpp`. `world::RegisterMailboxTypes()`
		// above stays, because it must run before anything constructs a
		// `Postbox` and that is earlier than either walk.

		// `nil`, because this is a Roblox-shaped API and a Roblox author writes
		// `part.Parent = nil`. An alias for `null` rather than a third empty
		// value: JavaScript already has two and a third would be a footgun
		// wearing a familiar name.
		JS_SetPropertyStr(context, global, "nil", JS_NULL);

		// game
		{
			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(
				context, table, "GetService", JS_NewCFunction(context, GetService, "GetService", 1)
			);

			// Which world this script is standing on — see the Luau side for
			// why a world's name is what `JobId` answers with. A plain string
			// property rather than a getter, because the name is fixed for the
			// life of the world and a getter would imply otherwise.
			{
				const std::string_view name = JsOf(context).World->Name();
				JS_SetPropertyStr(
					context, table, "JobId", JS_NewStringLen(context, name.data(), name.size())
				);
			}

			JS_SetPropertyStr(context, global, "game", table);
		}

		// EnumItem's prototype, and `Enum` as a getter.
		{
			JSValue proto = JS_NewObject(context);
			static const JSCFunctionListEntry fields[] = {
				JS_CGETSET_MAGIC_DEF("Name", EnumItemGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("EnumType", EnumItemGet, nullptr, 1),
			};
			JS_SetPropertyFunctionList(context, proto, fields, 2);
			JS_SetPropertyStr(
				context, proto, "Equals", JS_NewCFunction(context, EnumItemEquals, "Equals", 1)
			);
			JS_SetPropertyStr(
				context, proto, "toString", JS_NewCFunction(context, EnumItemToString, "toString", 0)
			);
			JS_SetClassProto(context, bound->EnumItemClass, proto);

			// A **getter**, not a value, and the cast is how QuickJS spells one:
			// `JS_CFUNC_getter` takes `(ctx, this)` rather than the generic
			// argc/argv form, so the pointer is reinterpreted at registration
			// exactly as the header's own `JS_CGETSET_DEF` macro does.
			const JSAtom name = JS_NewAtom(context, "Enum");
			JS_DefinePropertyGetSet(
				context,
				global,
				name,
				JS_NewCFunction2(
					context,
					reinterpret_cast<JSCFunction *>(reinterpret_cast<void *>(EnumGet)),
					"Enum",
					0,
					JS_CFUNC_getter,
					0
				),
				JS_UNDEFINED,
				JS_PROP_CONFIGURABLE
			);
			JS_FreeAtom(context, name);
		}

		// Instance
		{
			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, InstanceNew, "new", 1));
			JS_SetPropertyStr(context, global, "Instance", table);
		}

		// workspace — **this world's `Workspace` service**, and until v0.7 it
		// was a plain object standing for the world itself. `Bindings.hpp`
		// carries the whole reason the two were collapsed; the short version is
		// that a world now has a real `Workspace` instance, keeping both meant
		// two answers to "what is in the scene", and the renderer listened to
		// neither.
		{
			// Idempotent, and here so that a world always has one. See the Luau
			// `OpenWorkspace`, which does the same for the same reason.
			Entity workspace = scene::InstallServices(store);
			if (workspace == ecs::NULL_ENTITY) {
				workspace = scene::WorkspaceOf(store);
			}

			// Built out rather than through `MakeJsInstance`, which seals the
			// object before returning it — and this one takes `Raycast` first.
			// Everything else about it is an ordinary instance: same class,
			// same prototype, same opaque entity, so `IsA`, `GetChildren` and
			// every declared property arrive through the chain rather than
			// being listed again here.
			JSValue world = JS_NULL;
			if (const ecs::ClassId id = bound->World->ClassOf(workspace); id.IsValid()) {
				JSValue proto = PrototypeFor(context, id, workspace);
				world = JS_NewObjectProtoClass(context, proto, static_cast<int>(bound->InstanceClass));
				JS_FreeValue(context, proto);
				JS_SetOpaque(world, new Entity(workspace));
			} else {
				world = JS_NewObject(context);
			}

			// **Before the seal**, because a sealed object cannot take a method
			// — nor an accessor, which is what made `CurrentCamera`'s absence
			// silent rather than loud.
			InstallJsQueries(context, global, world);

			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_DEF("CurrentCamera", CurrentCameraGet, CurrentCameraSet),
			};
			JS_SetPropertyFunctionList(context, world, members, 1);

			JS_PreventExtensions(context, world);

			bound->Workspace = JS_DupValue(context, world);
			JS_SetPropertyStr(context, global, "workspace", world);
		}

		JS_SetPropertyStr(context, global, "print", JS_NewCFunction(context, Print, "print", 1));

		JS_FreeValue(context, global);
	}

	void CloseJsBindings(JSContext *context) {
		auto *bound = static_cast<JsContext *>(JS_GetContextOpaque(context));
		if (bound == nullptr) {
			return;
		}

		for (JSValue &held : bound->Owned) {
			JS_FreeValue(context, held);
		}
		for (auto &entry : bound->Prototypes) {
			JS_FreeValue(context, entry.second);
		}
		JS_FreeValue(context, bound->Workspace);

		// **The store's change listeners go before the VM does.** They capture
		// the queue, and a store that outlives the runtime would otherwise call
		// into freed memory at its next barrier — which is the ordinary case,
		// because a world is destroyed after the scripts that built it.
		if (bound->World != nullptr) {
			bound->Changes.Detach(*bound->World);
		}

		// Every retained callable, whatever holds its reference. One loop
		// because `Callables` is where they all actually live; the tables above
		// hold integers into it.
		for (JSValue &callable : bound->Callables) {
			JS_FreeValue(context, callable);
		}

		JS_SetContextOpaque(context, nullptr);
		delete bound;
	}

	std::string PumpJsHeartbeat(JSContext *context, float delta) {
		JSValue argument = JS_NewFloat64(context, delta);
		const std::string failure =
			FireJsSignal(context, SignalKind::Heartbeat, ecs::NULL_ENTITY, 1, &argument);
		JS_FreeValue(context, argument);
		return failure;
	}

	namespace {
		// A stable, human-readable name for a bus refusal.
		//
		// §5: "Named, not swallowed." Each of these is something a script author
		// has to be able to see and handle, so each arrives as a string beside
		// the value rather than as a null that could mean three things.
		const char *DescribeStatus(world::BusStatus status) {
			switch (status) {
			case world::BusStatus::Ok:
				return "Ok";
			case world::BusStatus::NotFound:
				return "NotFound";
			case world::BusStatus::Conflict:
				return "Conflict";
			case world::BusStatus::OverBudget:
				return "OverBudget";
			case world::BusStatus::NoSuchWorld:
				return "NoSuchWorld";
			case world::BusStatus::Unsupported:
				return "Unsupported";
			}
			return "Unknown";
		}
	}

	std::string PumpJsDeliveries(JSContext *context, ecs::Store &store) {
		auto *bound = static_cast<JsContext *>(JS_GetContextOpaque(context));
		if (bound == nullptr) {
			return {};
		}

		const world::Postbox box(store);
		const auto deliveries = box.Deliveries();
		if (deliveries.empty()) {
			return {};
		}

		std::string firstError;

		for (const world::Delivery &delivery : deliveries) {
			// **A reply first**, because a suspended script is waiting on it and
			// a subscriber is not. Both are barrier deliveries and both are legal
			// resume sources; the order is stated so a world whose script both
			// published and awaited sees them the same way twice.
			if (delivery.Reply.Expected()) {
				const auto waiting = bound->AwaitedTickets.find(delivery.Reply.Value);
				if (waiting == bound->AwaitedTickets.end()) {
					continue;
				}

				const CallbackRef resolver = waiting->second;
				bound->AwaitedTickets.erase(waiting);

				// `{ Value, Status, Version }` in one object, because a promise
				// resolves with a single value. §5's rule is that each refusal
				// has to be something a script can see, so the status rides
				// beside the value rather than being swallowed.
				JSValue reply = JS_NewObject(context);

				JSValue value = JS_NULL;
				if (delivery.Status == world::BusStatus::Ok && !delivery.Payload.empty()) {
					ScriptValue decoded;
					if (Decode(delivery.Payload, decoded) == CodecStatus::Ok) {
						value = FromScriptValue(context, decoded);
					}
				}

				JS_SetPropertyStr(context, reply, "Value", value);
				JS_SetPropertyStr(
					context, reply, "Status", JS_NewString(context, DescribeStatus(delivery.Status))
				);
				JS_SetPropertyStr(
					context, reply, "Version", JS_NewFloat64(context, static_cast<double>(delivery.Version))
				);

				JSValue result = JS_Call(context, Held(context, resolver), JS_UNDEFINED, 1, &reply);
				if (JS_IsException(result)) {
					JSValue thrown = JS_GetException(context);
					if (firstError.empty()) {
						if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
							firstError = text;
							JS_FreeCString(context, text);
						}
					}
					JS_FreeValue(context, thrown);
				}

				JS_FreeValue(context, result);
				JS_FreeValue(context, reply);
				Release(context, resolver);
				continue;
			}

			// **A channel message is delivered to the world rather than to a
			// topic**, so it goes to one signal with no subject rather than to a
			// list of subscribers keyed by name. See `SignalKind::
			// CrossWorldMessage` and `CrossWorldService.cpp`.
			//
			// **Absent until v0.16, which is why the service could not be bound
			// here.** `CrossWorldService` is described once now and both
			// languages install it; a `MessageReceived` this pump never fired
			// would have been a signal that exists and is silent, which reads as
			// a broken engine rather than an unfinished one.
			if (delivery.Bus == world::BusKind::Channel) {
				JSValue arguments[2];

				ScriptValue decoded;
				arguments[0] = Decode(delivery.Payload, decoded) == CodecStatus::Ok
								   ? FromScriptValue(context, decoded)
								   : JS_NULL;

				// **The sender's name, second, which is what makes a channel a
				// channel.** A topic subscriber is told which topic; a channel
				// receiver is told who to answer, because answering is the point
				// and the destination already knows it is itself.
				const std::string_view from = delivery.From.Text();
				arguments[1] = JS_NewStringLen(context, from.data(), from.size());

				std::string failure =
					FireJsSignal(context, SignalKind::CrossWorldMessage, ecs::NULL_ENTITY, 2, arguments);
				if (!failure.empty() && firstError.empty()) {
					firstError = std::move(failure);
				}

				JS_FreeValue(context, arguments[0]);
				JS_FreeValue(context, arguments[1]);
				continue;
			}

			if (delivery.Bus != world::BusKind::Messaging || bound->Subscriptions.empty()) {
				continue;
			}

			const auto found = bound->Subscriptions.find(std::string(delivery.Key.Text()));
			if (found == bound->Subscriptions.end()) {
				continue;
			}

			JSValue arguments[2];

			// **Decoded through the shared codec**, so a table published from
			// Luau arrives as an object here. v0.5 handed over a raw string
			// because the codec did not exist.
			ScriptValue decoded;
			arguments[0] = Decode(delivery.Payload, decoded) == CodecStatus::Ok
							   ? FromScriptValue(context, decoded)
							   : JS_NULL;
			arguments[1] = JS_NewString(context, delivery.Key.Text().data());

			for (const CallbackRef callback : found->second) {
				JSValue result = JS_Call(context, Held(context, callback), JS_UNDEFINED, 2, arguments);
				if (JS_IsException(result)) {
					JSValue thrown = JS_GetException(context);
					if (firstError.empty()) {
						if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
							firstError = text;
							JS_FreeCString(context, text);
						}
					}
					JS_FreeValue(context, thrown);
				}
				JS_FreeValue(context, result);
			}

			JS_FreeValue(context, arguments[0]);
			JS_FreeValue(context, arguments[1]);
		}
		return firstError;
	}
}
