#include <engine/world/Lifecycle.hpp>

namespace engine::world {

	LifecycleAction DecideLifecycle(const LifecycleInputs &inputs) {
		// **A suspended world only ever wakes for its inbox.** Occupancy cannot
		// wake it, because nothing can occupy a world that is not running —
		// somebody arriving is a teleport, and a teleport is a message.
		if (inputs.State == WorldState::Suspended) {
			return inputs.InboxWaiting ? LifecycleAction::Resume : LifecycleAction::Leave;
		}

		// **Only `Active` is a candidate for suspension.** `Faulted` is the
		// supervisor's, and suspending it would take a world out of the
		// quarantine that is trying to restore it. `Idle` is already the
		// reduced-rate state, and a host that runs both would be deciding
		// between them here — which is a policy nobody has needed yet, and
		// guessing at it would put a second answer beside `SetState`.
		if (inputs.State != WorldState::Active) {
			return LifecycleAction::Leave;
		}

		// Occupancy outranks the clock, and it is checked before it so that the
		// caller may leave `IdleSeconds` stale for an occupied world — which is
		// what "touch it and stop counting" means in practice.
		if (inputs.Occupied) {
			return LifecycleAction::Leave;
		}

		// **A world with nobody in it is not always a world with nothing
		// happening.** NPCs, an economy, a countdown between rounds — see
		// `IdleSleep`. This is asked for rather than reached by leaving a number
		// high, so that "this world costs a machine forever" is visible.
		if (inputs.Sleep == IdleSleep::Never) {
			return LifecycleAction::Leave;
		}

		// `Immediate` skips the clock entirely rather than comparing against a
		// zero limit, so a host does not have to set two fields consistently to
		// get one behaviour.
		if (inputs.Sleep == IdleSleep::Timeout) {
			// **Clamped here rather than trusted from the caller**, because a
			// ceiling every host has to remember is a ceiling one of them will
			// not. See `MAXIMUM_IDLE_LIMIT_SECONDS`.
			const double limit =
				inputs.IdleLimit < MAXIMUM_IDLE_LIMIT_SECONDS ? inputs.IdleLimit : MAXIMUM_IDLE_LIMIT_SECONDS;
			if (inputs.IdleSeconds < limit) {
				return LifecycleAction::Leave;
			}
		}

		// **Last, so the reason a world survived is the interesting one.** Put
		// first, every world in a one-world universe would report "not idle"
		// rather than "kept because it is the only one".
		if (inputs.LastWorld) {
			return LifecycleAction::Leave;
		}

		return LifecycleAction::Suspend;
	}
}
