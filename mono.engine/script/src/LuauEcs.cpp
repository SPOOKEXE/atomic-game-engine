// The storage, as a script sees it.
//
// **Everything above this is the Roblox vocabulary and this is what is
// underneath it**, which `script/AGENTS.md` already states in one line:
//
//   > An instance is an entity. A class is a set of components. A property is a
//   > projection of one or more of them. Nothing else exists.
//
// `Instance.new("Part")` and `part.Size = v` are that model reached through a
// *class*, and a class is something the engine registered. A game has data the
// engine never heard of - a health, a cooldown, an inventory slot - and until
// v0.12 the only places to put it were an attribute, which is one loose value
// per instance with no query behind it, or a component in C++, which needs a
// rebuild.
//
// So this surface is the same storage named directly: declare a component,
// attach it to an entity, and ask the world who has one. **It adds no storage
// and no concept.** `World:Query` is `Store::EachMatching`, `SetComponent` is
// `Store::SetComponent`, and a component a script declared is iterated by a C++
// system through `Each` with no idea a script named it - which is the property
// that makes this worth having rather than a scripting-only side table.
//
// ## What a script may not reach, and why each refusal is the design
//
// - **A component the engine declared is not readable here.** `scene::Visual`
//   is a C++ struct with no field list at run time, so there is nothing to
//   marshal a table from - and it already has a property surface, which is the
//   supported way to touch it. Two ways to write one component is the debt the
//   root `AGENTS.md` calls the most expensive kind.
// - **Declaring a component is a registration, so it happens once.** Component
//   ids are a dense counter and archetypes iterate in id order, so a type first
//   seen mid-tick would take an id decided by whichever world ran first. The
//   refusal is `Schemas::Status::Sealed` and it is reported rather than fatal,
//   because the caller is an author who can fix a line.
// - **A query names components and gets entities.** It does not get the values:
//   a script reads those with `GetComponent`, because handing back a table per
//   component per row would allocate more than the query costs.

#include "LuauBindings.hpp"

#include <engine/ecs/Schema.hpp>

#include <algorithm>
#include <lualib.h>
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

		Store &StoreOf(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		Entity CheckEntity(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Instance");
			}
			return *static_cast<Entity *>(value);
		}

		// One component value, constructed and destroyed properly.
		//
		// **A holder rather than a stack buffer, and both halves of that matter.**
		// A described component's width is decided at run time, so there is no
		// array to size; and a schema holding a `String` field owns an
		// allocation, so the bytes have to be default-constructed before they
		// are written and destroyed afterwards. A `memset` and a `memcpy` would
		// leak one allocation per write and free a garbage pointer on the next.
		//
		// Luau raises by throwing a C++ exception - `luaD_throw` - so a field
		// conversion that rejects a script's value unwinds through this
		// destructor rather than leaking past it.
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

			// Overwrites this value with a copy of one the store holds.
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

		// --- resolving what a script named -----------------------------------

		// The described component behind a name, or an error saying which of the
		// two things went wrong.
		//
		// **A component the engine declared and a component nobody declared are
		// different mistakes**, and telling them apart is most of the value of
		// this function: "there is no such component" sends an author looking for
		// a typo, where the real answer is often "that one is reached through its
		// properties".
		const Schema &CheckSchema(lua_State *state, const char *name, ComponentId &out) {
			const ComponentId id = Components::Find(Name(name));
			if (!id.IsValid()) {
				luaL_errorL(
					state, "no component named '%s' - declare it with World:DefineComponent first", name
				);
			}

			const Schema *schema = Schemas::Of(id);
			if (schema == nullptr) {
				luaL_errorL(
					state,
					"'%s' is a component the engine declares, so it has no script-readable fields. "
					"Reach it through the properties of the instance that carries it",
					name
				);
			}

			out = id;
			return *schema;
		}

		// What a `Schemas::Status` means, in words an author can act on.
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

		// --- World:DefineComponent -------------------------------------------

		// The field list a script wrote, read out of a Luau table.
		//
		// **The strings are owned here**, because `FieldSpec` holds views and the
		// Luau stack values they would otherwise point into are popped by
		// `lua_next` before `Register` ever runs.
		struct Declaration {
			std::vector<std::string> Names;
			std::vector<std::string> Enums;
			std::vector<PropertyType> Types;
		};

		// `Kind = "Enum.BodyKind"` names an enum field; everything else is a
		// plain type spelling. One string per field rather than a nested table,
		// because a field declaration is one fact and `{ Type = ..., Enum = ... }`
		// would be two lines to say it.
		void ReadFieldType(lua_State *state, std::string_view spelling, Declaration &into) {
			constexpr std::string_view ENUM_PREFIX = "Enum.";

			if (spelling.rfind(ENUM_PREFIX, 0) == 0) {
				const std::string_view set = spelling.substr(ENUM_PREFIX.size());
				if (set.empty()) {
					luaL_errorL(state, "'Enum.' needs the name of a registered enum after it");
				}
				into.Types.push_back(PropertyType::Enum);
				into.Enums.emplace_back(set);
				return;
			}

			PropertyType type = PropertyType::Opaque;
			if (!Schemas::TypeNamed(spelling, type)) {
				luaL_errorL(state, "'%s' is not a field type", std::string(spelling).c_str());
			}

			into.Types.push_back(type);
			into.Enums.emplace_back();
		}

		// `World:DefineComponent(name, { Field = "type", ... })`
		int WorldDefineComponent(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);
			luaL_checktype(state, 3, LUA_TTABLE);

			Declaration declaration;

			lua_pushnil(state);
			while (lua_next(state, 3) != 0) {
				// **The key is copied before it is read as a string.** `lua_next`
				// needs the key back on the stack unchanged for the next step,
				// and `lua_tostring` on a number key rewrites it in place - which
				// ends the traversal in the middle.
				if (lua_type(state, -2) != LUA_TSTRING) {
					luaL_errorL(state, "a component's fields are named, so every key must be a string");
				}

				size_t length = 0;
				const char *field = lua_tolstring(state, -2, &length);
				declaration.Names.emplace_back(field, length);

				const char *spelling = luaL_checkstring(state, -1);
				ReadFieldType(state, spelling, declaration);

				lua_pop(state, 1);
			}

			std::vector<FieldSpec> fields;
			fields.reserve(declaration.Names.size());
			for (size_t at = 0; at < declaration.Names.size(); at++) {
				fields.push_back(
					FieldSpec{declaration.Names[at], declaration.Types[at], declaration.Enums[at]}
				);
			}

			const Schemas::Result result = Schemas::Register(name, fields);
			if (result.Why != Schemas::Status::Ok) {
				luaL_errorL(state, "cannot declare component '%s': %s", name, Explain(result.Why));
			}

			// Whether this call created it or agreed with a declaration already
			// there. Two scripts declaring one component is legal and an author
			// loading a library may want to know which happened.
			lua_pushboolean(state, result.Created);
			return 1;
		}

		// `World:HasComponentType(name)`
		int WorldHasComponentType(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);
			lua_pushboolean(state, Schemas::Find(Name(name)) != nullptr);
			return 1;
		}

		// `World:GetComponentSchema(name)` -> `{ Field = "type" }`
		//
		// **Sorted by field name rather than in layout order.** The layout is
		// widest-first because that is what packs; a person reading a list wants
		// it alphabetical, and nothing about a script's view of a component
		// should depend on how the storage chose to arrange it.
		int WorldGetComponentSchema(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);

			const Schema *schema = Schemas::Find(Name(name));
			if (schema == nullptr) {
				lua_pushnil(state);
				return 1;
			}

			lua_newtable(state);
			for (const FieldDescriptor &field : schema->Fields()) {
				if (field.Type == PropertyType::Enum) {
					const std::string spelling = "Enum." + std::string(field.Enum.Text());
					lua_pushstring(state, spelling.c_str());
				} else {
					lua_pushstring(state, ecs::Describe(field.Type));
				}
				lua_setfield(state, -2, field.Spelling.data());
			}
			return 1;
		}

		// `World:CreateEntity(name?)`
		//
		// **A bare entity: no class, no place in the tree, nothing drawn.** That
		// is what makes it the ECS surface rather than a second `Instance.new` -
		// an entity is a directory slot and a row is what a component buys, so
		// one created here costs a handle until a script attaches something.
		//
		// It is still an `Instance` on the script side, because it is still an
		// entity and there is nothing else for a handle to be. Asking one for
		// `.Name` or `.Parent` fails the way any missing member does: those are
		// properties of the `Instance` class, and this is not an instance of any
		// class.
		int WorldCreateEntity(lua_State *state) {
			Store &store = StoreOf(state);

			const Entity entity =
				lua_isnoneornil(state, 2) ? store.Create() : store.Create(luaL_checkstring(state, 2));

			if (entity == ecs::NULL_ENTITY) {
				// The realistic cause, and the one worth naming: a replica may
				// not mint an authoritative entity, because that index belongs
				// to whoever owns the simulation.
				if (store.AdoptOnly()) {
					luaL_errorL(
						state,
						"this world is a replica and the authority owns its entities. "
						"Test RunService:IsServer() first"
					);
				}
				luaL_errorL(state, "the world could not create an entity");
			}

			PushInstanceValue(state, entity);
			return 1;
		}

		// The component ids a query's arguments name, checked.
		//
		// **A name nothing registered is an error rather than an empty result.**
		// A typo in a query would otherwise be a loop that never runs, which
		// reads exactly like a world with nothing in it.
		std::vector<ComponentId> CheckTerms(lua_State *state, int first) {
			const int top = lua_gettop(state);
			if (top < first) {
				luaL_errorL(state, "a query has to name at least one component");
			}

			std::vector<ComponentId> terms;
			terms.reserve(static_cast<size_t>(top - first + 1));

			for (int argument = first; argument <= top; argument++) {
				const char *name = luaL_checkstring(state, argument);
				const ComponentId id = Components::Find(Name(name));
				if (!id.IsValid()) {
					luaL_errorL(state, "no component named '%s'", name);
				}
				terms.push_back(id);
			}
			return terms;
		}

		// `World:Query(...componentNames)` -> `{ Instance }`
		//
		// **An array, not an iterator.** A coroutine-shaped iterator would hold
		// the storage open across a yield, and `Store::EachMatching` defers
		// structural changes for the length of the call precisely so a body may
		// create and destroy - a suspended iterator would have that scope open
		// for as long as the script felt like. The array is also what lets a
		// script destroy every match without walking a collection it is editing.
		int WorldQuery(lua_State *state) {
			Store &store = StoreOf(state);
			const std::vector<ComponentId> terms = CheckTerms(state, 2);

			lua_newtable(state);
			int index = 0;

			store.EachMatching(terms, [state, &index](Entity entity) {
				PushInstanceValue(state, entity);
				lua_rawseti(state, -2, ++index);
			});
			return 1;
		}

		// `World:Count(...componentNames)`
		int WorldCount(lua_State *state) {
			Store &store = StoreOf(state);
			const std::vector<ComponentId> terms = CheckTerms(state, 2);
			lua_pushinteger(state, static_cast<int>(store.CountMatching(terms)));
			return 1;
		}

		// --- the instance half -------------------------------------------------

		// `entity:SetComponent(name, values)`
		//
		// **Adds or replaces, and the fields a script left out keep what they
		// had.** `SetComponent("Health", { Current = 50 })` on an entity that
		// already has one is a write to `Current` alone, because that is what
		// anybody writing that line means. On an entity that does not, the
		// missing fields take the type's zero - which is what a fresh component
		// is, and is why the value is default-constructed before the table is
		// applied rather than after.
		int InstanceSetComponent(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity entity = CheckEntity(state, 1);
			const char *name = luaL_checkstring(state, 2);

			ComponentId id;
			const Schema &schema = CheckSchema(state, name, id);

			// A tag - a component with no fields - takes no value at all, and
			// the table is optional for one.
			const bool tag = schema.Fields().empty();
			if (!tag) {
				luaL_checktype(state, 3, LUA_TTABLE);
			}

			ComponentValue value(schema, Components::Describe(id));

			// The current value first, so an unmentioned field keeps it. Null
			// when the entity does not carry the component yet, and then the
			// default-constructed blob is already the right starting point.
			value.CopyFrom(store.GetComponent(entity, id));

			if (!tag) {
				lua_pushnil(state);
				while (lua_next(state, 3) != 0) {
					if (lua_type(state, -2) != LUA_TSTRING) {
						luaL_errorL(state, "a component's fields are named, so every key must be a string");
					}

					const char *key = lua_tostring(state, -2);
					const FieldDescriptor *field = schema.Find(std::string_view(key));
					if (field == nullptr) {
						luaL_errorL(state, "'%s' has no field '%s'", name, key);
					}

					// **The string case is separate here for the reason it is
					// separate everywhere else**: the destination is a live
					// `std::string` and the shared marshaller writes into raw
					// bytes, which would overwrite an allocated pointer.
					if (field->Type == PropertyType::String) {
						size_t length = 0;
						const char *text = luaL_checklstring(state, -1, &length);
						*static_cast<std::string *>(value.At(*field)) = std::string(text, length);
					} else if (!ReadPropertyValue(state, -1, field->Type, field->Enum, value.At(*field))) {
						luaL_errorL(state, "'%s.%s' cannot take that value", name, key);
					}

					lua_pop(state, 1);
				}
			}

			store.SetComponent(entity, id, value.Raw());
			return 0;
		}

		// `entity:GetComponent(name)` -> `{ Field = value }` or nil
		int InstanceGetComponent(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity entity = CheckEntity(state, 1);
			const char *name = luaL_checkstring(state, 2);

			ComponentId id;
			const Schema &schema = CheckSchema(state, name, id);

			const void *held = store.GetComponent(entity, id);
			if (held == nullptr && !store.HasComponent(entity, id)) {
				// **Nil rather than an empty table**, so "not carried" and
				// "carried and every field is zero" are different answers. A tag
				// is the case that makes it matter: it has no fields either way.
				lua_pushnil(state);
				return 1;
			}

			lua_newtable(state);
			for (const FieldDescriptor &field : schema.Fields()) {
				const void *bytes = static_cast<const std::byte *>(held) + field.Offset;

				if (field.Type == PropertyType::String) {
					const auto &text = *static_cast<const std::string *>(bytes);
					lua_pushlstring(state, text.data(), text.size());
				} else if (!PushPropertyValue(state, field.Type, field.Enum, bytes)) {
					luaL_errorL(state, "'%s.%s' has no script representation", name, field.Spelling.data());
				}

				lua_setfield(state, -2, field.Spelling.data());
			}
			return 1;
		}

		// `entity:HasComponent(name)`
		int InstanceHasComponent(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity entity = CheckEntity(state, 1);
			const char *name = luaL_checkstring(state, 2);

			// **Not `CheckSchema`, because asking is not reaching.** A script
			// testing for a component the engine declared should be told "no"
			// rather than told off: the entity either carries it or does not,
			// and that is a question with a correct answer either way.
			const ComponentId id = Components::Find(Name(name));
			lua_pushboolean(state, id.IsValid() && store.HasComponent(entity, id));
			return 1;
		}

		// `entity:RemoveComponent(name)`
		int InstanceRemoveComponent(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity entity = CheckEntity(state, 1);
			const char *name = luaL_checkstring(state, 2);

			ComponentId id;
			CheckSchema(state, name, id);

			store.RemoveComponent(entity, id);
			return 0;
		}

		// `entity:GetComponents()` -> `{ string }`
		//
		// **Every component, including the ones the engine declared.** A `Part`
		// answers with `scene.Visual` and the rest, which is the honest view of
		// what an instance is and is exactly what a debug panel needs. Sorted by
		// name so two runs agree - the storage's own order is by registration id,
		// which is stable within a build and says nothing to a reader.
		int InstanceGetComponents(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity entity = CheckEntity(state, 1);

			std::vector<std::string_view> names;
			for (const ComponentId id : store.ComponentsOf(entity)) {
				names.push_back(Components::Describe(id).Name.Text());
			}
			std::sort(names.begin(), names.end());

			lua_newtable(state);
			for (size_t at = 0; at < names.size(); at++) {
				lua_pushlstring(state, names[at].data(), names[at].size());
				lua_rawseti(state, -2, static_cast<int>(at) + 1);
			}
			return 1;
		}
	}

	void OpenEcs(lua_State *state) {
		LuauContext &context = ContextOf(state);

		static const struct {
			const char *Name;
			lua_CFunction Function;
		} WORLD[] = {
			{"DefineComponent", WorldDefineComponent},
			{"HasComponentType", WorldHasComponentType},
			{"GetComponentSchema", WorldGetComponentSchema},
			{"CreateEntity", WorldCreateEntity},
			{"Query", WorldQuery},
			{"Count", WorldCount},
		};

		lua_newtable(state);
		for (const auto &entry : WORLD) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, entry.Function, entry.Name, 1);
			lua_setfield(state, -2, entry.Name);
		}
		lua_setglobal(state, "World");

		// **On the shared instance method table rather than on a wrapper**, and
		// that follows from the model rather than being a convenience: the thing
		// a component is attached to is an entity, and every `Instance` userdata
		// already is one. A separate `Entity` type would have been a second
		// handle onto the same sixty-four bits.
		//
		// `OpenInstances` builds that table, so this runs after it -
		// `LuauRuntime` orders the two.
		static const struct {
			const char *Name;
			lua_CFunction Function;
		} METHODS[] = {
			{"SetComponent", InstanceSetComponent},
			{"GetComponent", InstanceGetComponent},
			{"HasComponent", InstanceHasComponent},
			{"RemoveComponent", InstanceRemoveComponent},
			{"GetComponents", InstanceGetComponents},
		};

		lua_getfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");
		for (const auto &entry : METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, entry.Function, entry.Name, 1);
			lua_setfield(state, -2, entry.Name);
		}
		lua_pop(state, 1);
	}
}
