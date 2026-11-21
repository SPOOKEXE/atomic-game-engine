#include "Integration.hpp"
#include "WorkBudgetInternal.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/scene/WorldMembership.hpp>
#include <engine/scene/Components.hpp>

namespace engine::physics {

	void IntegrateMotion(ecs::Store &store) {
		const auto movingCount = store.Query<const scene::Transform, const scene::Motion>().Without<scene::OutsideWorld>().Count();
		const PhysicsWorkBudget *budget = PhysicsWorkBudgetOf(store);
		// A windowed tick integrates only its admitted bodies; see `WindowTurn`.
		const bool windowed = budget != nullptr && budget->InProgress && budget->Windowed;
		ChargePhysicsWork(
			store, PhysicsWorkStage::Integration, windowed ? budget->Active.size() : movingCount
		);
		ENGINE_PROFILE_CAT("physics.integrate", core::ProfileCategory::Physics);

		// The fixed step delta, read from the world. A system takes no float
		// argument precisely so that nobody can hand it a frame time. This is
		// the world's tick unless the world runs physics at a rate of its own,
		// in which case it is that rate's step - see `Clock.hpp`.
		const float delta = PhysicsStepSeconds(store);

		// `const Motion`, so the query cannot be widened into one that writes
		// velocity - that is the solver's job and it runs in another phase. No
		// `RigidBody` term: this loads no mass, which is the reason the two
		// components are separate at all.
		//
		// Nothing marks the transforms changed. `Store::MarkAllChanged` claims
		// *every* row carrying `Transform` moved, including the anchored ones,
		// and `SyncBroadphase` reads those same stamps to decide whether static
		// geometry has to be re-indexed - so over-reporting here would rebuild
		// the static index every tick, forever. A consumer that needs a
		// replication delta out of an integrated world marks it in its own
		// publish step, where the claim belongs.
		store.Query<scene::Transform, const scene::Motion>().Without<scene::OutsideWorld>().EachParallel(
			[delta, budget](ecs::Entity entity, scene::Transform &transform, const scene::Motion &motion) {
				if (PhysicsBodyParked(budget, entity)) return;
				IntegrateOne(transform, motion, delta);
			},
			INTEGRATE_GRAIN
		);
	}
}
