#pragma once

// How much arrived content one frame is willing to turn into something usable.
//
// **A delivery client answers in bursts, and the cost is on the taking end.**
// `Pump` finishes whatever the sources managed this frame, and a scene that
// named forty meshes at once gets forty answers in the frame the origin caught
// up. Taking one is cheap; what follows it is not - an `.amesh` has to be
// decoded and uploaded to the GPU before anything can draw it - so a loop that
// drains every completed request the moment it notices them spends a third of a
// second in one frame and the editor stops responding while somebody's model
// set lands.
//
// That is the whole problem this solves: not *when* content arrives, which the
// client already handles, but how much of it a frame agrees to absorb.
//
// **Bytes rather than a count**, because the work is in the geometry and not in
// the number of files. Forty icons and one character model are not the same
// frame, and a budget of "eight assets" is either uselessly small for the first
// or uselessly large for the second.
//
// **What does not fit is deferred, not dropped.** An arrival a frame cannot
// take is still an arrival - the caller puts it back in its pending list, which
// is exactly what it already does for a request that has not finished, so
// nothing had to learn a new state to wait in.
//
// **The first arrival of a frame is always admitted**, even one larger than the
// entire budget. An asset that can never fit under the limit would otherwise
// wait for ever behind it, checked and deferred every frame while the thing it
// belongs to stays invisible. One long frame beats never loading it.
//
// **Here rather than in the two loops that use it.** The studio and the client
// each drain content in their frame loop and neither is reachable from a test,
// so a rule written twice in two untestable places is a rule the build does not
// check - see rule 6. The counters exist for the same reason: `Deferred` is how
// a test, or somebody reading a profile, can tell "the budget did nothing" from
// "the budget is doing its job" without instrumenting the loop.
//
// @tier L11 · shared

#include <cstddef>

namespace engine::delivery {

	// A frame's allowance for absorbing delivered content.
	//
	// Used once per frame: `Begin`, then `Admits` before each arrival and
	// `Spend` after each one taken.
	//
	// @since v0.12
	class IntakeBudget {
	  public:
		// What one frame will absorb by default.
		//
		// Two megabytes is roughly one decode-and-upload of a detailed mesh, so
		// a frame does at least one asset's worth of real work and rarely much
		// more than that. It is a pacing number rather than a measured limit:
		// the point is that the cost per frame is bounded and roughly constant,
		// not that this particular figure is optimal on any one machine.
		static constexpr size_t FRAME_BYTES = 2u * 1024u * 1024u;

		// Builds a budget.
		//
		// @param bytes What each frame may absorb.
		explicit IntakeBudget(size_t bytes = FRAME_BYTES) : Limit(bytes) {}

		// Starts a frame, forgetting what the last one spent.
		void Begin() {
			Bytes = 0;
			Taken = 0;
			Held = 0;
		}

		// Whether another arrival may be absorbed this frame.
		//
		// **True for the first arrival however large the last one was**, which
		// is what admits an asset bigger than the whole budget: nothing has been
		// spent yet, so nothing is over.
		//
		// @return `true` when the caller should take the next completed request.
		bool Admits() const {
			return Bytes < Limit;
		}

		// Records an absorbed asset.
		//
		// @param bytes Its size.
		void Spend(size_t bytes) {
			Bytes += bytes;
			++Taken;
		}

		// Records an arrival this frame refused, which the caller keeps pending.
		void Defer() {
			++Held;
		}

		// What this frame has absorbed.
		size_t Spent() const {
			return Bytes;
		}

		// How many assets this frame absorbed.
		size_t Absorbed() const {
			return Taken;
		}

		// How many ready arrivals this frame left for the next one.
		size_t Deferred() const {
			return Held;
		}

		// What each frame may absorb.
		size_t Allowance() const {
			return Limit;
		}

	  private:
		size_t Limit = FRAME_BYTES;
		size_t Bytes = 0;
		size_t Taken = 0;
		size_t Held = 0;
	};
}
