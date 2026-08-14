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

#include <engine/ecs/Schema.hpp>

#include <algorithm>
#include <new>
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
		using ecs::FieldSpec;
		using ecs::PropertyType;
		using ecs::Schema;
		using ecs::Schemas;
		using ecs::Store;

		Store &StoreOf(JSContext *context) {
			return *JsOf(context).World;
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
		const Schema *FindSchema(JSContext *context, const char *name, ComponentId &out, JSValue &error) {
			const ComponentId id = Components::Find(Name(name));
			if (!id.IsValid()) {
				error = JS_ThrowReferenceError(
					context, "no component named '%s' - declare it with World.DefineComponent first", name
				);
				return nullptr;
			}

			const Schema *schema = Schemas::Of(id);
			if (schema == nullptr) {
				error = JS_ThrowTypeError(
					context,
					"'%s' is a component the engine declares, so it has no script-readable fields. "
					"Reach it through the properties of the instance that carries it",
					name
				);
				return nullptr;
			}

			out = id;
			return schema;
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
			case Schemas::Status::Sealed:
				return "components must be declared before the world starts ticking";
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

		// --- World -------------------------------------------------------------

		// `World.DefineComponent(name, { Field: "type" })`
		JSValue WorldDefineComponent(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
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
					continue;
				}

				PropertyType type = PropertyType::Opaque;
				if (!Schemas::TypeNamed(text, type)) {
					return JS_ThrowTypeError(context, "'%s' is not a field type", spelling.Get());
				}
				types.push_back(type);
				enums.emplace_back();
			}

			std::vector<FieldSpec> fields;
			fields.reserve(names.size());
			for (size_t at = 0; at < names.size(); at++) {
				fields.push_back(FieldSpec{names[at], types[at], enums[at]});
			}

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
				const std::string spelling = field.Type == PropertyType::Enum
												 ? "Enum." + std::string(field.Enum.Text())
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

		// `World.CreateEntity(name?)`
		JSValue WorldCreateEntity(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);

			Entity entity = ecs::NULL_ENTITY;
			if (count >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
				const Argument name(context, argv[0]);
				if (!name.Present()) {
					return JS_EXCEPTION;
				}
				entity = store.Create(name.Get());
			} else {
				entity = store.Create();
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
			store.EachMatching(terms, [&found](Entity entity) { found.push_back(entity); });

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
			return JS_NewInt64(context, static_cast<int64_t>(store.CountMatching(terms)));
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

			ComponentId id;
			JSValue error = JS_UNDEFINED;
			const Schema *schema = FindSchema(context, name.Get(), id, error);
			if (schema == nullptr) {
				return error;
			}

			const bool tag = schema->Fields().empty();
			if (!tag && (count < 2 || !JS_IsObject(argv[1]))) {
				return JS_ThrowTypeError(context, "SetComponent takes an object of field values");
			}

			ComponentValue value(*schema, Components::Describe(id));
			value.CopyFrom(store.GetComponent(entity, id));

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

					// The string case is separate for the reason it is separate
					// everywhere else: the destination is a live `std::string`
					// and the shared marshaller writes into raw bytes.
					bool ok = true;
					if (field->Type == PropertyType::String) {
						const Argument text(context, held);
						ok = text.Present();
						if (ok) {
							*static_cast<std::string *>(value.At(*field)) = text.Get();
						}
					} else {
						ok = FromJsValue(context, held, field->Type, field->Enum, value.At(*field));
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
				}
			}

			store.SetComponent(entity, id, value.Raw());
			return JS_UNDEFINED;
		}

		// `entity.GetComponent(name)`
		JSValue InstanceGetComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity = JsEntityOf(context, self);

			if (count < 1) {
				return JS_ThrowTypeError(context, "GetComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			ComponentId id;
			JSValue error = JS_UNDEFINED;
			const Schema *schema = FindSchema(context, name.Get(), id, error);
			if (schema == nullptr) {
				return error;
			}

			const void *held = store.GetComponent(entity, id);
			if (held == nullptr && !store.HasComponent(entity, id)) {
				// **Null rather than an empty object**, so "not carried" and
				// "carried and every field is zero" are different answers.
				return JS_NULL;
			}

			JSValue table = JS_NewObject(context);
			for (const FieldDescriptor &field : schema->Fields()) {
				const void *bytes = static_cast<const std::byte *>(held) + field.Offset;

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

		// `entity.HasComponent(name)`
		JSValue InstanceHasComponent(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			Store &store = StoreOf(context);
			const Entity entity = JsEntityOf(context, self);

			if (count < 1) {
				return JS_ThrowTypeError(context, "HasComponent takes a component name");
			}

			const Argument name(context, argv[0]);
			if (!name.Present()) {
				return JS_EXCEPTION;
			}

			// Not `FindSchema`, because asking is not reaching: the entity
			// either carries it or does not, whoever declared it.
			const ComponentId id = Components::Find(Name(name.Get()));
			return JS_NewBool(context, id.IsValid() && store.HasComponent(entity, id));
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

			ComponentId id;
			JSValue error = JS_UNDEFINED;
			if (FindSchema(context, name.Get(), id, error) == nullptr) {
				return error;
			}

			store.RemoveComponent(entity, id);
			return JS_UNDEFINED;
		}

		// `entity.GetComponents()`
		JSValue InstanceGetComponents(JSContext *context, JSValueConst self, int, JSValueConst *) {
			Store &store = StoreOf(context);
			const Entity entity = JsEntityOf(context, self);

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

	void InstallJsEcs(JSContext *context, JSValueConst global) {
		static const JSCFunctionListEntry WORLD[] = {
			JS_CFUNC_DEF("DefineComponent", 2, WorldDefineComponent),
			JS_CFUNC_DEF("HasComponentType", 1, WorldHasComponentType),
			JS_CFUNC_DEF("GetComponentSchema", 1, WorldGetComponentSchema),
			JS_CFUNC_DEF("CreateEntity", 1, WorldCreateEntity),
			JS_CFUNC_DEF("Query", 1, WorldQuery),
			JS_CFUNC_DEF("Count", 1, WorldCount),
		};

		JSValue world = JS_NewObject(context);
		JS_SetPropertyFunctionList(context, world, WORLD, static_cast<int>(std::size(WORLD)));
		JS_SetPropertyStr(context, global, "World", world);

		// **Onto the object `InstallJsInstanceMethods` built**, rather than a
		// list of its own: the thing a component attaches to is an entity, and
		// every instance object already is one. A separate `Entity` type would
		// have been a second handle onto the same sixty-four bits.
		static const JSCFunctionListEntry METHODS[] = {
			JS_CFUNC_DEF("SetComponent", 2, InstanceSetComponent),
			JS_CFUNC_DEF("GetComponent", 1, InstanceGetComponent),
			JS_CFUNC_DEF("HasComponent", 1, InstanceHasComponent),
			JS_CFUNC_DEF("RemoveComponent", 1, InstanceRemoveComponent),
			JS_CFUNC_DEF("GetComponents", 0, InstanceGetComponents),
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
