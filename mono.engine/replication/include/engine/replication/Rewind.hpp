#pragma once

// A short history of where things were, so a server can judge a client's action
// against what that client actually saw.
//
// **The problem this exists for, in one sentence.** A client renders the world
// `InterpolationSettings::DelayTicks` behind the newest tick it has received,
// and that tick left the server some fraction of a round trip earlier — so when
// a player aims at something, the thing they aimed at has already moved on the
// server by the time the input arrives. Judging the shot against the server's
// *current* state makes a hit that looked certain miss, and no amount of
// tightening the network fixes it, because the gap is the speed of light plus
// the interpolation the smoothness depends on.
//
// **What this is not.** It is not a rollback: nothing is re-simulated and no
// state is restored. It is a *record* of past placements and a query against
// it, so a game can ask "where was this, then" and decide for itself. Rollback
// is a much larger idea with a much larger blast radius, and a hit test does
// not need one.
//
// **The lookup is the caller's, exactly as `DistancePriority`'s is.** This
// module carries named components and has no idea which of them is a position,
// which is why `Record` is given one rather than reading one. The same division
// `SetInterest` and `SetPriority` are built on.
//
// **Nothing here reads a clock.** A tick number goes in and a tick number comes
// out; `TickSeenBy` turns a latency into a tick count and takes both the latency
// and the rate from its caller. A history that sampled its own clock would make
// two runs of one server disagree, which is the property `world::Recording`
// exists to preserve.
//
// @tier L12 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine::replication {

	// How much history to keep.
	//
	// @since v0.9
	struct RewindSettings {
		// How many ticks of placements to hold.
		//
		// **A ceiling on memory and on how far back a claim may reach**, and
		// the second is the one that matters: a client claiming to have seen
		// something two seconds ago is either on a dreadful connection or
		// lying, and both are answered the same way — the query fails and the
		// game decides what to do about it.
		//
		// Thirty-two ticks is half a second at sixty hertz, which covers a
		// round trip of two hundred milliseconds plus the interpolation delay
		// with room to spare. Past that the honest answer is that the server
		// does not know.
		size_t HistoryTicks = 32;
	};

	// Where things were, for the last `HistoryTicks` ticks.
	//
	// @since v0.9
	class Rewind {
	  private:
		// One tick's placements.
		//
		// **The maps are reused rather than freed**, so a server recording
		// sixty times a second allocates for the first `HistoryTicks` ticks and
		// then never again. `Clear` on an `unordered_map` keeps its buckets,
		// which is the whole reason a ring of them beats a deque of fresh ones.
		struct Frame {
			uint64_t Tick = 0;
			bool Filled = false;
			std::unordered_map<uint64_t, core::Vector3> Placements;
		};

		const Frame *Find(uint64_t tick) const;

		std::vector<Frame> Frames;
		size_t Cursor = 0;
		size_t Held = 0;
		uint64_t Latest = 0;
		bool Open = false;

		// **The private section comes first, which is unusual here and is the
		// price of one template.** `Each` is a template on the public surface
		// and calls `Find`, so `Find` has to be declared before it — and a
		// member declared after its first use inside a member *template* is not
		// found, unlike an ordinary member function. Splitting `Each` out into
		// a free function taking the frame would leak `Frame` into the header
		// instead, which is worse.

	  public:
		// Builds a history.
		//
		// @param settings How much to keep.
		explicit Rewind(const RewindSettings &settings = {});

		// Starts recording a tick, dropping the oldest one held.
		//
		// **Ticks must arrive in order and each at most once.** A repeated or
		// out-of-order tick is refused rather than accepted into the wrong
		// slot, because a history whose frames are not in tick order answers
		// every query with the wrong frame and says nothing about it.
		//
		// @param tick The tick being recorded.
		// @return `false` for a tick at or before the newest one held.
		bool Begin(uint64_t tick);

		// Records where one entity is, in the tick `Begin` opened.
		//
		// @param entity The entity.
		// @param at     Where it is, in world space.
		// @return `false` when no tick is open.
		bool Record(ecs::Entity entity, const core::Vector3 &at);

		// Where an entity was at a tick, interpolating between the two either
		// side of a fractional one.
		//
		// **Fractional because a client's view is fractional.** A renderer sits
		// between two ticks and blends — `SnapshotBuffer::RenderTick` returns a
		// double for exactly this reason — so rewinding to a whole tick would
		// answer with a pose the client never actually saw, and the error is
		// largest for the fastest things, which is where a hit test is already
		// hardest.
		//
		// @param tick   The tick to sample, fractional.
		// @param entity The entity.
		// @param out    Where it was. Untouched on failure.
		// @return `false` when the tick is outside what is held, or the entity
		//         was not recorded at the ticks needed.
		bool Sample(double tick, ecs::Entity entity, core::Vector3 &out) const;

		// The tick a client was looking at when it produced an input.
		//
		// The input's own tick less the interpolation delay it renders behind
		// and the half round trip it took to arrive. **Both are the caller's**:
		// this module knows neither the client's link nor the game's
		// interpolation, and taking either from somewhere global would be the
		// clock read the header rules out.
		//
		// @param inputTick    The tick the input was produced for.
		// @param delayTicks   `InterpolationSettings::DelayTicks`.
		// @param latencyMilliseconds The link's round-trip estimate.
		// @param tickRate     Ticks a second.
		// @return The fractional tick to sample, never below zero.
		static double
		TickSeenBy(uint64_t inputTick, double delayTicks, double latencyMilliseconds, double tickRate);

		// Walks everything recorded at a tick, interpolated the same way
		// `Sample` interpolates.
		//
		// **The enumeration a hit test needs and `Sample` cannot give it.** A
		// caller testing a shot has a ray and no candidate list — and walking
		// the *world* instead would miss exactly the entities lag compensation
		// exists for: the ones destroyed between the tick the client saw and
		// now. The history remembers them; the world does not.
		//
		// The frame walked is the one at or before `tick`, so an entity present
		// then is offered even if it is gone by the next.
		//
		// @param tick  The tick to walk, fractional.
		// @param visit Called as `visit(ecs::Entity, const core::Vector3 &)`.
		// @return How many were visited.
		template <class Visitor> size_t Each(double tick, Visitor &&visit) const {
			if (Held == 0 || !std::isfinite(tick) || tick < 0.0) {
				return 0;
			}

			const double floored = std::floor(tick);
			if (floored < 0.0 || floored > static_cast<double>(Latest)) {
				return 0;
			}

			const Frame *frame = Find(static_cast<uint64_t>(floored));
			if (frame == nullptr) {
				return 0;
			}

			size_t visited = 0;
			for (const auto &[id, placement] : frame->Placements) {
				ecs::Entity entity;
				entity.Id = id;

				// Through `Sample` rather than handing back the stored value,
				// so the fraction is honoured and there is one interpolation
				// rather than two that could disagree.
				core::Vector3 at;
				if (Sample(tick, entity, at)) {
					visit(entity, at);
					visited++;
				}
			}
			return visited;
		}

		// The oldest tick held, or zero when nothing has been recorded.
		uint64_t Oldest() const;

		// The newest tick held, or zero when nothing has been recorded.
		uint64_t Newest() const {
			return Latest;
		}

		// How many ticks are held.
		size_t Depth() const {
			return Held;
		}

		// Forgets everything, keeping the capacity.
		//
		// For a world that has been reloaded or rewound by something else: a
		// history spanning a discontinuity would answer with placements from
		// before it, which is worse than answering with nothing.
		void Clear();
	};
}
