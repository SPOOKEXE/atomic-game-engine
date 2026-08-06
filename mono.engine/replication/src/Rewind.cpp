#include <engine/replication/Rewind.hpp>

#include <algorithm>
#include <cmath>

namespace engine::replication {

	Rewind::Rewind(const RewindSettings &settings) {
		// At least one frame, so `Begin` always has somewhere to write. A
		// setting of zero is a caller mistake and a history of one is the
		// smallest thing that is still a history rather than a crash.
		Frames.resize(std::max<size_t>(1, settings.HistoryTicks));
	}

	bool Rewind::Begin(uint64_t tick) {
		if (Held > 0 && tick <= Latest) {
			// **Refused rather than written into the ring anyway.** A repeated
			// or out-of-order tick lands in the slot the next one wanted, and
			// from then on every query finds the wrong frame — with nothing
			// saying so, because a frame is only ever identified by the tick
			// stamped on it.
			return false;
		}

		Frame &frame = Frames[Cursor];
		frame.Tick = tick;
		frame.Filled = true;

		// Cleared rather than replaced: `unordered_map::clear` keeps its
		// buckets, so a server recording sixty times a second allocates for the
		// first `HistoryTicks` ticks and then never again.
		frame.Placements.clear();

		Cursor = (Cursor + 1) % Frames.size();
		Held = std::min(Held + 1, Frames.size());
		Latest = tick;
		Open = true;
		return true;
	}

	bool Rewind::Record(ecs::Entity entity, const core::Vector3 &at) {
		if (!Open) {
			return false;
		}

		// The frame `Begin` opened is the one before the cursor, because the
		// cursor was advanced past it.
		Frame &frame = Frames[(Cursor + Frames.size() - 1) % Frames.size()];
		frame.Placements[entity.Id] = at;
		return true;
	}

	const Rewind::Frame *Rewind::Find(uint64_t tick) const {
		for (const Frame &frame : Frames) {
			if (frame.Filled && frame.Tick == tick) {
				return &frame;
			}
		}
		return nullptr;
	}

	bool Rewind::Sample(double tick, ecs::Entity entity, core::Vector3 &out) const {
		if (Held == 0 || !std::isfinite(tick) || tick < 0.0) {
			return false;
		}

		const double floored = std::floor(tick);
		if (floored < 0.0 || floored > static_cast<double>(Latest)) {
			return false;
		}

		const uint64_t earlier = static_cast<uint64_t>(floored);
		const double blend = tick - floored;

		const Frame *first = Find(earlier);
		if (first == nullptr) {
			return false;
		}

		const auto found = first->Placements.find(entity.Id);
		if (found == first->Placements.end()) {
			// Recorded at other ticks perhaps, but not this one — an entity
			// that had not spawned yet, or one a game chose not to offer. Not
			// an error, and not something to answer with a neighbouring tick's
			// value: that would be inventing a placement the client never saw.
			return false;
		}

		if (blend <= 0.0) {
			out = found->second;
			return true;
		}

		// **The later frame is optional, and its absence is not a failure.**
		// Sampling a fraction past the newest tick held is the ordinary case
		// for an input that arrived promptly, and answering with the tick that
		// exists beats refusing a query that is one part in sixty out.
		const Frame *second = Find(earlier + 1);
		if (second == nullptr) {
			out = found->second;
			return true;
		}

		const auto later = second->Placements.find(entity.Id);
		if (later == second->Placements.end()) {
			out = found->second;
			return true;
		}

		const core::Vector3 &from = found->second;
		const core::Vector3 &to = later->second;
		const float alpha = static_cast<float>(blend);

		// Lerp rather than the NLerp a `CFrame` would want: this is a position
		// and nothing else. Whatever a game needs of an orientation, it is not
		// this — and a rewind that carried one would be carrying a pose, which
		// is a much larger promise than "where was it".
		out = core::Vector3{
			from.X + (to.X - from.X) * alpha,
			from.Y + (to.Y - from.Y) * alpha,
			from.Z + (to.Z - from.Z) * alpha,
		};
		return true;
	}

	double
	Rewind::TickSeenBy(uint64_t inputTick, double delayTicks, double latencyMilliseconds, double tickRate) {
		if (!std::isfinite(delayTicks) || !std::isfinite(latencyMilliseconds) || !std::isfinite(tickRate) ||
			tickRate <= 0.0) {
			// A caller supplying nonsense gets the input's own tick, which is
			// the un-compensated answer and the one thing that is never wrong
			// by more than the compensation itself.
			return static_cast<double>(inputTick);
		}

		// **Half the round trip, not all of it.** The gap between what the
		// client saw and what the server holds is the time the *snapshot* took
		// to get out, and the input's journey back is already accounted for by
		// the tick the input names.
		const double oneWaySeconds = std::max(0.0, latencyMilliseconds) * 0.5 / 1000.0;
		const double behind = std::max(0.0, delayTicks) + oneWaySeconds * tickRate;

		return std::max(0.0, static_cast<double>(inputTick) - behind);
	}

	uint64_t Rewind::Oldest() const {
		if (Held == 0) {
			return 0;
		}

		uint64_t oldest = Latest;
		for (const Frame &frame : Frames) {
			if (frame.Filled) {
				oldest = std::min(oldest, frame.Tick);
			}
		}
		return oldest;
	}

	void Rewind::Clear() {
		for (Frame &frame : Frames) {
			frame.Filled = false;
			frame.Placements.clear();
		}
		Cursor = 0;
		Held = 0;
		Latest = 0;
		Open = false;
	}
}
