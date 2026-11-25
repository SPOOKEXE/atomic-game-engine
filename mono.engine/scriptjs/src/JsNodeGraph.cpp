// The `NodeGraph` global: the embedded prelude run over `NodeGraphService`.
//
// Everything a script graph does goes through the neutral service rows; this
// file only evaluates the JavaScript half of the object wrapper and installs it.

#include "JsBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/script/NodeGraphService.hpp>

#include <string>

namespace engine::script {

	void InstallJsNodeGraph(JSContext *context, JSValueConst global) {
		JSValue service = JS_GetPropertyStr(context, global, "NodeGraphService");
		if (JS_IsUndefined(service)) {
			// A context without the service gets no library.
			return;
		}
		const std::string source(NodeGraphJavaScriptPrelude());
		JSValue factory = JS_Eval(context, source.c_str(), source.size(), "<NodeGraph>", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(factory)) {
			JSValue thrown = JS_GetException(context);
			const char *text = JS_ToCString(context, thrown);
			ENGINE_ERROR("NodeGraph prelude did not load: {}", text != nullptr ? text : "?");
			JS_FreeCString(context, text);
			JS_FreeValue(context, thrown);
			JS_FreeValue(context, service);
			return;
		}
		JSValue library = JS_Call(context, factory, JS_UNDEFINED, 1, &service);
		JS_FreeValue(context, factory);
		JS_FreeValue(context, service);
		if (JS_IsException(library)) {
			JSValue thrown = JS_GetException(context);
			const char *text = JS_ToCString(context, thrown);
			ENGINE_ERROR("NodeGraph prelude failed: {}", text != nullptr ? text : "?");
			JS_FreeCString(context, text);
			JS_FreeValue(context, thrown);
			return;
		}
		JS_SetPropertyStr(context, global, "NodeGraph", library);
	}
}
