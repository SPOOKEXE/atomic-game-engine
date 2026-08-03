#include "Bindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>

#include <lualib.h>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		// The world every instance in this VM belongs to.
		//
		// Reached through the context on a light-userdata upvalue rather than a
		// global, so two runtimes over two worlds cannot reach each other's
		// store — the mistake a file-static would make available.
		Store &StoreOf(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		Entity CheckInstance(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Instance");
			}
			return *static_cast<Entity *>(value);
		}

		// Whether a value is the world.
		bool IsWorkspace(lua_State *state, int index) {
			return lua_touserdatatagged(state, index, TAG_WORLD) != nullptr;
		}

		void PushWorkspace(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace");
		}

		void PushInstance(lua_State *state, Entity entity) {
			void *memory = lua_newuserdatatagged(state, sizeof(Entity), TAG_INSTANCE);
			*static_cast<Entity *>(memory) = entity;

			luaL_getmetatable(state, "Instance");
			lua_setmetatable(state, -2);
		}

		// --- marshalling -----------------------------------------------------
		//
		// **A switch over `PropertyType` and nothing else.** No property is
		// named here and none ever should be: a property `scene` declares
		// tomorrow is readable and writable from Luau today, because this code
		// only ever learned the shapes a value can have.

		// Pushes a property's value, having read it into a buffer of its size.
		bool PushValue(lua_State *state, const PropertyDescriptor &property, const void *bytes) {
			switch (property.Type) {
			case PropertyType::Bool:
				lua_pushboolean(state, *static_cast<const bool *>(bytes));
				return true;
			case PropertyType::Float:
				lua_pushnumber(state, *static_cast<const float *>(bytes));
				return true;
			case PropertyType::Double:
				lua_pushnumber(state, *static_cast<const double *>(bytes));
				return true;
			case PropertyType::Int32:
				lua_pushinteger(state, *static_cast<const int32_t *>(bytes));
				return true;
			case PropertyType::Int64:
				lua_pushnumber(state, static_cast<double>(*static_cast<const int64_t *>(bytes)));
				return true;
			case PropertyType::Name:
				// Text, never the interned id. A number that means a string in
				// one process and a different string in the next is the whole
				// hazard `core::Name` exists around.
				lua_pushstring(state, static_cast<const Name *>(bytes)->Text().data());
				return true;
			case PropertyType::Enum:
				// An `EnumItem`, not a string — that is the whole difference
				// this type buys. The value is an interned `Name` exactly as
				// `PropertyType::Name` is; what changes is that userland gets a
				// value it can compare against `Enum.Material.Plastic` and be
				// told when it is wrong.
				PushEnumItem(state, property.EnumName, *static_cast<const Name *>(bytes));
				return true;
			case PropertyType::Vector3:
				*PushVector3(state) = *static_cast<const core::Vector3 *>(bytes);
				return true;
			case PropertyType::Color3:
				*PushColor3(state) = *static_cast<const core::Color3 *>(bytes);
				return true;
			case PropertyType::CFrame:
				*PushCFrame(state) = *static_cast<const core::CFrame *>(bytes);
				return true;
			case PropertyType::Reference: {
				// A root instance's parent is the world, which is what
				// `workspace` is. Handing back nil would make
				// `part.Parent = workspace` a write a script could not read
				// back, and the two would disagree about the same fact.
				const Entity referenced = *static_cast<const Entity *>(bytes);
				if (referenced == ecs::NULL_ENTITY) {
					PushWorkspace(state);
				} else {
					PushInstance(state, referenced);
				}
				return true;
			}
			case PropertyType::Opaque:
				break;
			}
			return false;
		}

		// Reads a Luau value into a buffer of the property's size.
		bool ReadValue(lua_State *state, int index, const PropertyDescriptor &property, void *out) {
			switch (property.Type) {
			case PropertyType::Bool:
				*static_cast<bool *>(out) = lua_toboolean(state, index) != 0;
				return true;
			case PropertyType::Float:
				*static_cast<float *>(out) = static_cast<float>(luaL_checknumber(state, index));
				return true;
			case PropertyType::Double:
				*static_cast<double *>(out) = luaL_checknumber(state, index);
				return true;
			case PropertyType::Int32:
				*static_cast<int32_t *>(out) = luaL_checkinteger(state, index);
				return true;
			case PropertyType::Int64:
				*static_cast<int64_t *>(out) = static_cast<int64_t>(luaL_checknumber(state, index));
				return true;
			case PropertyType::Name:
				*static_cast<Name *>(out) = Name(luaL_checkstring(state, index));
				return true;
			case PropertyType::Enum:
				// **A string is accepted as well as an `EnumItem`**, because
				// `part.Material = "Plastic"` is what Roblox accepts and what a
				// migrating script already contains. What is refused is a
				// member of the *wrong* enum, which is the error a bare string
				// could never have caught.
				return ReadEnumValue(state, index, property.EnumName, *static_cast<Name *>(out));
			case PropertyType::Vector3:
				*static_cast<core::Vector3 *>(out) = CheckVector3(state, index);
				return true;
			case PropertyType::Color3:
				*static_cast<core::Color3 *>(out) = CheckColor3(state, index);
				return true;
			case PropertyType::CFrame:
				*static_cast<core::CFrame *>(out) = CheckCFrame(state, index);
				return true;
			case PropertyType::Reference:
				// `part.Parent = workspace` — a root of this world.
				if (IsWorkspace(state, index) || lua_isnil(state, index)) {
					*static_cast<Entity *>(out) = ecs::NULL_ENTITY;
					return true;
				}
				*static_cast<Entity *>(out) = CheckInstance(state, index);
				return true;
			case PropertyType::Opaque:
				break;
			}
			return false;
		}

		// **Compared as text, not interned, and that is a measurement rather
		// than a preference.**
		//
		// This used to build a `core::Name` from the field and compare ids. An
		// id compare is an integer compare and looks like the cheap option — but
		// *making* the id is a lock on the process-wide registry plus a hash
		// lookup, paid on every property read and every property write. A script
		// animating a scene does that constantly: `Mirrors-1-world` touches
		// three properties on each of 24 parts every frame, so 72 lock
		// acquisitions a frame at 60 Hz, on a registry shared with every worker
		// thread in the job system.
		//
		// It showed up as `property lookup` costing about half of every
		// `instance set` on F5, which is what a lock in the wrong place looks
		// like from the outside: no spike anybody can point at, just a beat that
		// is always slower than the work in it.
		//
		// A class carries a dozen or so properties, so the scan is a dozen short
		// string compares over memory that is already hot — and `string_view`'s
		// comparison rejects on length before it reads a byte. No lock, no hash,
		// no allocation, and the interned id is never needed because nothing
		// here stores the name.
		const PropertyDescriptor *Find(const Store &store, Entity instance, std::string_view name) {
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Name.Text() == name) {
					return &property;
				}
			}
			return nullptr;
		}

		// --- methods ---------------------------------------------------------
		//
		// Every one of these is a call `Store` already had and a script could
		// not spell. Nothing new happens in the storage; what is new is that
		// `part:Destroy()` reaches `Store::DestroyInstance` instead of being a
		// missing member.

		// `instance:IsA(className)`
		int InstanceIsA(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *className = luaL_checkstring(state, 2);

			const ecs::ClassId wanted = ecs::Classes::Find(Name(className));
			if (!wanted.IsValid()) {
				// False rather than an error, matching Roblox. A script testing
				// for a class this game does not register is asking a question
				// with a correct answer, and it is "no".
				lua_pushboolean(state, false);
				return 1;
			}

			lua_pushboolean(state, ecs::Classes::IsA(store.ClassOf(instance), wanted));
			return 1;
		}

		// `instance:Destroy()`
		int InstanceDestroy(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const Entity instance = CheckInstance(state, 1);

			// **The signal table is told before the storage is.** A `.Changed`
			// connection on a destroyed row would otherwise fire against a dead
			// handle every tick for the rest of the world's life, and
			// `EachSubject` would keep visiting it.
			std::vector<CallbackRef> released;
			context.Signals.DropSubject(instance, released);
			context.Changes.Unwatch(instance);

			for (const CallbackRef reference : released) {
				lua_unref(state, reference);
			}

			// Children go too, and each of them has to be forgotten as well.
			// `DestroyInstance` takes the whole subtree, so a listener on a
			// grandchild would survive the row it was watching.
			context.World->EachChild(instance, [&](Entity child) {
				std::vector<CallbackRef> fromChild;
				context.Signals.DropSubject(child, fromChild);
				context.Changes.Unwatch(child);
				for (const CallbackRef reference : fromChild) {
					lua_unref(state, reference);
				}
			});

			context.World->DestroyInstance(instance);
			return 0;
		}

		// `instance:Clone()`
		int InstanceClone(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			const Entity copy = store.CloneInstance(instance);
			if (copy == ecs::NULL_ENTITY) {
				// Nil rather than an error, matching Roblox: a clone of
				// something unclonable is nil, and a script can test for it.
				lua_pushnil(state);
				return 1;
			}

			PushInstance(state, copy);
			return 1;
		}

		// `instance:GetChildren()`
		int InstanceGetChildren(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			lua_newtable(state);
			int index = 0;
			store.EachChild(instance, [&](Entity child) {
				PushInstance(state, child);
				lua_rawseti(state, -2, ++index);
			});
			return 1;
		}

		// `instance:GetDescendants()`
		//
		// Depth first, children before grandchildren, which is Roblox's order
		// and the one a script writing a recursive walk by hand would produce.
		int InstanceGetDescendants(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			lua_newtable(state);
			int index = 0;

			// An explicit stack rather than recursion: a deep tree would put the
			// depth of the scene onto the C stack, and a scene's depth is the
			// author's to choose.
			std::vector<Entity> pending;
			store.EachChild(instance, [&](Entity child) { pending.push_back(child); });

			while (!pending.empty()) {
				const Entity current = pending.front();
				pending.erase(pending.begin());

				PushInstance(state, current);
				lua_rawseti(state, -2, ++index);

				size_t insertAt = 0;
				store.EachChild(current, [&](Entity child) {
					pending.insert(pending.begin() + static_cast<ptrdiff_t>(insertAt++), child);
				});
			}
			return 1;
		}

		// `instance:FindFirstChild(name)`
		int InstanceFindFirstChild(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *name = luaL_checkstring(state, 2);

			const Entity found = store.FindFirstChild(instance, name);
			if (found == ecs::NULL_ENTITY) {
				lua_pushnil(state);
				return 1;
			}

			PushInstance(state, found);
			return 1;
		}

		// `instance:IsDescendantOf(ancestor)`
		int InstanceIsDescendantOf(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			// The world is every root's ancestor, so `part:IsDescendantOf(
			// workspace)` is true for anything parented into it.
			if (IsWorkspace(state, 2)) {
				lua_pushboolean(state, store.Alive(instance));
				return 1;
			}

			lua_pushboolean(state, store.IsDescendantOf(instance, CheckInstance(state, 2)));
			return 1;
		}

		// `instance:ClearAllChildren()`
		int InstanceClearAllChildren(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const Entity instance = CheckInstance(state, 1);

			// Collected first. `DestroyInstance` unlinks from the sibling list
			// the walk is standing in, so destroying inside `EachChild` would
			// visit whatever moved into the slot — or nothing.
			std::vector<Entity> children;
			context.World->EachChild(instance, [&](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				std::vector<CallbackRef> released;
				context.Signals.DropSubject(child, released);
				context.Changes.Unwatch(child);
				for (const CallbackRef reference : released) {
					lua_unref(state, reference);
				}
				context.World->DestroyInstance(child);
			}
			return 0;
		}

		// `instance:GetPropertyChangedSignal(name)`
		int InstanceGetPropertyChangedSignal(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			// Refused for a property that does not exist, which is the one place
			// a typo in a signal name can still be caught. A signal that
			// silently never fired would be indistinguishable from a value that
			// never changed.
			if (Find(store, instance, field) == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}

			PushSignal(state, SignalKind::PropertyChanged, instance, Name(field));
			return 1;
		}

		// The method table, built once and shared by every instance.
		//
		// On the metatable rather than on each userdata, for the reason the
		// JavaScript side puts accessors on a prototype: a scene of five hundred
		// parts would otherwise carry five hundred copies of one closure.
		void PushMethods(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");
		}

		// --- the metatable ---------------------------------------------------

		int InstanceIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			// **`.Changed` before the property lookup**, because it is not a
			// property and never will be: it projects onto no component, and
			// declaring one for scripts alone is the change `script/AGENTS.md`
			// says to refuse.
			if (name == "Changed") {
				PushSignal(state, SignalKind::Changed, instance);
				return 1;
			}

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property != nullptr) {
				// Sized from the descriptor rather than from a guess, so this
				// cannot be the place a size mismatch is introduced.
				alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
				if (property->Size > sizeof(bytes) ||
					!store.GetProperty(instance, property->Name, bytes, property->Size)) {
					luaL_errorL(state, "could not read '%s'", field);
				}

				if (!PushValue(state, *property, bytes)) {
					luaL_errorL(state, "'%s' has no script representation", field);
				}
				return 1;
			}

			// A method next.
			PushMethods(state);
			lua_getfield(state, -1, field);
			if (!lua_isnil(state, -1)) {
				lua_remove(state, -2);
				return 1;
			}
			lua_pop(state, 2);

			// **And finally a child by name.** `workspace.Baseplate` is how a
			// Roblox script reaches one, and it is last rather than first
			// deliberately: a child named `Size` must not shadow the property,
			// or a scene could break every script that touched it by adding a
			// part with an unlucky name.
			const Entity child = store.FindFirstChild(instance, name);
			if (child != ecs::NULL_ENTITY) {
				PushInstance(state, child);
				return 1;
			}

			luaL_errorL(state, "'%s' is not a valid member of this instance", field);
		}

		int InstanceNewIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}
			if (!property->Writable) {
				luaL_errorL(state, "'%s' is read-only", field);
			}

			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property->Size > sizeof(bytes) || !ReadValue(state, 3, *property, bytes)) {
				luaL_errorL(state, "'%s' cannot take that value", field);
			}

			// A refusal is an error rather than a silent no-op. A replica
			// rejecting the write is the case that matters: a script author
			// cannot tell "rejected" from "applied and then overwritten by the
			// next delta" without being told.
			if (!store.SetProperty(instance, property->Name, bytes, property->Size)) {
				if (store.AdoptOnly()) {
					luaL_errorL(
						state,
						"'%s' cannot be set here: this world is a replica, and the authority owns it. "
						"Test RunService:IsServer() first",
						field
					);
				}
				luaL_errorL(state, "could not set '%s'", field);
			}
			return 0;
		}

		int InstanceToString(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushstring(state, store.InstanceNameOf(instance).Text().data());
			return 1;
		}

		// Two handles onto one entity are one instance. Without this,
		// `part.Parent.Parent == workspace` would be false for an object a
		// script had never seen before, because each read mints a new userdata.
		int InstanceEqual(lua_State *state) {
			lua_pushboolean(state, CheckInstance(state, 1) == CheckInstance(state, 2));
			return 1;
		}

		int InstanceNew(lua_State *state) {
			Store &store = StoreOf(state);
			const char *className = luaL_checkstring(state, 1);

			const ecs::ClassId id = ecs::Classes::Find(Name(className));
			if (!id.IsValid()) {
				luaL_errorL(state, "'%s' is not a registered class", className);
			}

			const Entity instance = store.CreateInstance(id, className);
			if (instance == ecs::NULL_ENTITY) {
				if (store.AdoptOnly()) {
					luaL_errorL(
						state,
						"cannot create a '%s' here: this world is a replica and may not mint entities",
						className
					);
				}
				luaL_errorL(state, "could not create a '%s'", className);
			}

			// **The second argument, which Roblox has and v0.5 did not.**
			// `Instance.new("Part", workspace)` is one call rather than two, and
			// the difference is not only brevity: a part created and parented in
			// one statement is never briefly a root of the world, so nothing
			// that walks roots can observe the half-built state.
			if (!lua_isnoneornil(state, 2)) {
				const Entity parent = IsWorkspace(state, 2) ? ecs::NULL_ENTITY : CheckInstance(state, 2);
				if (!store.SetParent(instance, parent)) {
					luaL_errorL(state, "could not parent the new '%s'", className);
				}
			}

			PushInstance(state, instance);
			return 1;
		}
	}

	void PushInstanceValue(lua_State *state, ecs::Entity instance) {
		PushInstance(state, instance);
	}

	void OpenInstances(lua_State *state, ecs::Store &store) {
		LuauContext &context = ContextOf(state);

		// The method table, in the registry so `__index` hands back one shared
		// closure per method rather than building one per access.
		static const struct {
			const char *Name;
			lua_CFunction Function;
		} METHODS[] = {
			{"IsA", InstanceIsA},
			{"Destroy", InstanceDestroy},
			{"Clone", InstanceClone},
			{"GetChildren", InstanceGetChildren},
			{"GetDescendants", InstanceGetDescendants},
			{"FindFirstChild", InstanceFindFirstChild},
			{"IsDescendantOf", InstanceIsDescendantOf},
			{"ClearAllChildren", InstanceClearAllChildren},
			{"GetPropertyChangedSignal", InstanceGetPropertyChangedSignal},
		};

		lua_newtable(state);
		for (const auto &method : METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");

		luaL_newmetatable(state, "Instance");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceNewIndex, "__newindex", 1);
		lua_setfield(state, -2, "__newindex");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceToString, "__tostring", 1);
		lua_setfield(state, -2, "__tostring");

		lua_pushcfunction(state, InstanceEqual, "__eq");
		lua_setfield(state, -2, "__eq");

		// Hidden, for the reason the value types hide theirs: a metatable a
		// script can reach is one it can rewrite, and then every instance in
		// the world changes behaviour underneath everything holding one.
		lua_pushstring(state, "Instance");
		lua_setfield(state, -2, "__metatable");

		// What `typeof` reads — see `Values.cpp`'s `Install`.
		lua_pushstring(state, "Instance");
		lua_setfield(state, -2, "__type");

		lua_pop(state, 1);

		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceNew, "new", 1);
		lua_setfield(state, -2, "new");
		lua_setglobal(state, "Instance");

		(void)store;
	}

	std::string PumpChanges(lua_State *state) {
		LuauContext &context = ContextOf(state);
		if (context.Changes.Empty()) {
			return {};
		}

		std::string firstError;
		context.Changes.Drain([&](ecs::Entity instance, core::Name property) {
			// **Both signals, from one queue entry.** `.Changed` takes the
			// property's name and `GetPropertyChangedSignal` takes nothing,
			// which is Roblox's split and the reason the second exists at all —
			// a handler that only cares about one property should not be called
			// for every other one and made to filter.
			lua_pushstring(state, property.Text().data());
			const std::string changed = FireSignal(state, SignalKind::Changed, instance, 1);
			if (firstError.empty()) {
				firstError = changed;
			}

			LuauContext &live = ContextOf(state);
			live.Signals.Fire(SignalKind::PropertyChanged, instance, [&](const Connection &connection) {
				if (connection.Property != property) {
					return;
				}

				lua_getref(state, connection.Callback);
				if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a property listener failed";
					}
					lua_pop(state, 1);
				}
			});
		});
		return firstError;
	}

	namespace {
		int WorkspaceIndex(lua_State *state) {
			Store &store = StoreOf(state);
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			// The world's own name — `client.world`, `unified.server`. A
			// script that logs it is telling you which world it is running on,
			// which is a real question the moment a universe holds several.
			if (name == "Name") {
				lua_pushlstring(state, store.Name().data(), store.Name().size());
				return 1;
			}

			// `workspace.CurrentCamera` — the live camera, or nil when nothing
			// has made one. See `Camera.cpp`.
			if (name == "CurrentCamera") {
				PushCurrentCamera(state);
				return 1;
			}

			// A method next. **`__index` is a function here, so nothing falls
			// through to the metatable's own fields** — a method put there would
			// be invisible, which is exactly how `workspace:GetChildren()` was
			// missing while `GetChildren` sat on the metatable in plain sight.
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.world.methods");
			lua_getfield(state, -1, field);
			if (!lua_isnil(state, -1)) {
				lua_remove(state, -2);
				return 1;
			}
			lua_pop(state, 2);

			// And finally a root instance by name, which is what
			// `workspace.Baseplate` is. The world is the parent of every root.
			// Last, for the reason an instance's own children are last: a root
			// named `CurrentCamera` must not shadow the property.
			const Entity root = store.FindFirstRoot(name);
			if (root != ecs::NULL_ENTITY) {
				PushInstance(state, root);
				return 1;
			}

			luaL_errorL(state, "the world has no member '%s'", field);
		}

		int WorkspaceNewIndex(lua_State *state) {
			const char *field = luaL_checkstring(state, 2);

			if (std::string_view(field) == "CurrentCamera") {
				SetCurrentCamera(state, 3);
				return 0;
			}

			luaL_errorL(state, "the world's '%s' cannot be set", field);
		}

		// `workspace:GetChildren()` and friends. The world is the parent of
		// every root, so these are the instance methods with `NULL_ENTITY` in
		// place of the subject.
		int WorkspaceGetChildren(lua_State *state) {
			Store &store = StoreOf(state);

			lua_newtable(state);
			int index = 0;
			store.EachRoot([&](Entity child) {
				PushInstance(state, child);
				lua_rawseti(state, -2, ++index);
			});
			return 1;
		}

		int WorkspaceFindFirstChild(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity found = store.FindFirstRoot(luaL_checkstring(state, 2));

			if (found == ecs::NULL_ENTITY) {
				lua_pushnil(state);
				return 1;
			}
			PushInstance(state, found);
			return 1;
		}

		int WorkspaceToString(lua_State *state) {
			Store &store = StoreOf(state);
			lua_pushlstring(state, store.Name().data(), store.Name().size());
			return 1;
		}
	}

	void OpenWorkspace(lua_State *state, ecs::Store &store) {
		LuauContext &context = ContextOf(state);

		// Zero-sized userdata: it carries no state because the world it stands
		// for is reached through the upvalue every bound function already has.
		// What it needs is an identity a script can compare and assign.
		lua_newuserdatatagged(state, 1, TAG_WORLD);

		luaL_newmetatable(state, "World");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceNewIndex, "__newindex", 1);
		lua_setfield(state, -2, "__newindex");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceToString, "__tostring", 1);
		lua_setfield(state, -2, "__tostring");

		lua_pushstring(state, "World");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "World");
		lua_setfield(state, -2, "__type");

		lua_setmetatable(state, -2);

		// Kept in the registry as well as in a global, so the `Parent` getter
		// can hand back *the same* value a script assigned rather than a second
		// object that merely behaves alike.
		lua_pushvalue(state, -1);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.workspace");
		lua_setglobal(state, "workspace");

		// The world's methods, in their own registry table because `__index`
		// looks them up rather than falling through to the metatable.
		//
		// Its own table rather than the instance one: a world is not an
		// instance, so it answers a smaller set — and sharing the table would
		// have offered `workspace:Destroy()`, which means nothing.
		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceGetChildren, "GetChildren", 1);
		lua_setfield(state, -2, "GetChildren");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, WorkspaceFindFirstChild, "FindFirstChild", 1);
		lua_setfield(state, -2, "FindFirstChild");
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.world.methods");

		(void)store;
	}
}
