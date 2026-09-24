#include <engine/render/Flipbook.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {

	uint32_t FlipbookFrameAt(uint16_t frames, float rate, double seconds) {
		if (frames <= 1) {
			return 0;
		}

		const double perSecond = rate > 0.0f ? static_cast<double>(rate) : DEFAULT_RATE;

		// **Negative time holds frame zero rather than counting backwards.** A
		// caller's clock starts at zero and only grows, so this is the case a
		// mistake produces - and a modulo of a negative in C++ is
		// implementation-defined in sign, which would show as an animation
		// running backwards on one platform and not another.
		if (!(seconds > 0.0)) {
			return 0;
		}

		const double elapsed = std::floor(seconds * perSecond);

		// **In `double` before the cast, because a long-running session
		// overflows a 32-bit count.** At ten frames a second an `int` wraps after
		// about seven years and a `float` loses whole-number precision after
		// about nineteen days - at which point the animation would visibly
		// stutter and then stop. The modulo brings it back into range first.
		return static_cast<uint32_t>(std::fmod(elapsed, static_cast<double>(frames)));
	}

	uint32_t FlipbookFrameAt(std::span<const float> cumulativeEnds, double seconds) {
		if (cumulativeEnds.size() <= 1 || !(seconds > 0.0) || !std::isfinite(seconds)) return 0;
		const double total = cumulativeEnds.back();
		if (!(total > 0.0)) return 0;
		const float position = static_cast<float>(std::fmod(seconds, total));
		return static_cast<uint32_t>(
			std::upper_bound(cumulativeEnds.begin(), cumulativeEnds.end(), position) - cumulativeEnds.begin()
		);
	}

	FlipbookCell FlipbookCellAt(uint8_t side, uint16_t frames, float rate, double seconds) {
		if (side <= 1 || frames == 0) {
			return {};
		}

		const uint32_t frame = FlipbookFrameAt(frames, rate, seconds);
		const auto grid = static_cast<float>(side);

		FlipbookCell cell;
		cell.Scale = 1.0f / grid;

		// **Row-major from the top left**, which is the order `bake::ReadGif`
		// writes the cells in. Reading them column-major would play a GIF in an
		// order nothing produced and would look like a shuffled animation rather
		// than like an axis swapped.
		cell.OffsetU = static_cast<float>(frame % side) / grid;
		cell.OffsetV = static_cast<float>(frame / side) / grid;
		return cell;
	}

	FlipbookCell FlipbookCellAt(uint8_t side, std::span<const float> cumulativeEnds, double seconds) {
		if (side <= 1 || cumulativeEnds.empty()) return {};
		const uint32_t frame = FlipbookFrameAt(cumulativeEnds, seconds);
		const float grid = static_cast<float>(side);
		return FlipbookCell{
			.Scale = 1.0f / grid,
			.OffsetU = static_cast<float>(frame % side) / grid,
			.OffsetV = static_cast<float>(frame / side) / grid,
		};
	}
}
