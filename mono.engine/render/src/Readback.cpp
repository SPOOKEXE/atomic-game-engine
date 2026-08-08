#include <engine/render/Readback.hpp>

namespace engine::render {

	namespace {

		// Which bucket a value falls in.
		//
		// **`value * BUCKETS / 256` and not `value / 16`**, which are the same
		// arithmetic but only one of them stays right when `HISTOGRAM_BUCKETS`
		// changes. The constant is declared once; this reads it.
		size_t BucketOf(uint8_t value) {
			return static_cast<size_t>(value) * HISTOGRAM_BUCKETS / 256;
		}

		void Count(ChannelHistogram &channel, uint8_t value) {
			channel.Buckets[BucketOf(value)]++;
			channel.Minimum = value < channel.Minimum ? value : channel.Minimum;
			channel.Maximum = value > channel.Maximum ? value : channel.Maximum;
			channel.Counted++;
		}
	}

	ImageHistogram Histogram(std::span<const uint32_t> pixels) {
		ImageHistogram out;

		// **Byte order, stated rather than assumed.** SDL downloads a texture
		// into the transfer buffer in the texture's own format, and the scene
		// targets are `B8G8R8A8` — which is what `Impl::WriteCapture` already
		// takes apart the same way to write a BMP. A second unpacking that
		// disagreed would put the red histogram on the blue channel, and the
		// warning would be about the wrong thing rather than absent.
		for (const uint32_t pixel : pixels) {
			Count(out.Blue, static_cast<uint8_t>(pixel & 0xFFu));
			Count(out.Green, static_cast<uint8_t>((pixel >> 8) & 0xFFu));
			Count(out.Red, static_cast<uint8_t>((pixel >> 16) & 0xFFu));
			Count(out.Alpha, static_cast<uint8_t>((pixel >> 24) & 0xFFu));
		}

		return out;
	}

	void PendingReadback::Submitted(uint64_t frame) {
		InFlight_ = true;
		Requested_ = frame;
	}

	bool PendingReadback::Poll(bool signalled) {
		if (!InFlight_ || !signalled) {
			return false;
		}

		InFlight_ = false;
		Have_ = true;

		// **The picture belongs to the frame it was asked for**, not to the one
		// the fence happened to signal on. A panel saying "one frame old" when
		// the answer is three is worse than saying nothing: it is a number that
		// looks measured.
		Frame_ = Requested_;
		return true;
	}

	uint64_t PendingReadback::Age(uint64_t frame) const {
		if (!Have_ || frame <= Frame_) {
			return 0;
		}
		return frame - Frame_;
	}

	void PendingReadback::Clear() {
		InFlight_ = false;
		Have_ = false;
		Requested_ = 0;
		Frame_ = 0;
	}
}
