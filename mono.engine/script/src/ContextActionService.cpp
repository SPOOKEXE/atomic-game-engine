// `ContextActionService`'s surface, in neither language.
//
// **Split out of `InputServices.cpp` at v0.18** alongside `UserInputService.cpp`
// and `LuauInput.cpp` - one file held both services and the Luau pump that
// drives them, so a description with no VM in it was compiled against `<lua.h>`
// and neither service could be found by its own name.
//
// **What this adds over `UserInputService` is a priority stack**, and that is
// the whole of it. Polling `IsKeyDown` is fine until two systems want E - a door
// and a vehicle - and then the question is which one gets it. A bound action is
// a claim on a key with a number attached, and the highest claim wins.
//
// **The stack is shared and the callables are not.** What a claim *is* and which
// handler a press reaches are ordering rules a recording depends on, so they
// live in `ActionStack` and both languages read them; a Luau handler is a
// registry ref and a JavaScript one an index into `JsContext::Callables`, and
// both cross as a `CallbackRef` nothing shared may interpret. Which claim a key
// reaches is `ActionStack::ClaimingFrom`, walked by `PumpInput` and
// `PumpJsInput` alike.
//
// @tier L9 · shared
// @since v0.16

#include <engine/core/Name.hpp>
#include <engine/scene/Input.hpp>
#include <engine/script/Actions.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using scene::KeyCode;

		// The stack lives on the VM rather than on the world, because a bound
		// action holds a callable: it cannot cross a snapshot, cannot be
		// replicated, and dies with the runtime that registered it. A world
		// resource holding one would be a resource that cannot be serialised.

		// `ContextActionService:BindActionAtPriority(name, handler, touchButton, priority, ...keys)`
		//
		// The five-argument form is the one that says what it means; the
		// four-argument `BindAction` below is it with a priority of zero.
		//
		// @param call     The call.
		// @param priority Where the priority sits, or a negative index for the
		//        form that has none.
		// @param firstKey Where the variadic key list starts.
		void BindAtPriority(ScriptCall &call, int priority, size_t firstKey) {
			BoundAction action;
			action.Name = call.AsString(0);
			action.Callback = call.RetainCallback(1);
			action.Priority = priority;

			// **The whole tail rather than up to the first nil**, which is what
			// `ScriptCall::Arguments` exists for: Roblox's `BindAction` also
			// takes `Enum.UserInputType` members and this engine binds keys only,
			// so a value this cannot read is walked past rather than refused.
			for (size_t index = firstKey; index < call.Arguments(); index++) {
				core::Name member;
				if (!call.ReadEnum(index, core::Name("KeyCode"), member)) {
					continue;
				}

				const KeyCode key = scene::KeyFromName(member.Text());
				if (key != KeyCode::Unknown) {
					action.Keys.push_back(static_cast<uint16_t>(key));
				}
			}

			// **The replaced handler is released and not leaked.** Rebinding a
			// name replaces it rather than stacking a second - Roblox's
			// behaviour, and the one that makes a script safe to run twice - so
			// the callable the old row held has to go back to the VM that minted
			// it.
			CallbackRef replaced = 0;
			if (call.Actions().Bind(std::move(action), replaced)) {
				call.ReleaseCallback(replaced);
			}
		}

		void BindAction(ScriptCall &call) {
			// `(name, handler, createTouchButton, ...keys)` - the touch button is
			// accepted and ignored, because there is no touch surface. Accepted
			// rather than refused so a Roblox script runs unchanged.
			BindAtPriority(call, 0, 3);
		}

		void BindActionAtPriority(ScriptCall &call) {
			// `(name, handler, createTouchButton, priority, ...keys)`
			BindAtPriority(call, static_cast<int>(call.AsNumber(3)), 4);
		}

		void UnbindAction(ScriptCall &call) {
			CallbackRef released = 0;
			if (call.Actions().Unbind(call.AsString(0), released)) {
				call.ReleaseCallback(released);
			}
		}

		void UnbindAllActions(ScriptCall &call) {
			std::vector<CallbackRef> released;
			call.Actions().UnbindAll(released);
			for (const CallbackRef callback : released) {
				call.ReleaseCallback(callback);
			}
		}

		// --- the two that report ----------------------------------------------
		//
		// **Luau's alone until v0.16, and what closed them was a report rather
		// than a rewrite.** Each answers a record holding a list of
		// `Enum.KeyCode` members, and `ScriptCall` can return a `ScriptValue`,
		// which has no tag for an `EnumItem` - and must not gain one, because a
		// `ScriptValue` crosses a world and `Codec.hpp` is a wire format. The
		// answer is the one `UserInputService`'s `InputObject` already proved:
		// `BoundActionReport` is the fact, each adapter builds its own record and
		// its own `EnumItem`s, and neither the wire format nor the interface
		// learns what a `ContextActionService` is.

		// One action's report, with `stackOrder` inverted.
		//
		// @param actions  The whole stack, so a position becomes an order.
		// @param position The action's zero-based place in it.
		BoundActionReport
		ReportOf(std::span<const BoundAction> actions, size_t position, std::vector<core::Name> &keys) {
			const BoundAction &action = actions[position];

			keys.clear();
			keys.reserve(action.Keys.size());
			for (const uint16_t ordinal : action.Keys) {
				keys.emplace_back(scene::Describe(static_cast<KeyCode>(ordinal)));
			}

			BoundActionReport report;
			report.Name = action.Name;
			report.Keys = keys;
			report.Priority = action.Priority;
			report.StackOrder = static_cast<int>(actions.size() - position);
			return report;
		}

		// `ContextActionService:GetBoundActionInfo(name)` -> record or nil
		void GetBoundActionInfo(ScriptCall &call) {
			const std::string name = call.AsString(0);
			const ActionStack &stack = call.Actions();
			const std::span<const BoundAction> actions = stack.Entries();

			const BoundAction *found = stack.Find(name);
			if (found == nullptr) {
				// **Nil for an unbound name, not an empty record.** Roblox's
				// answer, and the one `if info then` reads correctly - an empty
				// table is truthy and would report every name as bound.
				call.ReturnNil();
				return;
			}

			// **`stackOrder` needs the position and `Find` answers the row**, so
			// the span is what turns one into the other - a pointer into the
			// stack's own vector, subtracted from its start.
			std::vector<core::Name> keys;
			call.ReturnBoundAction(ReportOf(actions, static_cast<size_t>(found - actions.data()), keys));
		}

		// `ContextActionService:GetAllBoundActionInfo()` -> { [name]: record }
		void GetAllBoundActionInfo(ScriptCall &call) {
			const std::span<const BoundAction> actions = call.Actions().Entries();

			// **The key names outlive the reports, which is why they are one
			// vector per action rather than one reused buffer.** A
			// `BoundActionReport` holds a `span` into this storage and the whole
			// list is handed over at once, so a buffer the next action overwrote
			// would leave every report but the last pointing at the wrong keys.
			std::vector<std::vector<core::Name>> keys(actions.size());
			std::vector<BoundActionReport> reports;
			reports.reserve(actions.size());

			for (size_t index = 0; index < actions.size(); index++) {
				reports.push_back(ReportOf(actions, index, keys[index]));
			}

			call.ReturnBoundActions(reports);
		}
	}

	const ServiceSurface &ContextActionServiceSurface() {
		// A plain table, because this one is **methods only** - and a method is
		// exactly the case `GETIMPORT` is correct for: the closure never
		// changes, so caching it is what the optimisation is for. Only a mutable
		// *property* is broken by it. That is also why this one could cross to
		// JavaScript and `UserInputService` above it could not.
		static constexpr std::array<ServiceMethod, 6> METHODS{{
			{"BindAction", BindAction},
			{"BindActionAtPriority", BindActionAtPriority},
			{"UnbindAction", UnbindAction},
			{"UnbindAllActions", UnbindAllActions},
			{"GetBoundActionInfo", GetBoundActionInfo},
			{"GetAllBoundActionInfo", GetAllBoundActionInfo},
		}};

		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "ContextActionService";
			surface.Methods = METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
