#include <engine/replication/Prediction.hpp>

#include <algorithm>

namespace engine::replication {

	Prediction::Prediction(const PredictionSettings &settings) : Settings_(settings) {}

	bool Prediction::Record(uint64_t tick, std::span<const std::byte> bytes) {
		bool dropped = false;

		// The oldest goes, not the newest. The oldest is the one the server is
		// most likely to have already consumed, and dropping the newest would
		// throw away the input the player just made — which is the one they can
		// see not happening.
		if (Settings_.MaximumPending > 0 && Inputs.size() >= Settings_.MaximumPending) {
			Inputs.erase(Inputs.begin());
			Dropped_++;
			dropped = true;
		}

		Input input;
		input.Tick = tick;
		input.Bytes.assign(bytes.begin(), bytes.end());
		Inputs.push_back(std::move(input));
		return !dropped;
	}

	size_t Prediction::Reconcile(uint64_t applied) {
		// Everything up to and including the acknowledged tick. What is left is
		// exactly what has to be replayed to arrive back at the present.
		const auto first = std::find_if(Inputs.begin(), Inputs.end(), [applied](const Input &input) {
			return input.Tick > applied;
		});

		const size_t retired = static_cast<size_t>(first - Inputs.begin());
		Inputs.erase(Inputs.begin(), first);
		return retired;
	}

	void Prediction::Clear() {
		Inputs.clear();
	}
}
