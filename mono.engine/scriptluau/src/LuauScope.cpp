// Native cleanup scopes for Luau.
//
// The shared ScopeTable records only lifetime and order. Registry references
// and calls into Luau stay here, which keeps the VM boundary explicit.

#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>

#include <lualib.h>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		struct ScopePayload {
			ScopeHandle Handle;
			CallbackRef ErrorHandler = 0;
			std::vector<ScopeItem> Items;
		};

		ScopePayload &CheckScope(lua_State *state, int index) {
			void *memory = lua_touserdatatagged(state, index, TAG_SCOPE);
			if (memory == nullptr) {
				luaL_typeerrorL(state, index, "Scope");
			}
			return *static_cast<ScopePayload *>(memory);
		}

		void ReportFailure(lua_State *state, ScopePayload &scope, const char *operation, std::string message) {
			if (scope.ErrorHandler != 0) {
				lua_getref(state, scope.ErrorHandler);
				lua_pushlstring(state, message.data(), message.size());
				if (lua_pcall(state, 1, 0, 0) == LUA_OK) {
					return;
				}
				const char *handlerError = lua_tostring(state, -1);
				ENGINE_WARN(
					"[script] Scope error handler failed while handling {}: {}",
					operation,
					handlerError != nullptr ? handlerError : "non-string error"
				);
				lua_pop(state, 1);
			}
			ENGINE_WARN("[script] Scope {} failed: {}", operation, message);
		}

		void RunCallback(lua_State *state, ScopePayload &scope, CallbackRef reference) {
			lua_getref(state, reference);
			if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
				const char *message = lua_tostring(state, -1);
				ReportFailure(state, scope, "callback cleanup", message != nullptr ? message : "non-string error");
				lua_pop(state, 1);
			}
			lua_unref(state, reference);
		}

		void CancelThread(lua_State *state, LuauContext &context, CallbackRef reference) {
			lua_getref(state, reference);
			if (lua_isthread(state, -1)) {
				lua_State *thread = lua_tothread(state, -1);
				const auto found = context.Threads.find(thread);
				if (found != context.Threads.end() && context.Tasks.Cancel(found->second)) {
					context.WaitTicks.erase(thread);
					context.PendingArguments.erase(thread);
					lua_unref(state, found->second);
					context.Threads.erase(found);
				}
			}
			lua_pop(state, 1);
			lua_unref(state, reference);
		}

		void RunObject(lua_State *state, ScopePayload &scope, CallbackRef reference) {
			static constexpr const char *METHODS[] = {"Destroy", "Disconnect", "Cancel"};
			lua_getref(state, reference);
			for (const char *method : METHODS) {
				lua_getfield(state, -1, method);
				if (lua_isnil(state, -1)) {
					lua_pop(state, 1);
					continue;
				}
				lua_pushvalue(state, -2);
				if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
					const char *message = lua_tostring(state, -1);
					ReportFailure(state, scope, method, message != nullptr ? message : "non-string error");
					lua_pop(state, 1);
				}
				lua_pop(state, 1);
				lua_unref(state, reference);
				return;
			}
			lua_pop(state, 1);
			lua_unref(state, reference);
			ENGINE_WARN("[script] Scope ignored an object without Destroy, Disconnect, or Cancel");
		}

		void Dispose(lua_State *state, ScopePayload &scope, std::vector<ScopeItem> &items) {
			LuauContext &context = ContextOf(state);
			for (auto item = items.rbegin(); item != items.rend(); ++item) {
				switch (item->Kind) {
				case ScopeItemKind::Callback:
					RunCallback(state, scope, static_cast<CallbackRef>(item->Value));
					break;
				case ScopeItemKind::Task:
					CancelThread(state, context, static_cast<CallbackRef>(item->Value));
					break;
				case ScopeItemKind::Custom:
					RunObject(state, scope, static_cast<CallbackRef>(item->Value));
					break;
				default:
					break;
				}
			}
		}

		ScopeItem RetainItem(lua_State *state, int index) {
			ScopeItem item;
			if (lua_isfunction(state, index)) {
				item.Kind = ScopeItemKind::Callback;
			} else if (lua_isthread(state, index)) {
				item.Kind = ScopeItemKind::Task;
			} else {
				item.Kind = ScopeItemKind::Custom;
			}
			lua_pushvalue(state, index);
			item.Value = static_cast<uint64_t>(lua_ref(state, -1));
			lua_pop(state, 1);
			return item;
		}

		int ScopeAdd(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			if (!ContextOf(state).Scopes.IsAlive(scope.Handle)) {
				luaL_errorL(state, "Scope is destroyed");
				return 0;
			}
			luaL_checkany(state, 2);
			const ScopeItem item = RetainItem(state, 2);
			ContextOf(state).Scopes.Add(scope.Handle, item);
			scope.Items.push_back(item);
			lua_pushvalue(state, 1);
			return 1;
		}

		int ScopeAddBulk(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			if (!ContextOf(state).Scopes.IsAlive(scope.Handle)) {
				luaL_errorL(state, "Scope is destroyed");
				return 0;
			}
			luaL_checktype(state, 2, LUA_TTABLE);
			const int count = lua_objlen(state, 2);
			for (int index = 1; index <= count; index++) {
				lua_rawgeti(state, 2, index);
				const ScopeItem item = RetainItem(state, -1);
				lua_pop(state, 1);
				ContextOf(state).Scopes.Add(scope.Handle, item);
				scope.Items.push_back(item);
			}
			lua_pushvalue(state, 1);
			return 1;
		}

		int ScopeRemove(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			for (auto item = scope.Items.begin(); item != scope.Items.end(); ++item) {
				lua_getref(state, static_cast<CallbackRef>(item->Value));
				const bool same = lua_rawequal(state, -1, 2) != 0;
				lua_pop(state, 1);
				if (!same) continue;
				ContextOf(state).Scopes.Remove(scope.Handle, *item);
				lua_unref(state, static_cast<CallbackRef>(item->Value));
				scope.Items.erase(item);
				lua_pushboolean(state, 1);
				return 1;
			}
			lua_pushboolean(state, 0);
			return 1;
		}

		int ScopeClean(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			std::vector<ScopeItem> items;
			if (ContextOf(state).Scopes.Clean(scope.Handle, items)) {
				Dispose(state, scope, items);
				scope.Items.clear();
			}
			lua_pushboolean(state, 1);
			return 1;
		}

		int ScopeDestroy(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			std::vector<ScopeItem> items;
			if (ContextOf(state).Scopes.Destroy(scope.Handle, items)) {
				Dispose(state, scope, items);
				scope.Items.clear();
			}
			lua_pushboolean(state, 1);
			return 1;
		}

		int ScopeIsAlive(lua_State *state) {
			const ScopePayload &scope = CheckScope(state, 1);
			lua_pushboolean(state, ContextOf(state).Scopes.IsAlive(scope.Handle));
			return 1;
		}

		int ScopeCount(lua_State *state) {
			const ScopePayload &scope = CheckScope(state, 1);
			lua_pushinteger(state, static_cast<int>(ContextOf(state).Scopes.Count(scope.Handle)));
			return 1;
		}

		int ScopeSetErrorHandler(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);
			if (scope.ErrorHandler != 0) {
				lua_unref(state, scope.ErrorHandler);
			}
			lua_pushvalue(state, 2);
			scope.ErrorHandler = lua_ref(state, -1);
			lua_pop(state, 1);
			lua_pushvalue(state, 1);
			return 1;
		}

		int ScopeGc(lua_State *state) {
			ScopePayload &scope = CheckScope(state, 1);
			std::vector<ScopeItem> items;
			if (ContextOf(state).Scopes.Destroy(scope.Handle, items)) {
				Dispose(state, scope, items);
				scope.Items.clear();
			}
			if (scope.ErrorHandler != 0) {
				lua_unref(state, scope.ErrorHandler);
				scope.ErrorHandler = 0;
			}
			return 0;
		}

		int ScopeNew(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			void *memory = lua_newuserdatatagged(state, sizeof(ScopePayload), TAG_SCOPE);
			auto *scope = static_cast<ScopePayload *>(memory);
			*scope = ScopePayload{};
			scope->Handle = context.Scopes.Create();
			luaL_getmetatable(state, "Scope");
			lua_setmetatable(state, -2);
			return 1;
		}
	}

	void OpenScopes(lua_State *state) {
		LuauContext &context = ContextOf(state);
		static constexpr LuauServiceMethod METHODS[] = {
			{"Add", ScopeAdd}, {"AddBulk", ScopeAddBulk}, {"Remove", ScopeRemove}, {"Clean", ScopeClean}, {"Destroy", ScopeDestroy},
			{"IsAlive", ScopeIsAlive}, {"Count", ScopeCount}, {"SetErrorHandler", ScopeSetErrorHandler},
		};

		lua_newtable(state);
		for (const LuauServiceMethod &method : METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.scope.methods");

		luaL_newmetatable(state, "Scope");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, [](lua_State *inner) {
			lua_getfield(inner, LUA_REGISTRYINDEX, "engine.scope.methods");
			lua_pushvalue(inner, 2);
			lua_rawget(inner, -2);
			return 1;
		}, "__index", 1);
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, ScopeGc, "__gc");
		lua_setfield(state, -2, "__gc");
		lua_pop(state, 1);

		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, ScopeNew, "new", 1);
		lua_setfield(state, -2, "new");
		lua_setglobal(state, "Scope");
	}
}
