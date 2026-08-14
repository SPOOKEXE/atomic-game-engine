// What a JavaScript script is handed when a key it claimed moves.
//
// **This file exists because binding a handler that never fires is worse than
// not binding one.** `ContextActionService` became a service both languages
// reach at v0.16 — its stack is `ActionStack` and its handler crosses as a
// `CallbackRef` — and this language had no input pump at all, so a JavaScript
// `BindAction` would have taken a function and forgotten it. That is the failure
// `script/AGENTS.md` names twice over: `InputChanged` was reachable and silent
// for six versions, and `CollectionService` has no `GetInstanceAddedSignal`
// precisely so that it cannot happen again.
//
// **`UserInputService`'s six signals are here too, since that service crossed.**
// They were deliberately absent while it was Luau's alone — a signal surface for
// a service this language cannot reach is a signal nothing can connect — and
// leaving them out once it *had* crossed would have been the same failure in the
// other direction: `InputBegan` reachable, connectable and silent, which is the
// state this module names twice as reading like a broken engine rather than an
// unfinished one. So this pump is now `PumpInput`'s twin in full, over the same
// edges in the same order.
//
// **The report is shared and only the wrapper is here.** `Actions.hpp` holds
// `InputReport` and all four builders — `KeyReport`, `ButtonReport`,
// `MotionReport` and `WheelReport` — because two pumps building a report each is
// two answers to what a frame did; `LuauInput.cpp` builds the Luau userdata
// and this builds the JavaScript object. Both are the same five fields over the
// same fact, which is what makes a handler ported between the two languages read
// the same.
//
// @tier L9 · shared
// @since v0.16

#include "Actions.hpp"
#include "JsBindings.hpp"

#include <engine/scene/Input.hpp>

#include <string>
#include <utility>

namespace engine::script {

	namespace {
		using scene::InputState;
		using scene::KeyCode;

		// The report behind one `InputObject`.
		const InputReport *ReportOf(JSContext *context, JSValueConst value) {
			return static_cast<const InputReport *>(
				JS_GetOpaque2(context, value, JsOf(context).InputObjectClass)
			);
		}

		// The five fields Roblox's `InputObject` has that a script reads.
		//
		// **Accessors on the prototype rather than properties on the object**,
		// which is `JsContext::Prototypes`'s rule one door along: a frame that
		// produced twenty edges would otherwise define a hundred closures over
		// the same five names.
		//
		// **Read-only, and the object is sealed.** An input report is a fact
		// about a frame rather than a document a handler edits and passes on,
		// which is the same reason the Luau half has no `__newindex`.
		JSValue InputKeyCode(JSContext *context, JSValueConst self) {
			const InputReport *report = ReportOf(context, self);
			return report == nullptr
					   ? JS_EXCEPTION
					   : MakeJsEnumItem(
							 context, core::Name("KeyCode"), core::Name(scene::Describe(report->Key))
						 );
		}

		JSValue InputUserInputType(JSContext *context, JSValueConst self) {
			const InputReport *report = ReportOf(context, self);
			return report == nullptr
					   ? JS_EXCEPTION
					   : MakeJsEnumItem(
							 context, core::Name("UserInputType"), core::Name(scene::Describe(report->Source))
						 );
		}

		JSValue InputUserInputState(JSContext *context, JSValueConst self) {
			const InputReport *report = ReportOf(context, self);
			return report == nullptr ? JS_EXCEPTION
									 : MakeJsEnumItem(context, core::Name("UserInputState"), report->State);
		}

		JSValue InputPosition(JSContext *context, JSValueConst self) {
			const InputReport *report = ReportOf(context, self);
			return report == nullptr ? JS_EXCEPTION : MakeVector3(context, report->Position);
		}

		JSValue InputDelta(JSContext *context, JSValueConst self) {
			const InputReport *report = ReportOf(context, self);
			return report == nullptr ? JS_EXCEPTION : MakeVector3(context, report->Delta);
		}

		// **`JS_GetAnyOpaque` and not `JS_GetOpaque(value, 0)`** — a finaliser
		// cannot capture, so it has no way to name the class id it was registered
		// under, and `JS_GetOpaque` answers null whenever the id does not match.
		// Passing zero frees nothing, which is the leak `JsBindings.cpp` records
		// finding on every `Vector3` a script constructed.
		void FreeInputReport(JSRuntime *, JSValue value) {
			JSClassID id = 0;
			delete static_cast<InputReport *>(JS_GetAnyOpaque(value, &id));
		}

		// The message a handler threw, or a fallback.
		std::string ThrownBy(JSContext *context, const char *fallback) {
			JSValue thrown = JS_GetException(context);

			std::string message = fallback;
			if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
				message = text;
				JS_FreeCString(context, text);
			}

			JS_FreeValue(context, thrown);
			return message;
		}

		// Calls everything connected to one of `UserInputService`'s signals.
		//
		// **The name filter is the whole of what distinguishes the five**: they
		// share one `SignalKind` with a `NULL_ENTITY` subject and are told apart
		// by what the connection carries, exactly as `GetAttributeChangedSignal`
		// reuses `PropertyChanged`. `FireJsSignal` cannot be used because it does
		// not filter, which is the same reason `PumpJsChanges` walks the table
		// itself.
		//
		// **One argument list for every handler on one edge**, built once by the
		// caller and freed once. An `InputObject` is read-only and sealed, so
		// nothing a handler does to it can reach the next.
		//
		// **The caller builds the arguments where the Luau twin passes a
		// callable, and that is the one place the two pumps genuinely differ.**
		// Luau's arguments live on a stack and have to be pushed per call;
		// JavaScript's are values, so building them once is both simpler and
		// cheaper. What is shared is the rule underneath — which signals get what
		// — and that lives in the two pumps' identical call sites.
		//
		// @param context   The VM.
		// @param signal    Which signal, by name.
		// @param arguments What to hand over. Empty for the two focus signals,
		//        which Roblox calls with nothing. Owned by the caller.
		// @return The first handler failure, or empty.
		std::string FireInputSignal(JSContext *context, core::Name signal, std::span<JSValue> arguments) {
			JsContext &bound = JsOf(context);

			std::string firstError;

			// **What a `Once` connection spends.** `SignalTable::Fire` retires
			// nothing itself — only the VM knows how to release a callable — so
			// every direct caller owes this, and the Luau twin of this function
			// did not pay it either. `FireJsSignal` in `JsSurface.cpp` is what
			// this is copied from.
			std::vector<ConnectionId> spent;

			bound.Signals.Fire(
				SignalKind::PropertyChanged, ecs::NULL_ENTITY, [&](const Connection &connection) {
					if (connection.Property != signal) {
						return;
					}

					JSValue result = JS_Call(
						context,
						Held(context, connection.Callback),
						JS_UNDEFINED,
						static_cast<int>(arguments.size()),
						arguments.data()
					);

					// **Every connection runs even when one throws**, which is
					// `FireJsSignal`'s rule and the reason it is stated there: a
					// handler that threw once would otherwise silently stop
					// everything registered after it.
					if (JS_IsException(result) && firstError.empty()) {
						firstError = ThrownBy(context, "an input listener failed");
					}
					JS_FreeValue(context, result);

					if (connection.Once) {
						spent.push_back(connection.Id);
					}
				}
			);

			// After the fire and not inside it, so a `Once` handler that connects
			// another one does not have the list compacted under the walk still
			// reading it. Only connections this walk called are in `spent`, so a
			// `Once` on a different input signal is untouched.
			for (const ConnectionId id : spent) {
				CallbackRef released = 0;
				if (bound.Signals.Disconnect(id, released)) {
					Release(context, released);
				}
			}

			return firstError;
		}

		// Hands one key edge down the claims until one of them sinks it.
		//
		// **`ActionStack::ClaimingFrom` decides the order and
		// `Enum.ContextActionResult` decides how far the walk gets**, so this pump
		// and the Luau one are the same rule rather than two that agree today.
		// `LuauInput.cpp`'s twin carries the argument for why anything that is
		// not `Pass` sinks, and for why a handler that threw sinks too.
		//
		// @return The first handler's failure, or empty.
		std::string RunBoundActions(JSContext *context, KeyCode key, const InputReport &report) {
			JsContext &bound = JsOf(context);

			size_t position = 0;
			while (true) {
				const BoundAction *action = bound.Actions.ClaimingFrom(static_cast<uint16_t>(key), position);
				if (action == nullptr) {
					return {};
				}

				JSValue arguments[3];
				arguments[0] = JS_NewStringLen(context, action->Name.data(), action->Name.size());
				arguments[1] = MakeJsEnumItem(context, core::Name("UserInputState"), report.State);
				arguments[2] = MakeJsInputObject(context, report);

				JSValue result =
					JS_Call(context, Held(context, action->Callback), JS_UNDEFINED, 3, arguments);

				std::string failure;
				bool pass = false;
				if (JS_IsException(result)) {
					failure = ThrownBy(context, "a bound action failed");
				} else {
					core::Name answer;
					pass = ReadJsEnumValue(context, result, core::Name("ContextActionResult"), answer) &&
						   answer == core::Name("Pass");
				}

				JS_FreeValue(context, result);
				for (JSValue &argument : arguments) {
					JS_FreeValue(context, argument);
				}

				if (!pass) {
					return failure;
				}
			}
		}
	}

	void InstallJsInputObject(JSContext *context) {
		JsContext &bound = JsOf(context);

		static const JSClassDef definition = {"InputObject", FreeInputReport, nullptr, nullptr, nullptr};

		JSRuntime *runtime = JS_GetRuntime(context);
		JS_NewClassID(runtime, &bound.InputObjectClass);
		JS_NewClass(runtime, bound.InputObjectClass, &definition);

		static const JSCFunctionListEntry MEMBERS[] = {
			JS_CGETSET_DEF("KeyCode", InputKeyCode, nullptr),
			JS_CGETSET_DEF("UserInputType", InputUserInputType, nullptr),
			JS_CGETSET_DEF("UserInputState", InputUserInputState, nullptr),
			JS_CGETSET_DEF("Position", InputPosition, nullptr),
			JS_CGETSET_DEF("Delta", InputDelta, nullptr),
		};

		JSValue proto = JS_NewObject(context);
		JS_SetPropertyFunctionList(context, proto, MEMBERS, static_cast<int>(std::size(MEMBERS)));
		JS_SetClassProto(context, bound.InputObjectClass, proto);
	}

	JSValue MakeJsInputObject(JSContext *context, const InputReport &report) {
		JSValue object = JS_NewObjectClass(context, static_cast<int>(JsOf(context).InputObjectClass));
		if (JS_IsException(object)) {
			return object;
		}

		JS_SetOpaque(object, new InputReport(report));
		JS_PreventExtensions(context, object);
		return object;
	}

	std::string PumpJsInput(JSContext *context, std::span<const gui::GuiEvent> interface) {
		JsContext &bound = JsOf(context);
		if (bound.World == nullptr) {
			return {};
		}

		// **Null is the ordinary case on a server**, not an error: a headless
		// world ticks the same scripts and finds nothing pressed.
		const InputState *input = bound.World->Resource<InputState>();
		if (input == nullptr) {
			return {};
		}

		std::string firstError;
		const auto note = [&](std::string message) {
			if (firstError.empty() && !message.empty()) {
				firstError = std::move(message);
			}
		};

		// Decided once for the whole beat, exactly as the Luau pump does — see
		// `InterfaceHasPointer` for what it means and why a key is never
		// processed.
		const bool processed = InterfaceHasPointer(interface);

		// One edge's arguments: the `InputObject` and Roblox's
		// `gameProcessedEvent`. Built and freed per edge, because the object
		// carries that edge's report.
		const auto fireReport = [&](core::Name signal, const InputReport &report) {
			JSValue arguments[2];
			arguments[0] = MakeJsInputObject(context, report);
			arguments[1] = JS_NewBool(context, processed && IsPointerReport(report));

			note(FireInputSignal(context, signal, arguments));

			for (JSValue &argument : arguments) {
				JS_FreeValue(context, argument);
			}
		};

		// **Focus first, before the releases it caused.**
		// `input::Translator::ReleaseAll` clears every key on the frame focus is
		// lost, so this pump is also the one that reports them released — and a
		// listener that hears "you lost focus" after "W came up" has to guess
		// which of the two explains the other.
		if (input->WasFocusGained()) {
			note(FireInputSignal(context, core::Name("WindowFocused"), {}));
		}
		if (input->WasFocusLost()) {
			note(FireInputSignal(context, core::Name("WindowFocusReleased"), {}));
		}

		// Before the edges, because it explains them — the Luau pump's order.
		if (input->WasLastSourceChanged()) {
			JSValue member = MakeJsEnumItem(
				context, core::Name("UserInputType"), core::Name(scene::Describe(input->LastSource))
			);
			note(FireInputSignal(context, core::Name("LastInputTypeChanged"), std::span(&member, 1)));
			JS_FreeValue(context, member);
		}

		// **Edges only**, which is what a bound action means — `Begin` and `End`
		// are the two calls a handler gets, and a third every frame would make
		// every action a repeat-rate question. The same walk the Luau pump does,
		// over the same `KeyCode` order, so two worlds scripted in the two
		// languages see one sequence.
		for (size_t index = 0; index < static_cast<size_t>(KeyCode::Count); index++) {
			const auto key = static_cast<KeyCode>(index);
			const bool began = input->WasKeyPressed(key);
			if (!began && !input->WasKeyReleased(key)) {
				continue;
			}

			const InputReport report = KeyReport(key, began);
			note(RunBoundActions(context, key, report));

			// The service's own signals, after the bound actions. **Not gated by
			// them**: `InputBegan` is a report of what happened and a claim on a
			// key does not stop it having happened, which is Roblox's split and
			// the one that lets a debug overlay watch every key regardless.
			fireReport(core::Name(began ? "InputBegan" : "InputEnded"), report);
		}

		// **The buttons.** No bound actions here, for the reason
		// `BoundAction::Keys` gives: the stack holds `scene::KeyCode` ordinals
		// and binding a button would need the vector to say which of the two
		// spaces each entry is in.
		for (size_t index = 0; index < static_cast<size_t>(scene::MouseButton::Count); index++) {
			const auto button = static_cast<scene::MouseButton>(index);
			const bool began = input->WasButtonPressed(button);
			if (!began && !input->WasButtonReleased(button)) {
				continue;
			}

			const InputReport report = ButtonReport(*input, button, began);
			fireReport(core::Name(began ? "InputBegan" : "InputEnded"), report);
		}

		// **`InputChanged`, over the two things this engine can report
		// changing.** Exact compares against zero and not an epsilon: both fields
		// are written by the translator as a sum of integer SDL deltas and cleared
		// to a literal zero every frame, so a compare answers exactly the question
		// — and an epsilon would swallow the one-pixel move a slow drag is made
		// of.
		if (input->MouseDelta.X != 0.0f || input->MouseDelta.Y != 0.0f) {
			fireReport(core::Name("InputChanged"), MotionReport(*input));
		}

		if (input->WheelDelta != 0.0f) {
			fireReport(core::Name("InputChanged"), WheelReport(*input));
		}

		return firstError;
	}
}
