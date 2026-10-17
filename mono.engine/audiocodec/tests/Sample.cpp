#include <engine/audio/Sample.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_SUITE_ID("engine.audio.sample")

using engine::audio::AudioFormat;
using engine::audio::CHANNELS;
using engine::audio::DEFAULT_SAMPLE_RATE;
using engine::audio::SampleBuffer;

namespace {
	constexpr AudioFormat STEREO{.SampleRate = 48000, .Channels = 2};
	constexpr AudioFormat MONO{.SampleRate = 48000, .Channels = 1};

	SampleBuffer Of(AudioFormat format, const std::vector<float> &samples) {
		return SampleBuffer(format, samples);
	}

	// A buffer where every sample is the same, so a gain is checkable by eye.
	SampleBuffer Flat(AudioFormat format, size_t frames, float value) {
		std::vector<float> samples(frames * format.Channels, value);
		return SampleBuffer(format, samples);
	}
}

TEST_CASE("a format has to describe something playable", "[audio]") {
	CHECK(STEREO.IsValid());
	CHECK(MONO.IsValid());
	CHECK_FALSE(AudioFormat{.SampleRate = 0, .Channels = 2}.IsValid());
	CHECK_FALSE(AudioFormat{.SampleRate = 48000, .Channels = 0}.IsValid());
	// More channels than anything can place. Refused where a format is read
	// rather than where it is mixed.
	CHECK_FALSE(AudioFormat{.SampleRate = 48000, .Channels = 64}.IsValid());
}

TEST_CASE("a silent buffer has frames and no signal", "[audio]") {
	const SampleBuffer buffer(STEREO, size_t{128});
	CHECK(buffer.Frames() == 128);
	CHECK(buffer.Data().size() == 256);
	CHECK(buffer.Peak() == 0.0f);
	CHECK_FALSE(buffer.Empty());
}

TEST_CASE("a partial frame is refused rather than truncated", "[audio]") {
	// A partial frame is a channel count that does not match the data, and
	// carrying it would put a stride bug in the mixer. Truncating would
	// silently change the content's length.
	const SampleBuffer buffer = Of(STEREO, {0.1f, 0.2f, 0.3f});
	CHECK(buffer.Empty());
	CHECK(buffer.Frames() == 0);
}

TEST_CASE("frames index by channel stride", "[audio]") {
	const SampleBuffer buffer = Of(STEREO, {1.0f, 2.0f, 3.0f, 4.0f});
	REQUIRE(buffer.Frames() == 2);

	REQUIRE(buffer.Frame(0).size() == 2);
	CHECK(buffer.Frame(0)[0] == 1.0f);
	CHECK(buffer.Frame(0)[1] == 2.0f);
	CHECK(buffer.Frame(1)[0] == 3.0f);

	// Past the end is empty rather than undefined: a mixer walking off a voice
	// is an ordinary bug and should not be a crash.
	CHECK(buffer.Frame(2).empty());
	CHECK(buffer.Frame(9999).empty());
}

TEST_CASE("length in seconds comes from the rate", "[audio]") {
	const SampleBuffer buffer(AudioFormat{.SampleRate = 1000, .Channels = 2}, size_t{500});
	CHECK(buffer.Seconds() == 0.5);
}

TEST_CASE("mixing sums frame for frame", "[audio]") {
	SampleBuffer into = Flat(STEREO, 4, 0.25f);
	const SampleBuffer from = Flat(STEREO, 4, 0.5f);

	CHECK(into.MixFrom(from) == 4);
	for (const float sample : into.Data()) {
		CHECK(sample == 0.75f);
	}
}

TEST_CASE("mixing applies a gain as it adds", "[audio]") {
	SampleBuffer into(STEREO, size_t{4});
	const SampleBuffer from = Flat(STEREO, 4, 1.0f);

	into.MixFrom(from, 0.5f);
	CHECK(into.Data()[0] == 0.5f);
}

TEST_CASE("mixing stops at the shorter buffer", "[audio]") {
	// A bus summing a voice that ended mid-block is the ordinary case, not an
	// error - so it stops rather than reading past either side.
	SampleBuffer into = Flat(STEREO, 8, 0.0f);
	const SampleBuffer from = Flat(STEREO, 3, 1.0f);

	CHECK(into.MixFrom(from) == 3);
	CHECK(into.Frame(2)[0] == 1.0f);
	CHECK(into.Frame(3)[0] == 0.0f);
}

TEST_CASE("mixing across formats is a no-op rather than a resample", "[audio]") {
	// Resampling is a decision about quality and belongs where one can be made,
	// not in the middle of a sum on a thread with a deadline.
	SampleBuffer into = Flat(STEREO, 4, 0.0f);
	const SampleBuffer mono = Flat(MONO, 4, 1.0f);
	const SampleBuffer other = Flat(AudioFormat{.SampleRate = 44100, .Channels = 2}, 4, 1.0f);

	CHECK(into.MixFrom(mono) == 0);
	CHECK(into.MixFrom(other) == 0);
	CHECK(into.Peak() == 0.0f);
}

TEST_CASE("samples past full scale pass through the graph unharmed", "[audio]") {
	// **Nominal range is ±1.0 and exceeding it inside the graph is legal.**
	// That is the whole reason the format is float: a mixer sums, and clamping
	// at every stage would lose headroom it cannot get back. The clamp happens
	// once, at the device.
	SampleBuffer into = Flat(STEREO, 4, 0.9f);
	into.MixFrom(Flat(STEREO, 4, 0.9f));
	CHECK(into.Peak() > 1.0f);
}

TEST_CASE("resizing keeps what was there and silences what is new", "[audio]") {
	SampleBuffer buffer = Flat(STEREO, 2, 1.0f);
	buffer.Resize(4);

	CHECK(buffer.Frames() == 4);
	CHECK(buffer.Frame(1)[0] == 1.0f);
	CHECK(buffer.Frame(3)[0] == 0.0f);
}

TEST_CASE("silencing keeps the length", "[audio]") {
	SampleBuffer buffer = Flat(STEREO, 4, 1.0f);
	buffer.Silence();
	CHECK(buffer.Frames() == 4);
	CHECK(buffer.Peak() == 0.0f);
}

// --- conversion -----------------------------------------------------------

TEST_CASE("converting to the same format is the same buffer", "[audio]") {
	const SampleBuffer buffer = Flat(STEREO, 16, 0.5f);
	const SampleBuffer same = buffer.ConvertTo(STEREO);
	CHECK(same.Frames() == 16);
	CHECK(same.Peak() == 0.5f);
}

TEST_CASE("halving the rate halves the frames", "[audio]") {
	const SampleBuffer buffer = Flat(STEREO, 100, 1.0f);
	const SampleBuffer down = buffer.ConvertTo(AudioFormat{.SampleRate = 24000, .Channels = 2});

	CHECK(down.Frames() == 50);
	CHECK(down.Format().SampleRate == 24000);
	// A flat signal stays flat whatever the ratio - an interpolator that got
	// its indexing wrong would show up here as ringing at the ends.
	CHECK(down.Peak() == 1.0f);
}

TEST_CASE("doubling the rate doubles the frames", "[audio]") {
	const SampleBuffer buffer = Flat(MONO, 50, 1.0f);
	const SampleBuffer up = buffer.ConvertTo(AudioFormat{.SampleRate = 96000, .Channels = 1});
	CHECK(up.Frames() == 100);
}

TEST_CASE("a rate ratio that lands short of a frame does not drop it", "[audio]") {
	// Rounded rather than truncated. On a short sound the dropped frame is an
	// audible click at the end of every playback.
	const SampleBuffer buffer = Flat(MONO, 3, 1.0f);
	const SampleBuffer up = buffer.ConvertTo(AudioFormat{.SampleRate = 48000 * 3 / 2, .Channels = 1});
	CHECK(up.Frames() == 5);
}

TEST_CASE("mono fans out to every channel", "[audio]") {
	const SampleBuffer mono = Flat(MONO, 4, 0.5f);
	const SampleBuffer stereo = mono.ConvertTo(STEREO);

	REQUIRE(stereo.Frames() == 4);
	CHECK(stereo.Frame(0)[0] == 0.5f);
	CHECK(stereo.Frame(0)[1] == 0.5f);
}

TEST_CASE("a stereo source down to mono takes the first channel", "[audio]") {
	// The naive answer, stated plainly rather than a matrix nobody configured.
	const SampleBuffer stereo = Of(STEREO, {1.0f, -1.0f, 1.0f, -1.0f});
	const SampleBuffer mono = stereo.ConvertTo(MONO);

	REQUIRE(mono.Frames() == 2);
	CHECK(mono.Frame(0)[0] == 1.0f);
}

TEST_CASE("converting an empty buffer gives an empty one in the target format", "[audio]") {
	const SampleBuffer empty(STEREO, size_t{0});
	const SampleBuffer converted = empty.ConvertTo(MONO);
	CHECK(converted.Frames() == 0);
	CHECK(converted.Format() == MONO);
}

TEST_CASE("converting to an invalid format gives nothing", "[audio]") {
	const SampleBuffer buffer = Flat(STEREO, 4, 1.0f);
	CHECK(buffer.ConvertTo(AudioFormat{.SampleRate = 0, .Channels = 2}).Empty());
	CHECK(buffer.ConvertTo(AudioFormat{.SampleRate = 48000, .Channels = 0}).Empty());
}

TEST_CASE("conversion is deterministic", "[audio]") {
	// Two runs of one conversion produce identical samples. A resampler that
	// accumulated a running position in float would drift, and the drift shows
	// up as content that hashes differently between two loads of one file.
	std::vector<float> ramp(1000);
	for (size_t index = 0; index < ramp.size(); ++index) {
		ramp[index] = std::sin(static_cast<float>(index) * 0.01f);
	}
	const SampleBuffer source = Of(MONO, ramp);
	const AudioFormat target{.SampleRate = 44100, .Channels = 2};

	const SampleBuffer first = source.ConvertTo(target);
	const SampleBuffer second = source.ConvertTo(target);

	REQUIRE(first.Frames() == second.Frames());
	for (size_t index = 0; index < first.Data().size(); ++index) {
		REQUIRE(first.Data()[index] == second.Data()[index]);
	}
}

TEST_CASE("the engine's defaults are what the mixer assumes", "[audio]") {
	// Stated as a case because the mixer's panning is a stereo operation and
	// would need rewriting rather than reconfiguring if this changed.
	CHECK(CHANNELS == 2);
	CHECK(DEFAULT_SAMPLE_RATE == 48000);
}
