#include <engine/replication/Rewind.hpp>

#include <algorithm>
#include <cmath>

namespace engine::replication {

	Rewind::Rewind(const RewindSettings &settings) {
		Frames.resize(std::max<size_t>(1, settings.HistoryTicks));
	}

	bool Rewind::Begin(uint64_t tick) {
		if (Held > 0 && tick <= Latest) {
			// Preserve tick order so ring slots cannot answer the wrong query.
			return false;
		}

		Frame &frame = Frames[Cursor];
		frame.Tick = tick;
		frame.Filled = true;

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
			return false;
		}

		if (blend <= 0.0) {
			out = found->second;
			return true;
		}

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
			return static_cast<double>(inputTick);
		}

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
