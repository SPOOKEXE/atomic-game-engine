// The `host` global, and calling back into a script from the program.
//
// **Everything a host offers arrives through one closure.** `OpenHost` builds a
// table with one entry per `HostSurface::Names`, each a C function carrying the
// name it was made for — so a call is a marshal, one virtual call, and a marshal
// back. There is no per-name C++ anywhere in this module, which is the whole
// point: the editor adds `CreateDockWidget` and nothing here changes.
//
// **A name the host does not list is not a member.** The table is built from
// `Names()` rather than answered by an `__index` that asks the host, so a typo
// is "attempt to call a nil value" at the call site instead of a refusal from
// inside a program the author cannot see.
//
// ## The two things that make this more than a function table
//
// **A Luau function passed as an argument becomes a `HostCallback`.** It is put
// in the registry and the ref is remembered on the context, keyed by a counter
// that starts at one — so zero is "no callback" and no id is ever an address,
// which `physics/AGENTS.md` and this module's own determinism rules both refuse.
//
// **An `Instance` crosses in both directions.** A host call is inside one
// process against one store, so a handle means something — which is exactly why
// `ScriptValue` refuses one and `HostValue` does not. `Host.hpp` carries that
// argument.

#include "Bindings.hpp"

#include <engine/core/Log.hpp>

#include <algorithm>
#include <lualib.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;

		// How deep a table may nest on its way to a host.
		//
		// **The codec's own limit, for the codec's own reason**: a table
		// reachable from itself would otherwise recurse until the C stack ran
		// out, which is a crash with no line number from a script that looks
		// fine. This one is checked by depth alone rather than by a visited set,
		// because a host call is a handful of values and the set would cost more
		// than the recursion it guards.
		constexpr int HOST_MAX_DEPTH = 16;

		// Whether a table has no entries at all.
		//
		// **`lua_objlen` cannot answer this**: it reports the array part's
		// length, which is zero for an empty table and also zero for a table of
		// nothing but named keys. One `lua_next` is the whole test.
		bool IsEmptyTable(lua_State *state, int index) {
			lua_pushnil(state);
			if (lua_next(state, index) == 0) {
				return true;
			}

			// `lua_next` left a key and a value behind.
			lua_pop(state, 2);
			return false;
		}

		// One argument, read off the stack.
		//
		// Returns false for a value with no host representation, which the
		// caller turns into a script error naming the position — a thread or a
		// userdata the host has no way to hold.
		bool ReadHostValue(lua_State *state, int index, HostValue &out, int depth) {
			if (depth > HOST_MAX_DEPTH) {
				return false;
			}

			// **Room for the slots this level is about to use**, and it has to
			// be asked for rather than assumed: a C function is guaranteed
			// `LUA_MINSTACK` free slots and nothing more, and a map traversal
			// holds a key and a value per level while it recurses. Sixteen
			// levels is thirty-two slots, which is past the guarantee — and
			// overrunning it is a `LUAU_ASSERT` rather than a wrong answer, so
			// the symptom was an illegal instruction from a script that merely
			// nested a table.
			if (lua_checkstack(state, 4) == 0) {
				return false;
			}

			const int type = lua_type(state, index);
			switch (type) {
			case LUA_TNIL:
			case LUA_TNONE:
				out = HostValue{};
				return true;

			case LUA_TBOOLEAN:
				out = HostValue::Of(lua_toboolean(state, index) != 0);
				return true;

			case LUA_TNUMBER:
				out = HostValue::Of(lua_tonumber(state, index));
				return true;

			case LUA_TSTRING: {
				size_t length = 0;
				const char *text = lua_tolstring(state, index, &length);
				out = HostValue::Of(std::string_view(text, length));
				return true;
			}

			case LUA_TFUNCTION: {
				// **The registry, and a counter for the id.** An address would
				// be stable and would also be an address, which this module's
				// determinism rules refuse — and a counter reads better in a log.
				LuauContext &context = ContextOf(state);

				lua_pushvalue(state, index);
				const int reference = lua_ref(state, -1);
				lua_pop(state, 1);

				const HostCallback callback{++context.NextHostCallback};
				context.HostCallbacks.emplace(callback.Id, reference);

				out = HostValue(HostTag::Callback);
				out.Callback = callback;
				return true;
			}

			case LUA_TUSERDATA: {
				// The three value types a host is likely to be handed, plus the
				// one that makes the seam worth having.
				if (void *value = lua_touserdatatagged(state, index, TAG_INSTANCE); value != nullptr) {
					out = HostValue::Of(*static_cast<Entity *>(value));
					return true;
				}
				if (void *value = lua_touserdatatagged(state, index, TAG_VECTOR3); value != nullptr) {
					out = HostValue(HostTag::Vector3);
					out.Vector = *static_cast<core::Vector3 *>(value);
					return true;
				}
				if (void *value = lua_touserdatatagged(state, index, TAG_COLOR3); value != nullptr) {
					out = HostValue(HostTag::Color3);
					out.Colour = *static_cast<core::Color3 *>(value);
					return true;
				}
				if (void *value = lua_touserdatatagged(state, index, TAG_CFRAME); value != nullptr) {
					out = HostValue(HostTag::CFrame);
					out.Frame = *static_cast<core::CFrame *>(value);
					return true;
				}
				return false;
			}

			case LUA_TTABLE: {
				// **An array if it has a first element, a map otherwise**, which
				// is Luau's own ambiguity and the same reading `Codec.cpp`
				// makes: there is one table type and two shapes, and the length
				// operator is what tells them apart.
				//
				// **An empty table is an array, and that is a decision rather
				// than a fallthrough.** `{}` is the same value either way and the
				// reader has to pick one; a host expecting a map finds no entries
				// under either tag, where a host expecting a *list* gets a tag it
				// refuses. So the ambiguity is harmless in one direction and not
				// in the other — and `Selection:Set({})`, which is how a plugin
				// deselects everything, is exactly the call that would have been
				// refused.
				const int absolute = lua_absindex(state, index);

				if (lua_objlen(state, absolute) > 0 || IsEmptyTable(state, absolute)) {
					out = HostValue(HostTag::Array);
					const int count = static_cast<int>(lua_objlen(state, absolute));

					for (int at = 1; at <= count; at++) {
						lua_rawgeti(state, absolute, at);

						HostValue item;
						const bool ok = ReadHostValue(state, -1, item, depth + 1);
						lua_pop(state, 1);

						if (!ok) {
							return false;
						}
						out.Items.push_back(std::move(item));
					}
					return true;
				}

				out = HostValue(HostTag::Map);
				lua_pushnil(state);
				while (lua_next(state, absolute) != 0) {
					if (lua_type(state, -2) != LUA_TSTRING) {
						// A key the host could not name. Refused rather than
						// stringified: `[1]` and `["1"]` would become one entry.
						lua_pop(state, 2);
						return false;
					}

					size_t length = 0;
					const char *key = lua_tolstring(state, -2, &length);
					std::string named(key, length);

					HostValue item;
					const bool ok = ReadHostValue(state, -1, item, depth + 1);
					lua_pop(state, 1);

					if (!ok) {
						lua_pop(state, 1);
						return false;
					}
					out.Entries.emplace_back(std::move(named), std::move(item));
				}
				return true;
			}

			default:
				return false;
			}
		}

		// One value, pushed back onto the stack.
		void PushHostValue(lua_State *state, const HostValue &value) {
			switch (value.Tag) {
			case HostTag::Nil:
				lua_pushnil(state);
				return;
			case HostTag::Boolean:
				lua_pushboolean(state, value.Boolean);
				return;
			case HostTag::Number:
				lua_pushnumber(state, value.Number);
				return;
			case HostTag::String:
				lua_pushlstring(state, value.Text.data(), value.Text.size());
				return;
			case HostTag::Instance:
				PushInstanceValue(state, value.Instance);
				return;
			case HostTag::Vector3:
				*PushVector3(state) = value.Vector;
				return;
			case HostTag::Color3:
				*PushColor3(state) = value.Colour;
				return;
			case HostTag::CFrame:
				*PushCFrame(state) = value.Frame;
				return;

			case HostTag::Array: {
				lua_newtable(state);
				for (size_t at = 0; at < value.Items.size(); at++) {
					PushHostValue(state, value.Items[at]);
					lua_rawseti(state, -2, static_cast<int>(at) + 1);
				}
				return;
			}

			case HostTag::Map: {
				lua_newtable(state);
				for (const auto &[key, item] : value.Entries) {
					PushHostValue(state, item);
					lua_setfield(state, -2, key.c_str());
				}
				return;
			}

			case HostTag::Callback:
				// **A handler does not come back.** It went in as a reference
				// this module holds; handing it out again would be a second
				// name for one function and a second thing to release.
				lua_pushnil(state);
				return;
			}
			lua_pushnil(state);
		}

		// `host.<Name>(...)`, for whichever name this closure was built with.
		int HostCall(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const char *name = lua_tostring(state, lua_upvalueindex(2));

			if (context.Host == nullptr) {
				luaL_errorL(state, "this program offers no host surface");
			}

			const int count = lua_gettop(state);

			// **A service method may be called with a colon or a dot, and both
			// have to work.** `Selection:Get()` is what a Roblox script writes
			// and it passes the service table as the first argument; a host has
			// no use for it, and one that saw it would have to skip it in every
			// method it implements.
			//
			// Compared against the table this closure was built for rather than
			// against "is argument one a table", because a method whose first
			// real argument *is* a table — `Selection:Set({part})` — must not
			// lose it.
			int first = 1;
			if (count >= 1 && lua_type(state, lua_upvalueindex(3)) == LUA_TTABLE &&
				lua_rawequal(state, 1, lua_upvalueindex(3)) != 0) {
				first = 2;
			}

			std::vector<HostValue> arguments;
			arguments.reserve(static_cast<size_t>(count));

			for (int at = first; at <= count; at++) {
				HostValue value;
				if (!ReadHostValue(state, at, value, 0)) {
					luaL_errorL(state, "argument %d of %s has no host representation", at - first + 1, name);
				}
				arguments.push_back(std::move(value));
			}

			HostValue result;
			std::string failure;

			if (!context.Host->Call(name, arguments, result, failure)) {
				// **The host's own words.** A program refusing a call knows why
				// and the script author is the person who has to act on it, so
				// nothing here rewrites the message.
				luaL_errorL(state, "%s: %s", name, failure.c_str());
			}

			PushHostValue(state, result);
			return 1;
		}
	}

	void OpenHost(lua_State *state) {
		LuauContext &context = ContextOf(state);

		if (context.Host == nullptr) {
			// No host is the ordinary case — a game script has none — and a
			// world with no host global is what says so.
			return;
		}

		// **The globals may already be frozen, and that is the ordinary case.**
		// `luaL_sandbox` runs at construction so one script cannot rewrite the
		// language the next one runs in, and a host installed afterwards —
		// which `Runtime::SetHost` is — would otherwise be "attempt to modify a
		// readonly table" thrown out of a setter.
		//
		// Unfrozen for exactly this set of assignments and frozen again, rather
		// than left writable: the sandbox is what stops a plugin redefining
		// `print` for every chunk after it, and a host is not a reason to give
		// that up.
		lua_pushvalue(state, LUA_GLOBALSINDEX);
		const bool frozen = lua_getreadonly(state, -1) != 0;
		if (frozen) {
			lua_setreadonly(state, -1, 0);
		}

		// **A dotted name is a service and a bare one is a plain call.**
		// `Selection.Get` becomes `Selection:Get()`, and `game:GetService`
		// then finds it for free — that function resolves a service by looking
		// up a global of the same name, so a service installed here is reachable
		// both ways without `GetService` learning anything. `RunService` is
		// already exactly this shape, and the comment there gives the reason:
		// two objects for one service is two things to keep in step, and a
		// script comparing them would find them different.
		//
		// Collected before anything is installed, because a service's closures
		// take the service table as an upvalue and the table has to exist first.
		std::vector<std::string> flat;
		std::vector<std::pair<std::string, std::vector<std::string>>> services;

		for (const std::string &name : context.Host->Names()) {
			const size_t dot = name.find('.');
			if (dot == std::string::npos) {
				flat.push_back(name);
				continue;
			}

			const std::string service = name.substr(0, dot);
			const auto found = std::find_if(services.begin(), services.end(), [&service](const auto &entry) {
				return entry.first == service;
			});

			if (found == services.end()) {
				services.emplace_back(service, std::vector<std::string>{name});
			} else {
				found->second.push_back(name);
			}
		}

		// The host's own table, for everything that is not a service.
		lua_newtable(state);
		for (const std::string &name : flat) {
			lua_pushlightuserdata(state, &context);
			lua_pushstring(state, name.c_str());

			// No service table: a plain call never drops its first argument.
			lua_pushnil(state);

			lua_pushcclosure(state, HostCall, name.c_str(), 3);
			lua_setfield(state, -2, name.c_str());
		}

		// **The table itself is frozen too**, so a plugin cannot replace one of
		// its own host functions with something that looks like it — which
		// matters because a *second* chunk in the same VM would then be calling
		// the first one's replacement.
		lua_setreadonly(state, -1, 1);

		const std::string global(context.Host->GlobalName());
		lua_setfield(state, -2, global.c_str());

		for (const auto &[service, methods] : services) {
			lua_newtable(state);

			for (const std::string &name : methods) {
				const std::string method = name.substr(service.size() + 1);

				lua_pushlightuserdata(state, &context);
				lua_pushstring(state, name.c_str());

				// The service table, so a colon call can be told from a dot
				// call — see `HostCall`. Three deep, because the two upvalues
				// above are already on the stack.
				lua_pushvalue(state, -3);

				lua_pushcclosure(state, HostCall, method.c_str(), 3);
				lua_setfield(state, -2, method.c_str());
			}

			lua_setreadonly(state, -1, 1);
			lua_setfield(state, -2, service.c_str());
		}

		if (frozen) {
			lua_setreadonly(state, -1, 1);
		}
		lua_pop(state, 1);
	}

	bool CallHostCallback(lua_State *state, HostCallback callback, HostArguments arguments) {
		LuauContext &context = ContextOf(state);

		const auto found = context.HostCallbacks.find(callback.Id);
		if (found == context.HostCallbacks.end()) {
			return false;
		}

		lua_getref(state, found->second);
		for (const HostValue &argument : arguments) {
			PushHostValue(state, argument);
		}

		if (lua_pcall(state, static_cast<int>(arguments.size()), 0, 0) != LUA_OK) {
			const char *message = lua_tostring(state, -1);
			ENGINE_ERROR("host callback failed: {}", message != nullptr ? message : "unknown");
			lua_pop(state, 1);
			return false;
		}
		return true;
	}

	void ReleaseHostCallback(lua_State *state, HostCallback callback) {
		LuauContext &context = ContextOf(state);

		const auto found = context.HostCallbacks.find(callback.Id);
		if (found == context.HostCallbacks.end()) {
			return;
		}

		lua_unref(state, found->second);
		context.HostCallbacks.erase(found);
	}
}
