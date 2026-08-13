// `TweenService` and the `Tween` it hands back, in JavaScript.
//
// The twin of `TweenService.cpp`, and deliberately the same shape: the service
// is a plain object of two functions, the tween is a class whose payload is the
// entity that names it, and everything about *what a tween is* stays in
// `Tweens.hpp` where both languages read one copy of it.
//
// **`tween.Completed` is an accessor and `tween.Play()` is a method**, which is
// the same split the instance surface makes — a signal is a value, and a method
// call in this language is a dot rather than a colon.
//
// @tier L9 · shared

#include "JsBindings.hpp"

#include <engine/ecs/Classes.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::PropertyDescriptor;

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

		// The property a script named, or null when the instance has no such
		// scriptable property. The Luau half's `FindGoal`, and the same rule:
		// a non-scriptable property is not found rather than found and refused.
		const PropertyDescriptor *FindGoal(const ecs::Store &store, Entity instance, std::string_view name) {
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Spelling == name) {
					return property.Scriptable ? &property : nullptr;
				}
			}
			return nullptr;
		}

		// Reads the goal object, refusing anything a tween cannot drive by name.
		//
		// **Reports through `ok` rather than throwing part way**, because the
		// keys have to be freed before anything unwinds: `JS_GetOwnPropertyNames`
		// hands back reference-counted atoms, and a throw in the middle of the
		// walk would strand every one it had taken. So the names are copied out
		// first, the table is freed, and only then is anything refused.
		std::vector<TweenGoal>
		ReadGoals(JSContext *context, JSValueConst goals, ecs::Store &store, Entity target, bool &ok) {
			ok = false;

			std::vector<TweenGoal> found;
			if (!JS_IsObject(goals)) {
				JS_ThrowTypeError(context, "TweenService.Create: expected an object of goals");
				return found;
			}

			std::vector<std::string> names;
			{
				JSPropertyEnum *properties = nullptr;
				uint32_t count = 0;
				if (JS_GetOwnPropertyNames(
						context, &properties, &count, goals, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
					) != 0) {
					JS_ThrowTypeError(context, "TweenService.Create: could not read the goals");
					return found;
				}

				names.reserve(count);
				for (uint32_t index = 0; index < count; index++) {
					if (const char *text = JS_AtomToCString(context, properties[index].atom);
						text != nullptr) {
						names.emplace_back(text);
						JS_FreeCString(context, text);
					}
					JS_FreeAtom(context, properties[index].atom);
				}
				js_free(context, properties);
			}

			for (const std::string &name : names) {
				const PropertyDescriptor *property = FindGoal(store, target, name);
				if (property == nullptr) {
					JS_ThrowTypeError(
						context, "TweenService.Create: '%s' is not a property of this instance", name.c_str()
					);
					return found;
				}
				if (!property->Writable) {
					JS_ThrowTypeError(context, "TweenService.Create: '%s' cannot be assigned", name.c_str());
					return found;
				}
				if (!Interpolable(property->Type)) {
					JS_ThrowTypeError(
						context,
						"TweenService.Create: '%s' is a %s, which has no midpoint to interpolate through",
						name.c_str(),
						ecs::Describe(property->Type)
					);
					return found;
				}

				TweenGoal goal;
				goal.Property = property->Name;
				goal.Type = property->Type;
				goal.Size = property->Size;

				JSValue value = JS_GetPropertyStr(context, goals, name.c_str());
				const bool read = FromJsValue(context, value, property->Type, property->EnumName, goal.Goal);
				JS_FreeValue(context, value);
				if (!read) {
					JS_ThrowTypeError(
						context, "TweenService.Create: could not read the goal for '%s'", name.c_str()
					);
					return found;
				}

				found.push_back(goal);
			}

			// Sorted by spelling, exactly as the Luau half sorts: two properties
			// of one instance may project onto one component, so which of them
			// lands last is observable — and the two languages must not disagree
			// about it either.
			std::sort(found.begin(), found.end(), [](const TweenGoal &left, const TweenGoal &right) {
				return left.Property.Text() < right.Property.Text();
			});

			ok = true;
			return found;
		}

		// Drops a tween nothing holds any more: its connections, then its row.
		void ReleaseTween(JSContext *context, JsContext &bound, Entity tween) {
			std::vector<CallbackRef> released;
			bound.Signals.DropSubject(tween, released);
			for (const CallbackRef reference : released) {
				Release(context, reference);
			}
			bound.World->Destroy(tween);
		}

		// --- the service ------------------------------------------------------

		// `TweenService.GetValue(alpha, easingStyle, easingDirection)`
		JSValue GetValue(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			double alpha = 0.0;
			if (count < 3 || JS_ToFloat64(context, &alpha, argv[0]) != 0) {
				return JS_ThrowTypeError(context, "TweenService.GetValue: expected an alpha and two enums");
			}

			core::Name style;
			if (!ReadJsEnumValue(context, argv[1], core::Name("EasingStyle"), style)) {
				return JS_ThrowTypeError(context, "TweenService.GetValue: expected an Enum.EasingStyle");
			}

			core::Name direction;
			if (!ReadJsEnumValue(context, argv[2], core::Name("EasingDirection"), direction)) {
				return JS_ThrowTypeError(context, "TweenService.GetValue: expected an Enum.EasingDirection");
			}

			return JS_NewFloat64(
				context,
				core::TweenInfo::Ease(
					static_cast<float>(alpha), EasingStyleOf(style), EasingDirectionOf(direction)
				)
			);
		}

		// `TweenService.Create(instance, tweenInfo, goals)`
		JSValue Create(JSContext *context, JSValueConst, int count, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			if (count < 3) {
				return JS_ThrowTypeError(
					context, "TweenService.Create: expected an instance, a TweenInfo and goals"
				);
			}

			const Entity target = JsEntityOf(context, argv[0]);
			if (target == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "TweenService.Create: expected an Instance");
			}

			const auto *info =
				static_cast<const core::TweenInfo *>(JS_GetOpaque(argv[1], bound.TweenInfoClass));
			if (info == nullptr) {
				return JS_ThrowTypeError(context, "TweenService.Create: expected a TweenInfo");
			}

			bool ok = false;
			std::vector<TweenGoal> goals = ReadGoals(context, argv[2], *bound.World, target, ok);
			if (!ok) {
				return JS_EXCEPTION;
			}

			std::vector<Entity> dropped;
			const Entity tween = bound.Tweens.Create(*bound.World, target, *info, std::move(goals), dropped);
			for (const Entity stale : dropped) {
				ReleaseTween(context, bound, stale);
			}

			if (tween == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(
					context,
					"TweenService.Create: this world already holds %d running tweens",
					static_cast<int>(TweenTable::MAXIMUM)
				);
			}

			return MakeJsTween(context, tween);
		}

		// --- the tween --------------------------------------------------------

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

	void OpenJsTweenService(JSContext *context, JSValueConst global) {
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

		JSValue service = JS_NewObject(context);
		JS_SetPropertyStr(context, service, "GetValue", JS_NewCFunction(context, GetValue, "GetValue", 3));
		JS_SetPropertyStr(context, service, "Create", JS_NewCFunction(context, Create, "Create", 3));
		JS_SetPropertyStr(context, global, "TweenService", service);
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
