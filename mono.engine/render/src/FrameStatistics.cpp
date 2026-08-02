#include <engine/render/FrameStatistics.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {

	void FrameStatistics::Record(double now, float deltaSeconds) {
		// A zero delta is the first frame, or a clock that did not move. Either
		// way it would divide to infinity.
		if (deltaSeconds <= 0.0f) {
			return;
		}

		if (Count == Ring.size()) {
			// Full. Doubling and re-linearising, so the oldest sample is at
			// index zero again and the wrap arithmetic stays simple. How many
			// samples twenty seconds holds depends on the frame rate, so the
			// size cannot be picked up front — but it settles after a second or
			// two and never grows again.
			std::vector<Sample> grown(Ring.empty() ? 256 : Ring.size() * 2);
			for (size_t offset = 0; offset < Count; offset++) {
				grown[offset] = Ring[IndexOf(offset)];
			}
			Ring.swap(grown);
			Head = 0;
		}

		// Before the sample lands, while the one it follows is still the newest.
		if (Count > 0) {
			ChangeSum += std::abs(deltaSeconds - Ring[IndexOf(Count - 1)].Delta);
		}

		Ring[(Head + Count) % Ring.size()] = Sample{now, deltaSeconds};
		Count++;
		DeltaSum += deltaSeconds;

		if (Count == 1) {
			Worst = deltaSeconds;
			Best = deltaSeconds;
			ExtremesStale = false;
		} else {
			Worst = std::max(Worst, deltaSeconds);
			Best = std::min(Best, deltaSeconds);
		}

		while (Count > 0 && now - Ring[Head].Time > WINDOW_SECONDS) {
			const float leaving = Ring[Head].Delta;

			// The pair this sample formed with the one after it goes with it.
			if (Count >= 2) {
				ChangeSum -= std::abs(Ring[IndexOf(1)].Delta - leaving);
			}
			DeltaSum -= leaving;

			// It may have been the best or the worst frame in the window, and
			// there is no way to know what the next one is without looking.
			if (leaving == Worst || leaving == Best) {
				ExtremesStale = true;
			}

			Head = (Head + 1) % Ring.size();
			Count--;
		}

		if (Count == 0) {
			DeltaSum = 0.0;
			ChangeSum = 0.0;
			Worst = 0.0f;
			Best = 0.0f;
			ExtremesStale = false;
		}
	}

	void FrameStatistics::Rescan() const {
		ExtremesStale = false;

		DeltaSum = 0.0;
		ChangeSum = 0.0;
		if (Count == 0) {
			Worst = 0.0f;
			Best = 0.0f;
			return;
		}

		Worst = Ring[Head].Delta;
		Best = Ring[Head].Delta;

		float previous = Ring[Head].Delta;
		for (size_t offset = 0; offset < Count; offset++) {
			const float delta = Ring[IndexOf(offset)].Delta;
			Worst = std::max(Worst, delta);
			Best = std::min(Best, delta);
			DeltaSum += delta;
			if (offset > 0) {
				ChangeSum += std::abs(delta - previous);
			}
			previous = delta;
		}
	}

	void FrameStatistics::Clear() {
		// The storage is kept. It is the size the frame rate needs it to be, and
		// closing and reopening the panel should not pay to learn that again.
		Head = 0;
		Count = 0;
		DeltaSum = 0.0;
		ChangeSum = 0.0;
		Worst = 0.0f;
		Best = 0.0f;
		ExtremesStale = false;
	}

	bool FrameStatistics::HasSamples() const {
		return Count > 0;
	}

	FrameSummary FrameStatistics::Summarise() const {
		FrameSummary summary;
		if (Count == 0) {
			return summary;
		}

		// Constant time on almost every frame. The walk happens only when the
		// best or worst frame in the window has just aged out of it.
		if (ExtremesStale) {
			Rescan();
		}

		const float latest = Ring[IndexOf(Count - 1)].Delta;
		summary.Current = 1.0f / latest;
		summary.CurrentMilliseconds = latest * 1000.0f;

		// The slowest frame is the lowest FPS, so Minimum reports the *largest*
		// delta. Getting these the wrong way round makes the panel say the
		// opposite of the truth, which is worse than not having it.
		summary.Minimum = 1.0f / Worst;
		summary.Maximum = 1.0f / Best;
		// The mean of the deltas, then inverted. Averaging the per-frame FPS
		// values instead would weight the fast frames far too heavily.
		summary.Average = static_cast<float>(static_cast<double>(Count) / DeltaSum);
		summary.Jitter =
			Count < 2 ? 0.0f : static_cast<float>(ChangeSum / static_cast<double>(Count - 1)) * 1000.0f;

		return summary;
	}

	// Each of these is the whole window for one number. They are here for a
	// caller that wants exactly one; anything wanting several wants Summarise.
	float FrameStatistics::Current() const {
		return Summarise().Current;
	}

	float FrameStatistics::CurrentMilliseconds() const {
		return Summarise().CurrentMilliseconds;
	}

	float FrameStatistics::Minimum() const {
		return Summarise().Minimum;
	}

	float FrameStatistics::Maximum() const {
		return Summarise().Maximum;
	}

	float FrameStatistics::Average() const {
		return Summarise().Average;
	}

	float FrameStatistics::Jitter() const {
		return Summarise().Jitter;
	}
}
