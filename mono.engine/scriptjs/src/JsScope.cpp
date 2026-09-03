// Native cleanup scopes for JavaScript.

#include "JsBindings.hpp"

#include <engine/core/Log.hpp>

#include <string>
#include <vector>

namespace engine::script {

	namespace {
		struct ScopePayload {
			ScopeHandle Handle;
			CallbackRef ErrorHandler = 0;
			std::vector<ScopeItem> Items;
		};

		ScopePayload *ScopeOf(JSContext *context, JSValueConst value) {
			return static_cast<ScopePayload *>(JS_GetOpaque2(context, value, JsOf(context).ScopeClass));
		}

		void FreeScope(JSRuntime *, JSValue value) {
			JSClassID id = 0;
			delete static_cast<ScopePayload *>(JS_GetAnyOpaque(value, &id));
		}

		std::string Exception(JSContext *context) {
			JSValue thrown = JS_GetException(context);
			const char *text = JS_ToCString(context, thrown);
			std::string message = text != nullptr ? text : "non-string error";
			if (text != nullptr) {
				JS_FreeCString(context, text);
			}
			JS_FreeValue(context, thrown);
			return message;
		}

		void Report(JSContext *context, ScopePayload &scope, const char *operation, const std::string &message) {
			if (scope.ErrorHandler != 0) {
				JSValue argument = JS_NewStringLen(context, message.data(), message.size());
				JSValue result = JS_Call(context, Held(context, scope.ErrorHandler), JS_UNDEFINED, 1, &argument);
				JS_FreeValue(context, argument);
				if (JS_IsException(result)) {
					ENGINE_WARN("[script] Scope error handler failed: {}", Exception(context));
					return;
				}
				JS_FreeValue(context, result);
				return;
			}
			ENGINE_WARN("[script] Scope {} failed: {}", operation, message);
		}

		void Dispose(JSContext *context, ScopePayload &scope, std::vector<ScopeItem> &items) {
			static constexpr const char *METHODS[] = {"Destroy", "Disconnect", "Cancel"};
			for (auto item = items.rbegin(); item != items.rend(); ++item) {
				JSValueConst held = Held(context, static_cast<CallbackRef>(item->Value));
				if (item->Kind == ScopeItemKind::Callback) {
					JSValue result = JS_Call(context, held, JS_UNDEFINED, 0, nullptr);
					if (JS_IsException(result)) {
						Report(context, scope, "callback cleanup", Exception(context));
					} else {
						JS_FreeValue(context, result);
					}
				} else {
					bool invoked = false;
					for (const char *method : METHODS) {
						JSValue callable = JS_GetPropertyStr(context, held, method);
						if (!JS_IsFunction(context, callable)) {
							JS_FreeValue(context, callable);
							continue;
						}
						JSValue result = JS_Call(context, callable, held, 0, nullptr);
						JS_FreeValue(context, callable);
						if (JS_IsException(result)) {
							Report(context, scope, method, Exception(context));
						} else {
							JS_FreeValue(context, result);
						}
						invoked = true;
						break;
					}
					if (!invoked) {
						ENGINE_WARN("[script] Scope ignored an object without Destroy, Disconnect, or Cancel");
					}
				}
				Release(context, static_cast<CallbackRef>(item->Value));
			}
			scope.Items.clear();
		}

		JSValue ScopeAdd(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			ScopePayload *scope = ScopeOf(context, self);
			if (scope == nullptr) return JS_EXCEPTION;
			if (argc < 1) return JS_ThrowTypeError(context, "Scope.Add expects a resource");
			if (!JsOf(context).Scopes.IsAlive(scope->Handle)) return JS_ThrowTypeError(context, "Scope is destroyed");
			const CallbackRef reference = Retain(context, argv[0]);
			const ScopeItem item{JS_IsFunction(context, argv[0]) ? ScopeItemKind::Callback : ScopeItemKind::Custom,
			                     static_cast<uint64_t>(reference)};
			JsOf(context).Scopes.Add(scope->Handle, item);
			scope->Items.push_back(item);
			return JS_DupValue(context, self);
		}

		JSValue ScopeAddBulk(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			if (argc < 1) return JS_ThrowTypeError(context, "Scope.AddBulk expects resources");
			if (!JS_IsArray(argv[0])) {
				for (int index = 0; index < argc; index++) {
					JSValue result = ScopeAdd(context, self, 1, &argv[index]);
					if (JS_IsException(result)) return result;
					JS_FreeValue(context, result);
				}
				return JS_DupValue(context, self);
			}
			uint32_t count = 0;
			JSValue length = JS_GetPropertyStr(context, argv[0], "length");
			const int converted = JS_ToUint32(context, &count, length);
			JS_FreeValue(context, length);
			if (converted != 0) return JS_EXCEPTION;
			for (uint32_t index = 0; index < count; index++) {
				JSValue item = JS_GetPropertyUint32(context, argv[0], index);
				JSValue result = ScopeAdd(context, self, 1, &item);
				JS_FreeValue(context, item);
				if (JS_IsException(result)) return result;
				JS_FreeValue(context, result);
			}
			return JS_DupValue(context, self);
		}

		JSValue ScopeRemove(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			ScopePayload *scope = ScopeOf(context, self);
			if (scope == nullptr) return JS_EXCEPTION;
			if (argc < 1) return JS_FALSE;
			for (auto item = scope->Items.begin(); item != scope->Items.end(); ++item) {
				if (JS_IsStrictEqual(context, Held(context, static_cast<CallbackRef>(item->Value)), argv[0])) {
					JsOf(context).Scopes.Remove(scope->Handle, *item);
					Release(context, static_cast<CallbackRef>(item->Value));
					scope->Items.erase(item);
					return JS_TRUE;
				}
			}
			return JS_FALSE;
		}

		JSValue ScopeClean(JSContext *context, JSValueConst self, int, JSValueConst *) {
			ScopePayload *scope = ScopeOf(context, self);
			if (scope == nullptr) return JS_EXCEPTION;
			std::vector<ScopeItem> items;
			if (JsOf(context).Scopes.Clean(scope->Handle, items)) Dispose(context, *scope, items);
			return JS_TRUE;
		}

		JSValue ScopeDestroy(JSContext *context, JSValueConst self, int, JSValueConst *) {
			ScopePayload *scope = ScopeOf(context, self);
			if (scope == nullptr) return JS_EXCEPTION;
			std::vector<ScopeItem> items;
			if (JsOf(context).Scopes.Destroy(scope->Handle, items)) Dispose(context, *scope, items);
			return JS_TRUE;
		}

		JSValue ScopeCount(JSContext *context, JSValueConst self, int, JSValueConst *) {
			ScopePayload *scope = ScopeOf(context, self);
			return scope == nullptr ? JS_EXCEPTION : JS_NewInt64(context, JsOf(context).Scopes.Count(scope->Handle));
		}

		JSValue ScopeIsAlive(JSContext *context, JSValueConst self, int, JSValueConst *) {
			ScopePayload *scope = ScopeOf(context, self);
			return scope == nullptr ? JS_EXCEPTION : JS_NewBool(context, JsOf(context).Scopes.IsAlive(scope->Handle));
		}

		JSValue ScopeSetErrorHandler(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			ScopePayload *scope = ScopeOf(context, self);
			if (scope == nullptr) return JS_EXCEPTION;
			if (argc < 1 || !JS_IsFunction(context, argv[0])) return JS_ThrowTypeError(context, "Scope.SetErrorHandler expects a function");
			if (scope->ErrorHandler != 0) Release(context, scope->ErrorHandler);
			scope->ErrorHandler = Retain(context, argv[0]);
			return JS_DupValue(context, self);
		}

		JSValue ScopeNew(JSContext *context, JSValueConst, int, JSValueConst *) {
			JSValue object = JS_NewObjectClass(context, JsOf(context).ScopeClass);
			if (!JS_IsException(object)) {
				auto *scope = new ScopePayload();
				scope->Handle = JsOf(context).Scopes.Create();
				JS_SetOpaque(object, scope);
			}
			return object;
		}
	}

	void OpenJsScopes(JSContext *context) {
		JsContext &bound = JsOf(context);
		static JSClassDef definition = {"Scope", FreeScope, nullptr, nullptr, nullptr};
		JS_NewClassID(JS_GetRuntime(context), &bound.ScopeClass);
		JS_NewClass(JS_GetRuntime(context), bound.ScopeClass, &definition);
		JSValue prototype = JS_NewObject(context);
		static const JSCFunctionListEntry methods[] = {
			JS_CFUNC_DEF("Add", 1, ScopeAdd), JS_CFUNC_DEF("AddBulk", 1, ScopeAddBulk), JS_CFUNC_DEF("Remove", 1, ScopeRemove), JS_CFUNC_DEF("Clean", 0, ScopeClean), JS_CFUNC_DEF("Destroy", 0, ScopeDestroy),
			JS_CFUNC_DEF("Count", 0, ScopeCount), JS_CFUNC_DEF("IsAlive", 0, ScopeIsAlive), JS_CFUNC_DEF("SetErrorHandler", 1, ScopeSetErrorHandler),
		};
		JS_SetPropertyFunctionList(context, prototype, methods, 8);
		JS_SetClassProto(context, bound.ScopeClass, prototype);
		JSValue scope = JS_NewObject(context);
		JS_SetPropertyStr(context, scope, "new", JS_NewCFunction(context, ScopeNew, "new", 0));
		JSValue global = JS_GetGlobalObject(context);
		JS_SetPropertyStr(context, global, "Scope", scope);
		JS_FreeValue(context, global);
	}
}
