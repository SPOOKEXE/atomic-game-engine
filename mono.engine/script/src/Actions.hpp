#pragma once

// What `ContextActionService` has claimed, and the report a handler is handed.
//
// **The stack is shared and the callables are not**, which is `Signals.hpp`'s
// arrangement one door along and for the same reason. What a bound action *is* —
// a name, the keys it claims, a priority, and the rule that the highest claim
// wins and the rest never see the press — names no VM, and the order two actions
// at one priority fire in is a thing a recording depends on. Two hand-written
// copies of that would agree until the first time one was fixed.
//
// A Luau handler is a registry ref and a JavaScript one an index into
// `JsContext::Callables`, so a callable crosses as an opaque `CallbackRef`
// exactly as a connection's does. Nothing here may interpret one.
//
// **The stack lives on a VM's context and never on the world.** A bound action
// holds a callable: it cannot cross a snapshot, cannot be replicated, and dies
// with the runtime that registered it. A world resource holding one would be a
// resource that cannot be serialised.
//
// @tier L9 · shared
// @since v0.16

#include "Signals.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Input.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// One input event, as a script sees it.
	//
	// **Roblox's `InputObject`, and what this engine used to hand nobody.** Both
	// languages build a value of their own from one of these — a tagged userdata
	// on the Luau side and an object of a registered class on the JavaScript one
	// — which is the same split `ScriptCall::ReturnSignal` is on: the *fact* is
	// one thing and only its wrapper is two.
	//
	// Trivially copyable on purpose: the Luau half placement-news it into
	// userdata memory, exactly as every value type in `Values.cpp` is.
	struct InputReport {
		// Where the pointer was, in pixels from the top-left of the window,
		// with the wheel's notches in Z.
		//
		// **Roblox's placement exactly**, including the odd-looking Z: a wheel
		// notch has nowhere else to go in a `Vector3`, and a script migrated
		// from a Roblox place reads `input.Position.Z` for it.
		core::Vector3 Position;

		// How far it moved since the previous frame, in the same space.
		core::Vector3 Delta;

		// `Begin`, `Change` or `End`, as an `Enum.UserInputState` member.
		//
		// **A name rather than an ordinal**, because the member list is
		// registered in `scene/Part.cpp` and an ordinal here would be a second
		// statement of its order — the kind that agrees until somebody inserts a
		// member.
		core::Name State;

		// The key, or `Unknown` for anything that is not one. Roblox reports
		// `Enum.KeyCode.Unknown` for a mouse event and so does this.
		scene::KeyCode Key = scene::KeyCode::Unknown;

		// Where it came from.
		scene::InputSource Source = scene::InputSource::Keyboard;
	};

	// A report for one key edge.
	//
	// Position and delta are zero, as Roblox's are: a keyboard event has no
	// place on the screen.
	//
	// **Shared because both pumps build one.** The mouse, motion and wheel
	// reports stay in `InputServices.cpp` beside `UserInputService`'s signals,
	// which is the only thing that produces them — a bound action is keys only,
	// for the reason `RunBoundActions` gives.
	//
	// @param key   Which key moved.
	// @param began Whether it went down.
	// @return The report a handler is given.
	InputReport KeyReport(scene::KeyCode key, bool began);

	// One action bound through `ContextActionService`.
	//
	// @since v0.10
	struct BoundAction {
		// What the script called it. Unbinding is by this name.
		std::string Name;

		// The keys it claims, as `scene::KeyCode` ordinals.
		//
		// **Ordinals rather than the enum**, which costs one cast at each end
		// and keeps the stack's rules readable without the input vocabulary
		// beside them.
		std::vector<uint16_t> Keys;

		// The VM's name for the handler.
		CallbackRef Callback = 0;

		// Higher wins. Roblox's default is zero and so is this.
		int Priority = 0;
	};

	// Every action one VM has bound, highest priority first.
	//
	// @since v0.16
	class ActionStack {
	  public:
		// Binds an action, replacing one of the same name.
		//
		// **Rebinding a name replaces it rather than stacking a second.**
		// Roblox's behaviour, and the one that makes a script safe to run twice:
		// a reload that bound the same action again would otherwise fire its
		// handler twice per press, forever.
		//
		// **Sorted here rather than searched at press time**, because binding is
		// rare where pressing is not — and stably, so two actions at one
		// priority fire in bind order. That is the only tie-break that is
		// reproducible; an unstable sort would make which one wins depend on the
		// allocator.
		//
		// @param action   What to bind. Moved from.
		// @param released Filled in with the callable a replaced action held.
		// @return `true` when something was replaced and `released` is set.
		bool Bind(BoundAction action, CallbackRef &released);

		// Drops one action by name.
		//
		// @param name     What it was bound as.
		// @param released Filled in with the callable it held.
		// @return `false` when nothing of that name is bound.
		bool Unbind(std::string_view name, CallbackRef &released);

		// Drops every action.
		//
		// @param released Appended with every callable the stack held.
		void UnbindAll(std::vector<CallbackRef> &released);

		// The highest-priority action claiming a key, or null.
		//
		// **The first claim wins and the rest never see it**, which is the whole
		// reason `ContextActionService` exists beside polling.
		//
		// @param key A `scene::KeyCode` ordinal.
		// @return The action, or null when nothing claims it.
		const BoundAction *Claiming(uint16_t key) const;

		// One action by name, or null.
		const BoundAction *Find(std::string_view name) const;

		// Every action, highest priority first.
		std::span<const BoundAction> Entries() const;

	  private:
		std::vector<BoundAction> Bound;
	};
}
