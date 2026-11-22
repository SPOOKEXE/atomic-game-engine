// The storage, as a JavaScript script sees it.
//
// **The JavaScript twin of `LuauEcs.cpp`, and deliberately the same surface.**
// That file carries the whole argument for what this is and what it refuses;
// what is written here is only what the language changes:
//
// - **`World.Query(...)` rather than `World:Query(...)`.** No colon call, so
//   the world object is not passed as a first argument and the terms start at
//   `argv[0]`.
// - **A plain object rather than a table** for a component's fields, which is
//   the same thing under a different spelling - `JS_GetOwnPropertyNames` walks
//   it where `lua_next` walks the other.
// - **An array for a query result**, which is what both return; a JavaScript
//   array is an object with a length and the Luau one is a sequence, and
//   neither is an iterator for the reason `LuauEcs.cpp` gives.
//
// The important property is that neither runtime has storage of its own. A
// component declared in one is queried by the other and iterated by a C++
// system that never heard of either - `engine.script.ecs` has the case.

#include "JsBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/script/EcsInstanceMethods.hpp>
#include <engine/script/InstanceShim.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::ComponentId;
		using ecs::Components;
		using ecs::Entity;
		using ecs::FieldDescriptor;
		using ecs::FieldPacking;
		using ecs::FieldSpec;
		using ecs::PropertyType;
		using ecs::QueryTerms;
		using ecs::Schema;
		using ecs::Schemas;
		using ecs::Store;

		Store &StoreOf(JSContext *context) {
			return *JsOf(context).World;
		}

		bool ClientExecution(JSContext *context) {
			const auto &role = JsExecutionRole(context);
			return StoreOf(context).AdoptOnly() || (role.Client && !role.Server);
		}

		bool MayWriteComponent(JSContext *context, Entity entity, ComponentId component) {
			const auto &store = StoreOf(context);
			if (!ClientExecution(context)) return !ecs::IsClientLocalInstance(store, entity);
			return store.Alive(entity) && Store::IsPredicted(entity) &&
				   ecs::IsClientLocalInstance(store, entity) &&
				   component != Components::Of<ecs::Hierarchy>() &&
				   component != Components::Of<ecs::InstanceClass>() &&
				   component != Components::Of<ecs::ClientLocal>();
		}

		// One component value, constructed and destroyed properly.
		//
		// The same holder `LuauEcs.cpp` carries and for the same two reasons: a
		// described component's width is a run-time fact, and a schema holding a
		// `String` field owns an allocation that a memcpy would leak.
		//
		// **Not shared between the two files**, because the two bindings share
		// rules rather than code - and a holder is four lines of RAII around a
		// `TypeDescriptor` either side of a VM boundary neither may name.
		class ComponentValue {
		  public:
			ComponentValue(const Schema &schema, const ecs::TypeDescriptor &descriptor)
				: Layout(schema), Type(descriptor) {
				if (Layout.Size() == 0) {
					return;
				}
				Bytes = ::operator new(Layout.Size(), std::align_val_t(Layout.Alignment()));
				Type.DefaultConstruct(Bytes, 1);
			}

			~ComponentValue() {
				if (Bytes == nullptr) {
					return;
				}
				Type.Destruct(Bytes, 1);
				::operator delete(Bytes, std::align_val_t(Layout.Alignment()));
			}

			ComponentValue(const ComponentValue &) = delete;
			ComponentValue &operator=(const ComponentValue &) = delete;
			ComponentValue(ComponentValue &&) = delete;
			ComponentValue &operator=(ComponentValue &&) = delete;

			void CopyFrom(const void *source) {
				if (Bytes == nullptr || source == nullptr) {
					return;
				}
				Type.Destruct(Bytes, 1);
				Type.CopyConstruct(Bytes, source, 1);
			}

			void *At(const FieldDescriptor &field) {
				return static_cast<std::byte *>(Bytes) + field.Offset;
			}

			void *RawMutable() {
				return Bytes;
			}

			const void *Raw() const {
				return Bytes;
			}

		  private:
			const Schema &Layout;
			const ecs::TypeDescriptor &Type;
			void *Bytes = nullptr;
		};

		// A JavaScript string argument, owned for the length of a call.
		//
		// QuickJS hands back a pointer the context owns and every path has to
		// give it back - including the ones that throw. A holder is what stops
		// an early `return JS_Throw...` from leaking the string it was
		// complaining about.
		class Argument {
		  public:
			Argument(JSContext *context, JSValueConst value) : Context(context) {
				Text = JS_ToCString(context, value);
			}

			~Argument() {
				if (Text != nullptr) {
					JS_FreeCString(Context, Text);
				}
			}

			Argument(const Argument &) = delete;
			Argument &operator=(const Argument &) = delete;
			Argument(Argument &&) = delete;
			Argument &operator=(Argument &&) = delete;

			bool Present() const {
				return Text != nullptr;
			}

			const char *Get() const {
				return Text;
			}

		  private:
			JSContext *Context = nullptr;
			const char *Text = nullptr;
		};

		// The described component behind a name, or null having thrown.
		//
		// **A component the engine declared and a component nobody declared are
		// different mistakes**, exactly as they are on the Luau side.
		//
		// @return `false` having thrown for a name nothing declared. `schema` is
		//         the script component's schema, or null for an engine one.
		bool FindComponent(JSContext *context, const char *name, ComponentId &out, const Schema *&schema) {
			const ComponentId id = Components::Find(Name(name));
			if (!id.IsValid()) {
				JS_ThrowReferenceError(
					context, "no component named '%s' - declare it with World.DefineComponent first", name
				);
				return false;
			}
			out = id;
			schema = Schemas::Of(id);
			return true;
		}

		// False, having thrown, for an engine component a script may not add or
		// remove.
		bool ScriptWritableEngine(JSContext *context, ComponentId id, const char *name) {
			if (EngineComponentScriptWritable(id)) return true;
			JS_ThrowTypeError(
				context, "'%s' is part of what makes an entity an instance, so scripts cannot write it", name
			);
			return false;
		}

		// One engine property of `entity` as a JavaScript value, or an exception.
		JSValue EngineProperty(
			JSContext *context,
			const Store &store,
			Entity entity,
			const ecs::PropertyDescriptor &property,
			bool client
		) {
			const char *field = property.Spelling.data();
			if (property.Type == PropertyType::String) {
				std::string text;
				if (!ReadInstanceProperty(store, entity, property, &text, sizeof(text), client)) {
					return JS_ThrowTypeError(context, "could not read '%s'", field);
				}
				return JS_NewStringLen(context, text.data(), text.size());
			}
			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property.Size > sizeof(bytes) ||
				!ReadInstanceProperty(store, entity, property, bytes, property.Size, client)) {
				return JS_ThrowTypeError(context, "could not read '%s'", field);
			}
			return ToJsValue(context, property.Type, property.EnumName, bytes);
		}

		// Writes `value` into one engine property. False having thrown.
		bool WriteEngineProperty(
			JSContext *context,
			Store &store,
			Entity entity,
			const ecs::PropertyDescriptor &property,
			JSValueConst value,
			bool client
		) {
			const char *field = property.Spelling.data();
			if (!property.Writable) {
				JS_ThrowTypeError(context, "'%s' is read-only", field);
				return false;
			}
			bool written = false;
			if (property.Type == PropertyType::String) {
				size_t length = 0;
				const char *text = JS_ToCStringLen(context, &length, value);
				if (text == nullptr) return false;
				const std::string held(text, length);
				JS_FreeCString(context, text);
				written = WriteInstanceProperty(store, entity, property, &held, sizeof(held), client);
			} else {
				alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
				if (property.Size > sizeof(bytes) ||
					!FromJsValue(context, value, property.Type, property.EnumName, bytes)) {
					JS_ThrowTypeError(context, "'%s' cannot take that value", field);
					return false;
				}
				written = WriteInstanceProperty(store, entity, property, bytes, property.Size, client);
			}
			if (!written) {
				JS_ThrowTypeError(context, "could not set '%s'", field);
				return false;
			}
			return true;
		}

		// `entity`'s value of a component as an object, null when the entity
		// does not carry it, or an exception. `GetComponent` and `ForEach` both
		// answer this.
		//
		// **Null rather than an empty object**, so "not carried" and "carried
		// and every field is zero" are different answers.
		JSValue ComponentObject(
			JSContext *context,
			const Store &store,
			Entity entity,
			ComponentId id,
			const Schema *schema,
			bool client
		) {
			if (entity == ecs::NULL_ENTITY || !store.HasComponent(entity, id)) return JS_NULL;

			JSValue table = JS_NewObject(context);
			if (schema == nullptr) {
				for (const ecs::PropertyDescriptor *property : EngineComponentProperties(store, entity, id)) {
					JSValue value = EngineProperty(context, store, entity, *property, client);
					if (JS_IsException(value)) {
						JS_FreeValue(context, table);
						return value;
					}
					JS_SetPropertyStr(context, table, std::string(property->Spelling).c_str(), value);
				}
				return table;
			}

			const void *held = store.GetComponent(entity, id);
			for (const FieldDescriptor &field : schema->Fields()) {
				alignas(8) std::array<std::byte, 8> scratch{};
				const void *bytes = Schemas::ReadField(held, field, scratch.data());

				JSValue value;
				if (field.Type == PropertyType::String) {
					const auto &text = *static_cast<const std::string *>(bytes);
					value = JS_NewStringLen(context, text.data(), text.size());
				} else {
					value = ToJsValue(context, field.Type, field.Enum, bytes);
				}

				if (JS_IsException(value)) {
					JS_FreeValue(context, table);
					return value;
				}
				JS_SetPropertyStr(context, table, std::string(field.Spelling).c_str(), value);
			}
			return table;
		}

		const char *Explain(Schemas::Status status) {
			switch (status) {
			case Schemas::Status::Ok:
				return "";
			case Schemas::Status::BadField:
				return "a field named a type the storage cannot hold";
			case Schemas::Status::DuplicateField:
				return "two fields share a name";
			case Schemas::Status::Conflict:
				return "that name is already a component with different fields";
			case Schemas::Status::Unnamed:
				return "a component needs a name";
			case Schemas::Status::Exhausted:
				return "this process has described as many components as it can hold";
			}
			return "the declaration was refused";
		}

		// Every own enumerable key of an object, as strings.
		//
		// **Freed through a holder for the same reason `Argument` exists**: the
		// atom table is reference counted and a throw in the middle of a walk
		// would otherwise strand every atom the walk had taken.
		class Keys {
		  public:
			Keys(JSContext *context, JSValueConst object) : Context(context) {
				Ok = JS_GetOwnPropertyNames(
						 context, &Table, &Count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
					 ) == 0;
			}

			~Keys() {
				if (Table == nullptr) {
					return;
				}
				for (uint32_t at = 0; at < Count; at++) {
					JS_FreeAtom(Context, Table[at].atom);
				}
				js_free(Context, Table);
			}

			Keys(const Keys &) = delete;
			Keys &operator=(const Keys &) = delete;
			Keys(Keys &&) = delete;
			Keys &operator=(Keys &&) = delete;

			bool Present() const {
				return Ok;
			}

			uint32_t Size() const {
				return Count;
			}

			JSAtom At(uint32_t index) const {
				return Table[index].atom;
			}

		  private:
			JSContext *Context = nullptr;
			JSPropertyEnum *Table = nullptr;
			uint32_t Count = 0;
			bool Ok = false;
		};

		bool ReadStringArray(
			JSContext *context, JSValueConst value, const char *label, std::vector<std::string> &strings
		) {
			if (!JS_IsArray(value)) {
				JS_ThrowTypeError(context, "%s has to be an array of strings", label);
				return false;
			}

			JSValue held = JS_GetPropertyStr(context, value, "length");
			uint32_t length = 0;
			const int converted = JS_ToUint32(context, &length, held);
			JS_FreeValue(context, held);
			if (converted < 0) {
				return false;
			}

			strings.clear();
			strings.reserve(length);
			for (uint32_t at = 0; at < length; at++) {
				held = JS_GetPropertyUint32(context, value, at);
				if (!JS_IsString(held)) {
					JS_FreeValue(context, held);
					JS_ThrowTypeError(context, "%s has to contain only strings", label);
					return false;
				}

				const Argument string(context, held);
				JS_FreeValue(context, held);
				if (!string.Present()) {
					return false;
				}
				strings.emplace_back(string.Get());
			}
			return true;
		}

		std::vector<std::string_view> ViewsOf(const std::vector<std::string> &strings) {
			std::vector<std::string_view> views;
			views.reserve(strings.size());
			for (const std::string &string : strings) {
				views.emplace_back(string);
			}
			return views;
		}

		bool ReadComponentArray(
			JSContext *context, JSValueConst value, const char *label, std::vector<ComponentId> &components
		) {
			std::vector<std::string> names;
			if (!ReadStringArray(context, value, label, names)) {
				return false;
			}

			components.clear();
			components.reserve(names.size());
			for (const std::string &name : names) {
				const ComponentId component = Components::Find(Name(name));
				if (!component.IsValid()) {
					JS_ThrowReferenceError(
						context, "no component named '%s' in %s filter", name.c_str(), label
					);
					return false;
				}
				components.push_back(component);
			}
			return true;
		}

		JSValue StringArray(JSContext *context, const std::vector<std::string> &strings) {
			JSValue array = JS_NewArray(context);
			for (size_t at = 0; at < strings.size(); at++) {
				JS_SetPropertyUint32(
					context,
					array,
					static_cast<uint32_t>(at),
					JS_NewStringLen(context, strings[at].data(), strings[at].size())
				);
			}
			return array;
		}

		// --- World -------------------------------------------------------------

		// `World.DefineComponent(name, { Field: "type" })`
		JSValue WorldDefineComponent(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (!JsWorldWritable(context, "World.DefineComponent")) return JS_EXCEPTION;
			if (count < 2 || !JS_IsObject(argv[1])) {
				return JS_ThrowTypeError(context, "DefineComponent takes a name and a table of fields");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			const Keys keys(context, argv[1]);
			if (!keys.Present()) {
				return JS_EXCEPTION;
			}

			// Owned here, because `FieldSpec` holds views and everything the VM
			// hands back is freed before `Register` runs.
			std::vector<std::string> names;
			std::vector<std::string> enums;
			std::vector<PropertyType> types;
			std::vector<FieldPacking> packings;

			constexpr std::string_view ENUM_PREFIX = "Enum.";

			for (uint32_t at = 0; at < keys.Size(); at++) {
				const char *key = JS_AtomToCString(context, keys.At(at));
				if (key == nullptr) {
					return JS_EXCEPTION;
				}
				names.emplace_back(key);
				JS_FreeCString(context, key);

				const JSValue value = JS_GetProperty(context, argv[1], keys.At(at));
				const Argument spelling(context, value);
				JS_FreeValue(context, value);

				if (!spelling.Present()) {
					return JS_ThrowTypeError(
						context, "the type of field '%s' has to be a string", names.back().c_str()
					);
				}

				const std::string_view text(spelling.Get());
				if (text.rfind(ENUM_PREFIX, 0) == 0) {
					const std::string_view set = text.substr(ENUM_PREFIX.size());
					if (set.empty()) {
						return JS_ThrowTypeError(
							context, "'Enum.' needs the name of a registered enum after it"
						);
					}
					types.push_back(PropertyType::Enum);
					enums.emplace_back(set);
					packings.push_back(FieldPacking::Native);
					continue;
				}

				PropertyType type = PropertyType::Opaque;
				FieldPacking packing = FieldPacking::Native;
				if (!Schemas::FieldTypeNamed(text, type, packing)) {
					return JS_ThrowTypeError(context, "'%s' is not a field type", spelling.Get());
				}
				types.push_back(type);
				enums.emplace_back();
				packings.push_back(packing);
			}

			std::vector<FieldSpec> fields;
			fields.reserve(names.size());
			for (size_t at = 0; at < names.size(); at++) {
				fields.push_back(FieldSpec{names[at], types[at], enums[at], packings[at]});
			}

			// A receiver can confirm an existing declaration without publishing a new global type.
			if (ClientExecution(context) && Schemas::Find(Name(name.Get())) == nullptr)
				return JS_ThrowTypeError(context, "the authority declares component types");
			const Schemas::Result result = Schemas::Register(name.Get(), fields);
			if (result.Why != Schemas::Status::Ok) {
				return JS_ThrowTypeError(
					context, "cannot declare component '%s': %s", name.Get(), Explain(result.Why)
				);
			}
			return JS_NewBool(context, result.Created);
		}

		// `World.HasComponentType(name)`
		JSValue WorldHasComponentType(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (count < 1) {
				return JS_ThrowTypeError(context, "HasComponentType takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}
			return JS_NewBool(context, Schemas::Find(Name(name.Get())) != nullptr);
		}

		// `World.GetComponentSchema(name)`
		JSValue WorldGetComponentSchema(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (count < 1) {
				return JS_ThrowTypeError(context, "GetComponentSchema takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			const Schema *schema = Schemas::Find(Name(name.Get()));
			if (schema == nullptr) {
				return JS_NULL;
			}

			JSValue table = JS_NewObject(context);
			for (const FieldDescriptor &field : schema->Fields()) {
				const std::string spelling =
					field.Packing != FieldPacking::Native ? ecs::Describe(field.Packing)
					: field.Type == PropertyType::Enum	  ? "Enum." + std::string(field.Enum.Text())
														  : std::string(ecs::Describe(field.Type));
				JS_SetPropertyStr(
					context,
					table,
					std::string(field.Spelling).c_str(),
					JS_NewString(context, spelling.c_str())
				);
			}
			return table;
		}

		JSValue WorldSetComponentTags(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (!JsWorldWritable(context, "component metadata")) return JS_EXCEPTION;
			if (ClientExecution(context))
				return JS_ThrowTypeError(context, "the authority owns component metadata");
			if (count < 2) {
				return JS_ThrowTypeError(context, "SetComponentTags takes a component name and tags");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}
			ComponentId component;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), component, schema)) {
				return JS_EXCEPTION;
			}
			if (schema == nullptr) {
				return JS_ThrowTypeError(context, "'%s' is not a script component", name.Get());
			}

			std::vector<std::string> tags;
			if (!ReadStringArray(context, argv[1], "component tags", tags)) {
				return JS_EXCEPTION;
			}
			return JS_NewBool(context, Schemas::SetTags(component, ViewsOf(tags)));
		}

		JSValue WorldSetComponentFieldTags(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (!JsWorldWritable(context, "component metadata")) return JS_EXCEPTION;
			if (ClientExecution(context))
				return JS_ThrowTypeError(context, "the authority owns component metadata");
			if (count < 3) {
				return JS_ThrowTypeError(
					context, "SetComponentFieldTags takes a component name, field, and tags"
				);
			}

			const Argument name(context, argv[0]);
			const Argument field(context, argv[1]);
			if (!name.Present() || !field.Present()) {
				return JS_EXCEPTION;
			}
			ComponentId component;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), component, schema)) {
				return JS_EXCEPTION;
			}
			if (schema == nullptr) {
				return JS_ThrowTypeError(context, "'%s' is not a script component", name.Get());
			}

			std::vector<std::string> tags;
			if (!ReadStringArray(context, argv[2], "component field tags", tags)) {
				return JS_EXCEPTION;
			}
			return JS_NewBool(context, Schemas::SetFieldTags(component, Name(field.Get()), ViewsOf(tags)));
		}

		JSValue WorldExposeComponentField(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (!JsWorldWritable(context, "component metadata")) return JS_EXCEPTION;
			if (ClientExecution(context))
				return JS_ThrowTypeError(context, "the authority owns component metadata");
			if (count < 3) {
				return JS_ThrowTypeError(
					context, "ExposeComponentField takes a component name, field, and boolean"
				);
			}

			const Argument name(context, argv[0]);
			const Argument field(context, argv[1]);
			if (!name.Present() || !field.Present()) {
				return JS_EXCEPTION;
			}
			ComponentId component;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), component, schema)) {
				return JS_EXCEPTION;
			}
			if (schema == nullptr) {
				return JS_ThrowTypeError(context, "'%s' is not a script component", name.Get());
			}
			return JS_NewBool(
				context,
				Schemas::SetFieldExposed(component, Name(field.Get()), JS_ToBool(context, argv[2]) != 0)
			);
		}

		JSValue WorldGetComponentMetadata(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (count < 1) {
				return JS_ThrowTypeError(context, "GetComponentMetadata takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}
			const Schema *schema = Schemas::Find(Name(name.Get()));
			if (schema == nullptr) {
				return JS_NULL;
			}

			JSValue metadata = JS_NewObject(context);
			JS_SetPropertyStr(context, metadata, "Tags", StringArray(context, schema->Tags()));

			JSValue fields = JS_NewObject(context);
			for (const FieldDescriptor &field : schema->Fields()) {
				JSValue described = JS_NewObject(context);
				const std::string type = field.Type == PropertyType::Enum
											 ? "Enum." + std::string(field.Enum.Text())
											 : std::string(ecs::Describe(field.Type));
				JS_SetPropertyStr(context, described, "Type", JS_NewString(context, type.c_str()));
				JS_SetPropertyStr(
					context, described, "Packing", JS_NewString(context, ecs::Describe(field.Packing))
				);
				JS_SetPropertyStr(context, described, "Tags", StringArray(context, field.Tags));
				JS_SetPropertyStr(context, described, "Exposed", JS_NewBool(context, field.Exposed));
				JS_SetPropertyStr(context, fields, std::string(field.Spelling).c_str(), described);
			}
			JS_SetPropertyStr(context, metadata, "Fields", fields);
			return metadata;
		}

		// `World.CreateEntity(name?)`
		JSValue WorldCreateEntity(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (!JsWorldWritable(context, "World.CreateEntity")) return JS_EXCEPTION;
			Store &store = StoreOf(context);
			const bool client = ClientExecution(context);

			Entity entity = ecs::NULL_ENTITY;
			if (count >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
				const Argument name(context, argv[0]);
				if (!name.Present()) {
					return JS_EXCEPTION;
				}
				const Entity existing = store.Find(name.Get());
				if (!client && store.Alive(existing) && ecs::IsClientLocalInstance(store, existing))
					return JS_ThrowTypeError(context, "this entity name belongs to client execution");
				if (client && store.Alive(existing) &&
					(!Store::IsPredicted(existing) || !ecs::IsClientLocalInstance(store, existing)))
					return JS_ThrowTypeError(context, "this entity name is not owned by this client");
				entity = client ? store.CreatePredicted(name.Get()) : store.Create(name.Get());
			} else {
				entity = client ? store.CreatePredicted() : store.Create();
			}

			if (entity == ecs::NULL_ENTITY) {
				if (store.AdoptOnly()) {
					return JS_ThrowTypeError(
						context,
						"this world is a replica and the authority owns its entities. "
						"Test RunService.IsServer() first"
					);
				}
				return JS_ThrowInternalError(context, "the world could not create an entity");
			}

			if (client) {
				if (!Store::IsPredicted(entity))
					return JS_ThrowTypeError(context, "this entity name belongs to the authority");
				store.Set(entity, ecs::ClientLocal{});
			}
			return MakeJsInstance(context, entity);
		}

		// The component ids a query's arguments name, or false having thrown.
		bool ReadTerms(JSContext *context, int count, JSValueConst *argv, std::vector<ComponentId> &terms) {
			if (count < 1) {
				JS_ThrowTypeError(context, "a query has to name at least one component");
				return false;
			}

			terms.reserve(static_cast<size_t>(count));
			for (int at = 0; at < count; at++) {
				const Argument name(context, argv[at]);
				if (!name.Present()) {
					return false;
				}

				const ComponentId id = Components::Find(Name(name.Get()));
				if (!id.IsValid()) {
					JS_ThrowReferenceError(context, "no component named '%s'", name.Get());
					return false;
				}
				terms.push_back(id);
			}
			return true;
		}

		// `World.Query(...componentNames)`
		JSValue WorldQuery(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);

			std::vector<ComponentId> terms;
			if (!ReadTerms(context, count, argv, terms)) {
				return JS_EXCEPTION;
			}

			// Collected before any of it reaches the VM. `EachMatching` defers
			// structural changes for the length of the call, and building the
			// array inside it would let a getter on `Array.prototype` run with
			// that scope open.
			std::vector<Entity> found;
			const bool client = ClientExecution(context);
			store.EachMatching(terms, [&](Entity entity) {
				if (InstanceVisibleToScript(store, entity, client)) found.push_back(entity);
			});

			JSValue array = JS_NewArray(context);
			for (size_t at = 0; at < found.size(); at++) {
				JS_SetPropertyUint32(
					context, array, static_cast<uint32_t>(at), MakeJsInstance(context, found[at])
				);
			}
			return array;
		}

		JSValue WorldQueryFiltered(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			if (count < 2) {
				return JS_ThrowTypeError(context, "QueryFiltered takes include and exclude arrays");
			}

			std::vector<ComponentId> required;
			std::vector<ComponentId> excluded;
			if (!ReadComponentArray(context, argv[0], "include", required) ||
				!ReadComponentArray(context, argv[1], "exclude", excluded)) {
				return JS_EXCEPTION;
			}

			std::vector<Entity> found;
			Store &store = StoreOf(context);
			const bool client = ClientExecution(context);
			store.EachMatching(
				QueryTerms{
					{}, std::span<const ComponentId>(required), std::span<const ComponentId>(excluded)
				},
				[&](Entity entity) {
					if (InstanceVisibleToScript(store, entity, client)) found.push_back(entity);
				}
			);

			JSValue array = JS_NewArray(context);
			for (size_t at = 0; at < found.size(); at++) {
				JS_SetPropertyUint32(
					context, array, static_cast<uint32_t>(at), MakeJsInstance(context, found[at])
				);
			}
			return array;
		}

		// `World.Count(...componentNames)`
		JSValue WorldCount(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);

			std::vector<ComponentId> terms;
			if (!ReadTerms(context, count, argv, terms)) {
				return JS_EXCEPTION;
			}
			const bool client = ClientExecution(context);
			int64_t visible = 0;
			store.EachMatching(terms, [&](Entity entity) {
				if (InstanceVisibleToScript(store, entity, client)) ++visible;
			});
			return JS_NewInt64(context, visible);
		}

		// Holds a read-only pass open for the length of one scope.
		class ReadOnlyScope {
		  public:
			explicit ReadOnlyScope(JsContext &context) : Context(context) {
				Context.ReadOnlyDepth++;
			}
			~ReadOnlyScope() {
				Context.ReadOnlyDepth--;
			}
			ReadOnlyScope(const ReadOnlyScope &) = delete;
			ReadOnlyScope &operator=(const ReadOnlyScope &) = delete;

		  private:
			JsContext &Context;
		};

		// The body of `ForEach` and `ForEachParallel`. The JavaScript twin of
		// `LuauEcs.cpp`'s `ForEachMatch`: matches are collected first, each is
		// checked again before its call, and each value is what `GetComponent`
		// answers.
		JSValue ForEachMatch(JSContext *context, int count, JSValueConst *argv, bool readOnly) {
			if (count < 2 || !JS_IsFunction(context, argv[count - 1])) {
				return JS_ThrowTypeError(context, "ForEach takes component names and then a function");
			}
			JSValueConst body = argv[count - 1];

			std::vector<ComponentId> terms;
			std::vector<const Schema *> schemas;
			for (int at = 0; at < count - 1; at++) {
				const Argument name(context, argv[at]);
				if (!name.Present()) return JS_EXCEPTION;
				ComponentId id;
				const Schema *schema = nullptr;
				if (!FindComponent(context, name.Get(), id, schema)) return JS_EXCEPTION;
				terms.push_back(id);
				schemas.push_back(schema);
			}

			Store &store = StoreOf(context);
			const bool client = ClientExecution(context);
			std::vector<Entity> found;
			store.EachMatching(terms, [&](Entity entity) {
				if (InstanceVisibleToScript(store, entity, client)) found.push_back(entity);
			});

			std::optional<ReadOnlyScope> scope;
			if (readOnly) scope.emplace(JsOf(context));

			std::vector<JSValue> arguments(terms.size() + 1, JS_UNDEFINED);
			const auto release = [&]() {
				for (JSValue &argument : arguments) {
					JS_FreeValue(context, argument);
					argument = JS_UNDEFINED;
				}
			};
			for (const Entity entity : found) {
				bool matches = store.Alive(entity);
				for (const ComponentId id : terms) {
					matches = matches && store.HasComponent(entity, id);
				}
				if (!matches) continue;

				arguments[0] = MakeJsInstance(context, entity);
				const Entity readable = InstanceForScriptRead(store, entity, client);
				for (size_t at = 0; at < terms.size(); at++) {
					arguments[at + 1] =
						ComponentObject(context, store, readable, terms[at], schemas[at], client);
					if (JS_IsException(arguments[at + 1])) {
						arguments[at + 1] = JS_UNDEFINED;
						release();
						return JS_EXCEPTION;
					}
				}
				JSValue result = JS_Call(
					context, body, JS_UNDEFINED, static_cast<int>(arguments.size()), arguments.data()
				);
				release();
				if (JS_IsException(result)) return result;
				JS_FreeValue(context, result);
			}
			return JS_UNDEFINED;
		}

		// `World.ForEach(name..., body)`
		JSValue WorldForEach(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			return ForEachMatch(context, count, argv, false);
		}

		// `World.ForEachParallel(name..., body)`, the read-only pass.
		JSValue WorldForEachParallel(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			return ForEachMatch(context, count, argv, true);
		}

		// --- the instance half -------------------------------------------------

		// `entity.SetComponent(name, values)`
		JSValue InstanceSetComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity = JsEntityOf(context, self);

			if (count < 1) {
				return JS_ThrowTypeError(context, "SetComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			if (!JsWorldWritable(context, "SetComponent")) return JS_EXCEPTION;
			ComponentId id;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), id, schema)) {
				return JS_EXCEPTION;
			}

			if (!MayWriteComponent(context, entity, id))
				return JS_ThrowTypeError(context, "component writes cannot cross script execution sides");

			// **An engine component is assigned through its properties.** Added
			// default-constructed first when missing, so the properties that
			// read it have something to write into.
			if (schema == nullptr) {
				if (!ScriptWritableEngine(context, id, name.Get())) return JS_EXCEPTION;
				const bool values = count >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
				if (values && !JS_IsObject(argv[1])) {
					return JS_ThrowTypeError(context, "SetComponent takes an object of field values");
				}
				if (!AddDefaultComponent(store, entity, id)) {
					return JS_ThrowTypeError(context, "could not add '%s'", name.Get());
				}
				if (!values) return JS_UNDEFINED;

				const bool client = ClientExecution(context);
				const auto properties = EngineComponentProperties(store, entity, id);
				const Keys keys(context, argv[1]);
				if (!keys.Present()) return JS_EXCEPTION;
				for (uint32_t at = 0; at < keys.Size(); at++) {
					const char *key = JS_AtomToCString(context, keys.At(at));
					if (key == nullptr) return JS_EXCEPTION;
					const ecs::PropertyDescriptor *match = nullptr;
					for (const ecs::PropertyDescriptor *property : properties) {
						if (property->Spelling == key) match = property;
					}
					if (match == nullptr) {
						JSValue missing =
							JS_ThrowTypeError(context, "'%s' has no field '%s'", name.Get(), key);
						JS_FreeCString(context, key);
						return missing;
					}
					JS_FreeCString(context, key);

					const JSValue held = JS_GetProperty(context, argv[1], keys.At(at));
					if (JS_IsException(held)) return JS_EXCEPTION;
					const bool written = WriteEngineProperty(context, store, entity, *match, held, client);
					JS_FreeValue(context, held);
					if (!written) return JS_EXCEPTION;
				}
				return JS_UNDEFINED;
			}

			const bool tag = schema->Fields().empty();
			if (!tag && (count < 2 || !JS_IsObject(argv[1]))) {
				return JS_ThrowTypeError(context, "SetComponent takes an object of field values");
			}

			// Property getters can run script and change the entity while the
			// input is decoded. Keep the supplied fields separate until then.
			ComponentValue decoded(*schema, Components::Describe(id));
			std::vector<const FieldDescriptor *> supplied;

			if (!tag) {
				const Keys keys(context, argv[1]);
				if (!keys.Present()) {
					return JS_EXCEPTION;
				}

				for (uint32_t at = 0; at < keys.Size(); at++) {
					const char *key = JS_AtomToCString(context, keys.At(at));
					if (key == nullptr) {
						return JS_EXCEPTION;
					}

					const FieldDescriptor *field = schema->Find(std::string_view(key));
					if (field == nullptr) {
						JSValue missing =
							JS_ThrowTypeError(context, "'%s' has no field '%s'", name.Get(), key);
						JS_FreeCString(context, key);
						return missing;
					}
					JS_FreeCString(context, key);

					const JSValue held = JS_GetProperty(context, argv[1], keys.At(at));
					if (JS_IsException(held)) {
						return JS_EXCEPTION;
					}

					// The string case is separate for the reason it is separate
					// everywhere else: the destination is a live `std::string`
					// and the shared marshaller writes into raw bytes.
					alignas(8) std::array<std::byte, 8> scratch{};
					void *target =
						field->Packing == FieldPacking::Native ? decoded.At(*field) : scratch.data();
					bool ok = true;
					if (field->Type == PropertyType::String) {
						const Argument text(context, held);
						ok = text.Present() &&
							 std::string_view(text.Get()).size() <= Schemas::MAXIMUM_STRING_BYTES;
						if (ok) {
							*static_cast<std::string *>(target) = text.Get();
						}
					} else {
						ok = FromJsValue(context, held, field->Type, field->Enum, target);
					}
					if (ok && field->Packing != FieldPacking::Native) {
						ok = Schemas::WriteField(decoded.RawMutable(), *field, target);
					}
					JS_FreeValue(context, held);

					if (!ok) {
						return JS_ThrowTypeError(
							context,
							"'%s.%s' cannot take that value",
							name.Get(),
							std::string(field->Spelling).c_str()
						);
					}
					supplied.push_back(field);
				}
			}

			if (!store.Alive(entity) || !MayWriteComponent(context, entity, id)) {
				return JS_ThrowTypeError(context, "component target is no longer writable");
			}
			ComponentValue value(*schema, Components::Describe(id));
			value.CopyFrom(store.GetComponent(entity, id));
			for (const FieldDescriptor *field : supplied) {
				alignas(8) std::array<std::byte, 8> scratch{};
				const void *source = Schemas::ReadField(decoded.Raw(), *field, scratch.data());
				if (!Schemas::WriteField(value.RawMutable(), *field, source)) {
					return JS_ThrowInternalError(context, "could not apply a component field");
				}
			}
			store.SetComponent(entity, id, value.Raw());
			return JS_UNDEFINED;
		}

		// `entity.GetComponent(name)`
		JSValue InstanceGetComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity =
				InstanceForScriptRead(store, JsEntityOf(context, self), ClientExecution(context));

			if (count < 1) {
				return JS_ThrowTypeError(context, "GetComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			ComponentId id;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), id, schema)) {
				return JS_EXCEPTION;
			}
			return ComponentObject(context, store, entity, id, schema, ClientExecution(context));
		}

		// `entity.HasComponent(name)`
		JSValue InstanceHasComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity =
				InstanceForScriptRead(store, JsEntityOf(context, self), ClientExecution(context));

			if (count < 1) {
				return JS_ThrowTypeError(context, "HasComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			// Not `FindComponent`, because asking is not reaching: a name nothing
			// declared is a "no" rather than an error.
			const ComponentId id = Components::Find(Name(name.Get()));
			return JS_NewBool(
				context, entity != ecs::NULL_ENTITY && id.IsValid() && store.HasComponent(entity, id)
			);
		}

		// `entity.RemoveComponent(name)`
		JSValue
		InstanceRemoveComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity = JsEntityOf(context, self);

			if (count < 1) {
				return JS_ThrowTypeError(context, "RemoveComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			if (!JsWorldWritable(context, "RemoveComponent")) return JS_EXCEPTION;
			ComponentId id;
			const Schema *schema = nullptr;
			if (!FindComponent(context, name.Get(), id, schema)) {
				return JS_EXCEPTION;
			}
			if (schema == nullptr && !ScriptWritableEngine(context, id, name.Get())) return JS_EXCEPTION;

			if (!MayWriteComponent(context, entity, id))
				return JS_ThrowTypeError(context, "component writes cannot cross script execution sides");
			store.RemoveComponent(entity, id);
			return JS_UNDEFINED;
		}

		// `entity.GetComponents()`
		JSValue InstanceGetComponents(JSContext *context, JSValueConst self, int, JSValueConst *) {
			Store &store = StoreOf(context);
			const Entity entity =
				InstanceForScriptRead(store, JsEntityOf(context, self), ClientExecution(context));
			if (entity == ecs::NULL_ENTITY) return JS_NewArray(context);

			std::vector<std::string_view> names;
			for (const ComponentId id : store.ComponentsOf(entity)) {
				names.push_back(Components::Describe(id).Name.Text());
			}
			std::sort(names.begin(), names.end());

			JSValue array = JS_NewArray(context);
			for (size_t at = 0; at < names.size(); at++) {
				JS_SetPropertyUint32(
					context,
					array,
					static_cast<uint32_t>(at),
					JS_NewStringLen(context, names[at].data(), names[at].size())
				);
			}
			return array;
		}
	}

	bool JsWorldWritable(JSContext *context, const char *what) {
		if (JsOf(context).ReadOnlyDepth == 0) return true;
		JS_ThrowTypeError(context, "%s writes the world, and World.ForEachParallel is read-only", what);
		return false;
	}

	void InstallJsEcs(JSContext *context, JSValueConst global) {
		static const JSCFunctionListEntry WORLD[] = {
			JS_CFUNC_DEF("DefineComponent", 2, WorldDefineComponent),
			JS_CFUNC_DEF("HasComponentType", 1, WorldHasComponentType),
			JS_CFUNC_DEF("GetComponentSchema", 1, WorldGetComponentSchema),
			JS_CFUNC_DEF("SetComponentTags", 2, WorldSetComponentTags),
			JS_CFUNC_DEF("SetComponentFieldTags", 3, WorldSetComponentFieldTags),
			JS_CFUNC_DEF("ExposeComponentField", 3, WorldExposeComponentField),
			JS_CFUNC_DEF("GetComponentMetadata", 1, WorldGetComponentMetadata),
			JS_CFUNC_DEF("CreateEntity", 1, WorldCreateEntity),
			JS_CFUNC_DEF("Query", 1, WorldQuery),
			JS_CFUNC_DEF("QueryFiltered", 2, WorldQueryFiltered),
			JS_CFUNC_DEF("Count", 1, WorldCount),
			JS_CFUNC_DEF("ForEach", 2, WorldForEach),
			JS_CFUNC_DEF("ForEachParallel", 2, WorldForEachParallel),
		};

		JSValue world = JS_NewObject(context);
		JS_SetPropertyFunctionList(context, world, WORLD, static_cast<int>(std::size(WORLD)));
		JS_SetPropertyStr(context, global, "World", world);

		// **Onto the object `InstallJsInstanceMethods` built**, rather than a
		// list of its own: the thing a component attaches to is an entity, and
		// every instance object already is one. A separate `Entity` type would
		// have been a second handle onto the same sixty-four bits.
		static const JSCFunctionListEntry METHODS[] = {
			JS_CFUNC_DEF(EcsInstanceMethodName(EcsInstanceMethod::SetComponent), 2, InstanceSetComponent),
			JS_CFUNC_DEF(EcsInstanceMethodName(EcsInstanceMethod::GetComponent), 1, InstanceGetComponent),
			JS_CFUNC_DEF(EcsInstanceMethodName(EcsInstanceMethod::HasComponent), 1, InstanceHasComponent),
			JS_CFUNC_DEF(
				EcsInstanceMethodName(EcsInstanceMethod::RemoveComponent), 1, InstanceRemoveComponent
			),
			JS_CFUNC_DEF(EcsInstanceMethodName(EcsInstanceMethod::GetComponents), 0, InstanceGetComponents),
		};

		const JSValue methods = JS_GetPropertyStr(context, global, "__instanceMethods");
		if (!JS_IsObject(methods)) {
			// Nothing to hang them on means the order in `OpenJsSurface` moved,
			// and a surface that silently lost five methods is exactly what
			// `InstallJsInstanceMethods`'s own `std::size` comment is about.
			ENGINE_ERROR("the instance method table is missing, so the ECS methods were not installed");
			JS_FreeValue(context, methods);
			return;
		}

		JS_SetPropertyFunctionList(context, methods, METHODS, static_cast<int>(std::size(METHODS)));
		JS_FreeValue(context, methods);
	}
}
