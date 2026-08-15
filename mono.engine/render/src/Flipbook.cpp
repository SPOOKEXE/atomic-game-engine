#include <engine/render/Flipbook.hpp>

#include <cmath>

namespace engine::render {

	uint32_t FlipbookFrameAt(uint8_t frames, float rate, double seconds) {
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

	FlipbookCell FlipbookCellAt(uint8_t side, uint8_t frames, float rate, double seconds) {
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
}
