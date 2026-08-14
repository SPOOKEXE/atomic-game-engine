// The `Tween` handle, in JavaScript.
//
// **The handle and never the service.** `TweenService.cpp` describes `GetValue`
// and `Create` once and both VMs install them; what is here is the object
// `Create` answers with — a registered class whose payload is the entity that
// names the tween, and the four members on its prototype. `LuauTween.cpp` is the
// same thing said in the other language, and `Tweens.hpp` says why this half is
// per language at all.
//
// **`tween.Completed` is an accessor and `tween.Play()` is a method**, which is
// the same split the instance surface makes — a signal is a value, and a method
// call in this language is a dot rather than a colon.
//
// @tier L9 · shared

#include "JsBindings.hpp"

#include <iterator>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;

		// The class payload, made and freed the way every other datatype's is.
		//@{
		void FreeTween(JSRuntime *, JSValue value) {
			JSClassID id = 0;
			delete static_cast<Entity *>(JS_GetAnyOpaque(value, &id));
		}

		Entity *TweenOf(JSContext *context, JSValueConst value) {
			return static_cast<Entity *>(JS_GetOpaque2(context, value, JsOf(context).TweenClass));
		}
		//@}

		// Drops a tween nothing holds any more: its connections, then its row.
		//
		// The Luau half's, and the same split: only a VM knows what a
		// `CallbackRef` means, which is why `TweenService:Create` asks for the
		// same thing through `ScriptCall::ForgetSubject` rather than doing it.
		void ReleaseTween(JSContext *context, JsContext &bound, Entity tween) {
			std::vector<CallbackRef> released;
			bound.Signals.DropSubject(tween, released);
			for (const CallbackRef reference : released) {
				Release(context, reference);
			}
			bound.World->Destroy(tween);
		}

		// `tween.Play()`, `tween.Pause()`, `tween.Cancel()`
		//@{
		JSValue TweenPlay(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity *tween = TweenOf(context, self);
			if (tween == nullptr) {
				return JS_EXCEPTION;
			}

			JsContext &bound = JsOf(context);
			return JS_NewBool(context, bound.Tweens.Play(*bound.World, *tween));
		}

		JSValue TweenPause(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity *tween = TweenOf(context, self);
			if (tween == nullptr) {
				return JS_EXCEPTION;
			}
			return JS_NewBool(context, JsOf(context).Tweens.Pause(*tween));
		}

		JSValue TweenCancel(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity *tween = TweenOf(context, self);
			if (tween == nullptr) {
				return JS_EXCEPTION;
			}
			return JS_NewBool(context, JsOf(context).Tweens.Cancel(*tween));
		}
		//@}

		// `tween.Completed`
		JSValue TweenCompleted(JSContext *context, JSValueConst self) {
			const Entity *tween = TweenOf(context, self);
			if (tween == nullptr) {
				return JS_EXCEPTION;
			}
			return MakeJsSignal(context, SignalKind::TweenCompleted, *tween);
		}

		// `tween.Equals(other)`
		//
		// **Here for the reason `Instance.Equals` exists**: this language has no
		// operator overloading and `===` on two objects is identity, so two
		// handles to one tween are never `===`. The Luau half answers the same
		// question through `__eq`.
		JSValue TweenEquals(JSContext *context, JSValueConst self, int count, JSValueConst *argv) {
			const Entity *left = TweenOf(context, self);
			if (left == nullptr) {
				return JS_EXCEPTION;
			}

			const Entity *right = count > 0 ? TweenOf(context, argv[0]) : nullptr;
			if (right == nullptr) {
				// Not an exception: asking whether a tween equals something that
				// is not one is a fair question with a boring answer.
				JS_FreeValue(context, JS_GetException(context));
				return JS_NewBool(context, false);
			}
			return JS_NewBool(context, *left == *right);
		}

		const JSCFunctionListEntry TWEEN_MEMBERS[] = {
			JS_CFUNC_DEF("Play", 0, TweenPlay),
			JS_CFUNC_DEF("Pause", 0, TweenPause),
			JS_CFUNC_DEF("Cancel", 0, TweenCancel),
			JS_CFUNC_DEF("Equals", 1, TweenEquals),
			JS_CGETSET_DEF("Completed", TweenCompleted, nullptr),
		};
	}

	JSValue MakeJsTween(JSContext *context, ecs::Entity tween) {
		JSValue object = JS_NewObjectClass(context, static_cast<int>(JsOf(context).TweenClass));
		if (JS_IsException(object)) {
			return object;
		}

		JS_SetOpaque(object, new ecs::Entity(tween));
		return object;
	}

	void OpenJsTweenHandle(JSContext *context) {
		JsContext &bound = JsOf(context);

		// **Static, and that is why the finaliser is a free function rather than
		// a lambda**: `JSClassDef` holds the pointer for as long as the runtime
		// does, so a local would dangle the moment this returned.
		static JSClassDef definition = {"Tween", FreeTween, nullptr, nullptr, nullptr};

		JSRuntime *runtime = JS_GetRuntime(context);
		JS_NewClassID(runtime, &bound.TweenClass);
		JS_NewClass(runtime, bound.TweenClass, &definition);

		JSValue prototype = JS_NewObject(context);
		JS_SetPropertyFunctionList(
			context, prototype, TWEEN_MEMBERS, static_cast<int>(std::size(TWEEN_MEMBERS))
		);
		JS_SetClassProto(context, bound.TweenClass, prototype);
	}

	std::string PumpJsTweens(JSContext *context, float delta) {
		JsContext &bound = JsOf(context);

		// Collected and then fired, exactly as the Luau half does and for the
		// same reason: a `Completed` handler may cancel the tween it was told
		// about, and `TweenTable::Advance` is walking the list that names it.
		std::vector<ecs::Entity> completed;
		std::vector<ecs::Entity> dropped;
		bound.Tweens.Advance(*bound.World, delta, completed, dropped);

		std::string firstError;
		for (const ecs::Entity tween : completed) {
			const std::string failed = FireJsSignal(context, SignalKind::TweenCompleted, tween, 0, nullptr);
			if (firstError.empty()) {
				firstError = failed;
			}
		}

		for (const ecs::Entity tween : dropped) {
			ReleaseTween(context, bound, tween);
		}
		return firstError;
	}
}
