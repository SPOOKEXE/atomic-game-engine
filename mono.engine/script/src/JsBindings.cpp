#include "JsBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/world/Postbox.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		// Everything one JS runtime needs to reach the world, hung off the
		// context rather than a static — two runtimes over two worlds must not
		// be able to reach each other's storage.
		struct Context {
			Store *World = nullptr;

			JSClassID InstanceClass = 0;
			JSClassID Vector3Class = 0;
			JSClassID Color3Class = 0;
			JSClassID CFrameClass = 0;

			// One prototype per ECS class, built the first time an instance of
			// it is made. **Accessors live on the prototype, not the object**:
			// a scene of five hundred parts would otherwise define five
			// thousand properties, and every one of them would be the same
			// closure over the same name.
			std::unordered_map<uint32_t, JSValue> Prototypes;

			// Kept so the prototypes can be freed with the context.
			std::vector<JSValue> Owned;

			// What `RunService.Heartbeat.Connect` was given.
			std::vector<JSValue> Heartbeat;

			// The world object, kept so `Parent` hands back the same value a
			// script assigned rather than a second object that behaves alike.
			JSValue Workspace = JS_UNDEFINED;

			// Topic to callbacks, one list per topic.
			std::unordered_map<std::string, std::vector<JSValue>> Subscriptions;
		};

		Context &Of(JSContext *context) {
			return *static_cast<Context *>(JS_GetContextOpaque(context));
		}

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
		return MakeValue(context, Of(context).Vector3Class, value);
	}

	JSValue MakeColor3(JSContext *context, const core::Color3 &value) {
		return MakeValue(context, Of(context).Color3Class, value);
	}

	JSValue MakeCFrame(JSContext *context, const core::CFrame &value) {
		return MakeValue(context, Of(context).CFrameClass, value);
	}

	core::Vector3 *AsVector3(JSContext *context, JSValueConst value) {
		return ValueOf<core::Vector3>(context, value, Of(context).Vector3Class);
	}

	core::Color3 *AsColor3(JSContext *context, JSValueConst value) {
		return ValueOf<core::Color3>(context, value, Of(context).Color3Class);
	}

	core::CFrame *AsCFrame(JSContext *context, JSValueConst value) {
		return ValueOf<core::CFrame>(context, value, Of(context).CFrameClass);
	}

	namespace {
		JSValue MakeInstance(JSContext *context, Entity instance);

		// --- marshalling -----------------------------------------------------
		//
		// **A switch over `PropertyType` and nothing else**, exactly as the Luau
		// side is. No property is named in either file, which is what makes one
		// property declaration reach both languages.

		JSValue ToJs(JSContext *context, const PropertyDescriptor &property, const void *bytes) {
			switch (property.Type) {
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
			case PropertyType::Vector3:
				return MakeVector3(context, *static_cast<const core::Vector3 *>(bytes));
			case PropertyType::Color3:
				return MakeColor3(context, *static_cast<const core::Color3 *>(bytes));
			case PropertyType::CFrame:
				return MakeCFrame(context, *static_cast<const core::CFrame *>(bytes));
			case PropertyType::Reference: {
				// A root instance's parent is the world, and the world is
				// `workspace`. Handing back null would make
				// `part.Parent = workspace` a write a script could not read
				// back, and the two would disagree about one fact.
				const Entity referenced = *static_cast<const Entity *>(bytes);
				if (referenced == ecs::NULL_ENTITY) {
					return JS_DupValue(context, Of(context).Workspace);
				}
				return MakeInstance(context, referenced);
			}
			case PropertyType::Opaque:
				break;
			}
			return JS_ThrowTypeError(
				context, "'%s' has no script representation", property.Name.Text().data()
			);
		}

		bool FromJs(JSContext *context, JSValueConst value, const PropertyDescriptor &property, void *out) {
			switch (property.Type) {
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
			case PropertyType::Name: {
				const char *text = JS_ToCString(context, value);
				if (text == nullptr) {
					return false;
				}
				*static_cast<Name *>(out) = Name(text);
				JS_FreeCString(context, text);
				return true;
			}
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
			case PropertyType::Reference: {
				// `part.Parent = workspace` — a root of this world. An instance
				// arrives as its object; `null` detaches, which is what
				// Roblox's `Parent = nil` means.
				if (JS_IsNull(value) || JS_IsUndefined(value) ||
					JS_IsStrictEqual(context, value, Of(context).Workspace)) {
					*static_cast<Entity *>(out) = ecs::NULL_ENTITY;
					return true;
				}

				void *opaque = JS_GetOpaque2(context, value, Of(context).InstanceClass);
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

		// --- instances -------------------------------------------------------

		Entity EntityOf(JSContext *context, JSValueConst object) {
			void *opaque = JS_GetOpaque2(context, object, Of(context).InstanceClass);
			return opaque == nullptr ? ecs::NULL_ENTITY : *static_cast<Entity *>(opaque);
		}

		const PropertyDescriptor *Find(const Store &store, Entity instance, const char *name) {
			const Name key(name);
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Name == key) {
					return &property;
				}
			}
			return nullptr;
		}

		// The accessor pair every property on a prototype is made of. The
		// property's name travels as closure data, so one function serves all of
		// them and none of them is written by hand.
		JSValue
		PropertyGet(JSContext *context, JSValueConst self, int, JSValueConst *, int, JSValueConst *data) {
			Context &bound = Of(context);
			const Entity instance = EntityOf(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			const char *name = JS_ToCString(context, data[0]);
			const PropertyDescriptor *property = Find(*bound.World, instance, name);
			JS_FreeCString(context, name);

			if (property == nullptr) {
				return JS_ThrowTypeError(context, "no such property");
			}

			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property->Size > sizeof(bytes) ||
				!bound.World->GetProperty(instance, property->Name, bytes, property->Size)) {
				return JS_ThrowTypeError(context, "could not read '%s'", property->Name.Text().data());
			}
			return ToJs(context, *property, bytes);
		}

		JSValue PropertySet(
			JSContext *context, JSValueConst self, int argc, JSValueConst *argv, int, JSValueConst *data
		) {
			Context &bound = Of(context);
			const Entity instance = EntityOf(context, self);
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

			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property->Size > sizeof(bytes) || !FromJs(context, argv[0], *property, bytes)) {
				return JS_ThrowTypeError(
					context, "'%s' cannot take that value", property->Name.Text().data()
				);
			}

			// Refused loudly. A replica rejecting the write is the case that
			// matters: a script author cannot tell "rejected" from "applied and
			// overwritten by the next delta" without being told.
			if (!bound.World->SetProperty(instance, property->Name, bytes, property->Size)) {
				return JS_ThrowTypeError(context, "could not set '%s'", property->Name.Text().data());
			}
			return JS_UNDEFINED;
		}

		// The prototype for one ECS class, built once and cached.
		JSValue PrototypeFor(JSContext *context, ecs::ClassId id, Entity sample) {
			Context &bound = Of(context);

			const auto cached = bound.Prototypes.find(id.Index);
			if (cached != bound.Prototypes.end()) {
				return JS_DupValue(context, cached->second);
			}

			JSValue proto = JS_NewObject(context);
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

		// One instance object for an entity, prototype and all.
		//
		// Shared by `Instance.new`, by `workspace` and by every `Reference`
		// property that hands one back — three call sites that had grown three
		// copies of the same six lines before this existed.
		JSValue MakeInstance(JSContext *context, Entity instance) {
			Context &bound = Of(context);
			if (instance == ecs::NULL_ENTITY) {
				return JS_NULL;
			}

			const ecs::ClassId id = bound.World->ClassOf(instance);
			if (!id.IsValid()) {
				return JS_NULL;
			}

			JSValue proto = PrototypeFor(context, id, instance);
			JSValue object = JS_NewObjectProtoClass(context, proto, static_cast<int>(bound.InstanceClass));
			JS_FreeValue(context, proto);

			if (JS_IsException(object)) {
				return object;
			}

			JS_SetOpaque(object, new Entity(instance));
			JS_PreventExtensions(context, object);
			return object;
		}

		JSValue InstanceNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			Context &bound = Of(context);
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
				JSValue error =
					JS_ThrowTypeError(context, "'%s' is not a service this engine provides", name);
				JS_FreeCString(context, name);
				JS_FreeValue(context, service);
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

		JSValue HeartbeatConnect(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1 || !JS_IsFunction(context, argv[0])) {
				return JS_ThrowTypeError(context, "Connect needs a function");
			}

			// Duplicated so the connection survives the script that made it.
			// The context owns it from here and frees it in CloseJsBindings.
			Of(context).Heartbeat.push_back(JS_DupValue(context, argv[0]));
			return JS_UNDEFINED;
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
			size_t length = 0;
			const char *message = JS_ToCStringLen(context, &length, argv[1]);
			if (topic == nullptr || message == nullptr) {
				JS_FreeCString(context, topic);
				JS_FreeCString(context, message);
				return JS_EXCEPTION;
			}

			world::Postbox box(*Of(context).World);
			const auto *bytes = reinterpret_cast<const std::byte *>(message);
			const bool sent = box.Publish(topic, {bytes, length});

			JSValue error = JS_UNDEFINED;
			if (!sent) {
				error = JS_ThrowTypeError(context, "PublishAsync: over this world's budget for '%s'", topic);
			}

			JS_FreeCString(context, topic);
			JS_FreeCString(context, message);
			return sent ? JS_UNDEFINED : error;
		}

		JSValue SubscribeAsync(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2 || !JS_IsFunction(context, argv[1])) {
				return JS_ThrowTypeError(context, "SubscribeAsync needs a topic and a function");
			}

			const char *topic = JS_ToCString(context, argv[0]);
			if (topic == nullptr) {
				return JS_EXCEPTION;
			}

			Context &bound = Of(context);
			world::Postbox box(*bound.World);
			if (!box.Subscribe(topic)) {
				JSValue error =
					JS_ThrowTypeError(context, "SubscribeAsync: over this world's budget for '%s'", topic);
				JS_FreeCString(context, topic);
				return error;
			}

			bound.Subscriptions[topic].push_back(JS_DupValue(context, argv[1]));
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

		JSValue Color3Get(JSContext *context, JSValueConst self, int magic) {
			const core::Color3 *value = AsColor3(context, self);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Color3");
			}
			return JS_NewFloat64(context, magic == 0 ? value->R : magic == 1 ? value->G : value->B);
		}

	}

	void OpenJsBindings(JSContext *context, ecs::Store &store) {
		auto *bound = new Context();
		bound->World = &store;
		JS_SetContextOpaque(context, bound);

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
			};
			JS_SetPropertyFunctionList(context, proto, fields, 3);
			JS_SetClassProto(context, bound->Vector3Class, proto);

			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, Vector3New, "new", 3));
			JS_SetPropertyStr(context, global, "Vector3", table);
		}

		// Color3
		{
			JSValue proto = JS_NewObject(context);
			static const JSCFunctionListEntry fields[] = {
				JS_CGETSET_MAGIC_DEF("R", Color3Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("G", Color3Get, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("B", Color3Get, nullptr, 2),
			};
			JS_SetPropertyFunctionList(context, proto, fields, 3);
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
			JS_SetPropertyStr(context, global, "CFrame", table);
		}

		// Before anything constructs a `Postbox` — see `Services.cpp` for what
		// happens otherwise, which is an abort in an unrelated test.
		world::RegisterMailboxTypes();

		// MessagingService — the Universe's, not this world's.
		{
			JSValue service = JS_NewObject(context);
			JS_SetPropertyStr(
				context, service, "PublishAsync", JS_NewCFunction(context, PublishAsync, "PublishAsync", 2)
			);
			JS_SetPropertyStr(
				context,
				service,
				"SubscribeAsync",
				JS_NewCFunction(context, SubscribeAsync, "SubscribeAsync", 2)
			);
			JS_SetPropertyStr(context, global, "MessagingService", service);
		}

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
			JS_SetPropertyStr(context, global, "game", table);
		}

		// Instance
		{
			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "new", JS_NewCFunction(context, InstanceNew, "new", 1));
			JS_SetPropertyStr(context, global, "Instance", table);
		}

		// RunService
		//
		// `Heartbeat.Connect(fn)` rather than `Heartbeat:Connect(fn)` — the
		// colon is Lua's, and a JavaScript author writes the dot. Same signal,
		// same list, spelled the way each language spells a method call.
		{
			JSValue heartbeat = JS_NewObject(context);
			JS_SetPropertyStr(
				context, heartbeat, "Connect", JS_NewCFunction(context, HeartbeatConnect, "Connect", 1)
			);

			JSValue service = JS_NewObject(context);
			JS_SetPropertyStr(context, service, "Heartbeat", heartbeat);
			JS_SetPropertyStr(context, global, "RunService", service);
		}

		// workspace — the world this script runs on, not an instance in it.
		// See `Bindings.hpp`: a world is what entities live in, and making it
		// an entity would put a phantom row in every scene.
		{
			JSValue world = JS_NewObject(context);
			JS_SetPropertyStr(
				context, world, "Name", JS_NewString(context, std::string(store.Name()).c_str())
			);
			JS_PreventExtensions(context, world);

			bound->Workspace = JS_DupValue(context, world);
			JS_SetPropertyStr(context, global, "workspace", world);
		}

		JS_SetPropertyStr(context, global, "print", JS_NewCFunction(context, Print, "print", 1));

		JS_FreeValue(context, global);
	}

	void CloseJsBindings(JSContext *context) {
		auto *bound = static_cast<Context *>(JS_GetContextOpaque(context));
		if (bound == nullptr) {
			return;
		}

		for (JSValue &held : bound->Owned) {
			JS_FreeValue(context, held);
		}
		for (auto &entry : bound->Prototypes) {
			JS_FreeValue(context, entry.second);
		}
		for (JSValue &connection : bound->Heartbeat) {
			JS_FreeValue(context, connection);
		}
		JS_FreeValue(context, bound->Workspace);
		for (auto &entry : bound->Subscriptions) {
			for (JSValue &callback : entry.second) {
				JS_FreeValue(context, callback);
			}
		}

		JS_SetContextOpaque(context, nullptr);
		delete bound;
	}

	std::string PumpJsHeartbeat(JSContext *context, float delta) {
		auto *bound = static_cast<Context *>(JS_GetContextOpaque(context));
		if (bound == nullptr) {
			return {};
		}

		std::string firstError;
		JSValue argument = JS_NewFloat64(context, delta);

		// Every connection runs even when one throws, for the reason the Luau
		// side gives: half a scene animating points nowhere near the cause.
		for (JSValue &connection : bound->Heartbeat) {
			JSValue result = JS_Call(context, connection, JS_UNDEFINED, 1, &argument);
			if (JS_IsException(result)) {
				if (firstError.empty()) {
					JSValue thrown = JS_GetException(context);
					if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
						firstError = text;
						JS_FreeCString(context, text);
					}
					JS_FreeValue(context, thrown);
				} else {
					JS_FreeValue(context, JS_GetException(context));
				}
			}
			JS_FreeValue(context, result);
		}

		JS_FreeValue(context, argument);
		return firstError;
	}

	std::string PumpJsDeliveries(JSContext *context, ecs::Store &store) {
		auto *bound = static_cast<Context *>(JS_GetContextOpaque(context));
		if (bound == nullptr || bound->Subscriptions.empty()) {
			return {};
		}

		const world::Postbox box(store);
		std::string firstError;

		for (const world::Delivery &delivery : box.Deliveries()) {
			if (delivery.Bus != world::BusKind::Messaging) {
				continue;
			}

			const auto found = bound->Subscriptions.find(std::string(delivery.Key.Text()));
			if (found == bound->Subscriptions.end()) {
				continue;
			}

			JSValue arguments[2];
			arguments[0] = JS_NewStringLen(
				context, reinterpret_cast<const char *>(delivery.Payload.data()), delivery.Payload.size()
			);
			arguments[1] = JS_NewString(context, delivery.Key.Text().data());

			for (JSValue &callback : found->second) {
				JSValue result = JS_Call(context, callback, JS_UNDEFINED, 2, arguments);
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
