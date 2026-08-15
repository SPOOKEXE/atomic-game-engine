#pragma once

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
		// How many past ticks of position are kept.
		//
		// **A bound on memory and on fairness at once.** Deeper history honours
		// a laggier client's view of where things were, and how far back a
		// server is willing to go is a game-design decision about fairness
		// rather than an engine one - half a second at sixty hertz is the
		// default because it covers an ordinary connection and not an excuse.
		size_t HistoryTicks = 32;
	};

	// Where things were, for the last `HistoryTicks` ticks.
	//
	// @since v0.9
	class Rewind {
	  private:
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

	  public:
		// Builds a history.
		//
		// @param settings How much to keep.
		explicit Rewind(const RewindSettings &settings = {});

		// Starts recording a tick, dropping the oldest one held.
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
		// @param tick   The tick to sample, fractional.
		// @param entity The entity.
		// @param out    Where it was. Untouched on failure.
		// @return `false` when the tick is outside what is held, or the entity
		//         was not recorded at the ticks needed.
		bool Sample(double tick, ecs::Entity entity, core::Vector3 &out) const;

		// The tick a client was looking at when it produced an input.
		// @param inputTick    The tick the input was produced for.
		// @param delayTicks   `InterpolationSettings::DelayTicks`.
		// @param latencyMilliseconds The link's round-trip estimate.
		// @param tickRate     Ticks a second.
		// @return The fractional tick to sample, never below zero.
		static double
		TickSeenBy(uint64_t inputTick, double delayTicks, double latencyMilliseconds, double tickRate);

		// Walks recorded placements at a fractional tick.
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
