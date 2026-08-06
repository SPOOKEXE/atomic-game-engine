#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>

namespace engine::scene {

	void CapturePreviousTransforms(ecs::Store &store) {
		// **Takes `parallel::Jobs::DEFAULT_GRAIN`, and nobody has checked whether
		// it should.** That is a dispatch floor of 32,768 rows. This note exists
		// because the neighbouring loops stopped taking the default and this one
		// has no reading to move on, which is a different thing from agreeing
		// with it.
		//
		// The case for a smaller grain: a row here is a whole `core::CFrame`,
		// the same 28 bytes `physics::IntegrateMotion` carries, and
		// `physics/Integrate.hpp` measures that body's crossover at 8,000 rows
		// against a grain of 1024.
		//
		// The case against: the resemblance stops at the bytes. That body does a
		// quaternion product, a normalise and a reciprocal square root; this one
		// does a 28-byte copy and no arithmetic whatsoever, which is the shape
		// `DEFAULT_GRAIN` *was* calibrated for. `parallel/AGENTS.md` records that
		// the bandwidth-bound case tops out near 1.3x however many threads it is
		// handed, and the three-float-add body it measured that on crosses near
		// 262,144 rows. A straight column copy may well cross there too, in which
		// case a grain of 1024 would dispatch this thirty times too early.
		//
		// **So it keeps the default until somebody measures it.** A guessed
		// constant under a confident comment is worse than a default under an
		// honest one. `engine.ecs.bench.iteration` is where the answer comes
		// from: this body, laddered either side of 8192 and again either side of
		// 262,144, with a grain small enough to force the handover at both.
		store.EachParallel<PreviousTransform, const Transform>(
			[](ecs::Entity, PreviousTransform &previous, const Transform &transform) {
				previous.Frame = transform.Frame;
			}
		);
	}
}
