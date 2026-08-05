#include "Integration.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/scene/Components.hpp>

namespace engine::physics {

	void IntegrateMotion(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.integrate", core::ProfileCategory::Physics);

		// The fixed tick delta, read from the world. A system takes no float
		// argument precisely so that nobody can hand it a frame time.
		const float delta = store.Time().Delta;

		// `const Motion`, so the query cannot be widened into one that writes
		// velocity — that is the solver's job and it runs in another phase. No
		// `RigidBody` term: this loads no mass, which is the reason the two
		// components are separate at all.
		//
		// Nothing marks the transforms changed. `Store::MarkAllChanged` claims
		// *every* row carrying `Transform` moved, including the anchored ones,
		// and `SyncBroadphase` reads those same stamps to decide whether static
		// geometry has to be re-indexed — so over-reporting here would rebuild
		// the static index every tick, forever. A consumer that needs a
		// replication delta out of an integrated world marks it in its own
		// publish step, where the claim belongs.
		store.EachParallel<scene::Transform, const scene::Motion>(
			[delta](ecs::Entity, scene::Transform &transform, const scene::Motion &motion) {
				IntegrateOne(transform, motion, delta);
			},
			INTEGRATE_GRAIN
		);
	}
}
