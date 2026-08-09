#pragma once

// The client's side of `world::ViewChannel`: N views in, one frame out.
//
// **A view is produced somewhere and drawn somewhere else.** Reading a world's
// draw list straight out of its store at render time works exactly as long as
// there is one world in this process producing at this process's frame rate.
// The moment either stops being true — a second world, a world in a host, a
// mirror rendering from a surface camera — the renderer is reaching into
// something that is being written by somebody with a different clock.
//
// A channel per view is the answer already built at L4. This is the consumer:
// it publishes on behalf of each world it tracks, takes the newest frame from
// each, and hands the renderer one list. A view whose producer stalled keeps
// its last frame and is counted as stale rather than disappearing, because a
// world flickering out of existence for one frame is worse than a world one
// frame behind.
//
// **Worlds are separate spaces.** Nothing says two worlds' coordinates mean the
// same thing — `world/AGENTS.md` is explicit that worlds do not share anything
// but a bus. So the compositor *places* them: each view after the first is
// offset along X by a spacing the caller chooses. That is a compositor's
// decision to make and not a simulation's, which is exactly why it is here and
// not in a system.
//
// @client

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/world/ViewChannel.hpp>
#include <engine/world/World.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace client {

	// One tracked view: its channel, and the last frame taken from it.
	//
	// @since v0.2
	struct ViewState {
		// The world this views.
		engine::core::Name World;

		// Its handle in the universe.
		engine::world::WorldId Id;

		// The last header taken.
		engine::world::ViewHeader Header;

		// How many instances the last frame carried.
		size_t Instances = 0;

		// Whether the last `Compose` found something new.
		bool Fresh = false;

		// Consecutive composes with nothing new. A number that climbs is a
		// producer that has stopped, which looks identical to a still scene
		// unless somebody counts it.
		uint64_t Stale = 0;
	};

	// N view channels, and the one draw list they compose into.
	//
	// @since v0.2
	class Compositor {
	  public:
		// Starts tracking a world's view.
		//
		// The channel is reserved at this size, so publishing a list that fits
		// never allocates — which is what keeps a producer off the allocator
		// inside its own render phase.
		//
		// **A starting size, not a ceiling.** A world that outgrows it grows
		// the channel in steps rather than being refused: the number a caller
		// has at this point is a guess about a world a script has not built
		// yet, and a guess that stops a world being drawn is worse than an
		// occasional reallocation. `Publish` says when it grows.
		//
		// @param id               The world's handle.
		// @param world            Its name, which is what crosses.
		// @param maximumInstances The draw list size to reserve for.
		void Track(engine::world::WorldId id, engine::core::Name world, size_t maximumInstances);

		// Publishes one world's view.
		//
		// Called from inside that world's `Enter`, after its `PreRender` phase
		// has filled the draw list.
		//
		// @param id      The world publishing.
		// @param frame   Where the view was taken from.
		// @param camera  The live camera's field of view and clipping distances.
		// @param list    What it drew.
		// @param tick    The tick that produced it.
		// @param alpha   The interpolation position it used.
		// @return `false` only for an untracked world. A list larger than the
		//         channel grows it rather than being refused.
		bool Publish(
			engine::world::WorldId id,
			const engine::core::CFrame &frame,
			const engine::scene::Camera &camera,
			std::span<const engine::scene::DrawInstance> list,
			uint64_t tick,
			float alpha
		);

		// Takes the newest frame from every channel and rebuilds the draw list.
		//
		// @param spacing World units between adjacent views along X. Zero
		//                overlays them, which is what a single view wants and
		//                what a mirror would want.
		void Compose(float spacing);

		// What to draw, every tracked view together.
		//
		// @return The combined instances, valid until the next `Compose`.
		std::span<const engine::scene::DrawInstance> Instances() const {
			return Combined;
		}

		// Where to draw from.
		//
		// The first tracked view's, shifted far enough to hold the rest — a
		// compositor with one camera and several worlds has to choose, and
		// choosing the first and framing the row is the choice that shows
		// something rather than nothing.
		//
		// @return The camera's world-space placement.
		const engine::core::CFrame &CameraFrame() const {
			return ViewFrame;
		}

		// What to draw through.
		//
		// Separate from the placement because that is how a world holds it: a
		// `scene::Transform` on the camera row and a `scene::Camera` beside it.
		// Composing moves the placement and never the lens.
		//
		// @return The live camera's field of view and clipping distances.
		const engine::scene::Camera &Camera() const {
			return ViewCamera;
		}

		// The views being tracked.
		//
		// @return One record per view, in the order they were added.
		std::span<const ViewState> Views() const {
			return States;
		}

		// The number of views.
		//
		// @return The view count.
		size_t Count() const {
			return States.size();
		}

		// Frames published and never taken, across every channel.
		//
		// The number worth putting on F5: a figure that climbs is a compositor
		// that cannot keep up with its producers, which is a tuning problem
		// rather than a bug — but only if somebody can see it.
		//
		// @return The dropped count.
		uint64_t Dropped() const;

		// How many times a channel raised its payload ceiling, across every slot.
		//
		// The companion to `Dropped`, and the one that says a draw list is
		// growing without bound: a figure that settles is a set of worlds that
		// found their size, and one that keeps climbing is memory going up with
		// no upper edge. `ViewChannel::Growths` carries the argument.
		//
		// @return The raise count.
		uint64_t Growths() const;

		// Views that had nothing new at the last `Compose`.
		//
		// @return The stale view count.
		size_t StaleViews() const;

	  private:
		struct Slot {
			ViewState State;
			std::unique_ptr<engine::world::ViewChannel> Channel;

			// Kept between frames so a steady scene stops allocating, and so a
			// stale view still has something to draw.
			std::vector<std::byte> Payload;
		};

		Slot *Find(engine::world::WorldId id);

		std::vector<Slot> Slots;
		std::vector<ViewState> States;

		// One publish buffer, reused across worlds and frames. A publisher that
		// allocated inside its own render phase would pay the allocator once a
		// frame per world.
		std::vector<std::byte> Scratch;
		std::vector<engine::scene::DrawInstance> Combined;
		engine::core::CFrame ViewFrame;
		engine::scene::Camera ViewCamera;
	};
}
