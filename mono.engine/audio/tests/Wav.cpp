#include <engine/audio/Sample.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.audio.wav")
TEST_DEPENDS("engine.audio.sample")

using engine::audio::DecodeWav;
using engine::audio::IsWav;
using engine::audio::SampleBuffer;

namespace {
	// A WAV file, assembled a field at a time.
	//
	// Built rather than checked in, because every case below is a *malformed*
	// file and a fixture directory of thirty broken `.wav`s is unreadable. The
	// builder makes what is wrong with each one visible at the call site.
	struct WavBuilder {
		uint16_t Encoding = 1;
		uint16_t Channels = 1;
		uint32_t SampleRate = 48000;
		uint16_t Bits = 16;
		std::vector<std::byte> Payload;

		// Overrides, for the cases that need a field to lie.
		bool LieAboutRiffSize = false;
		bool LieAboutDataSize = false;
		bool OmitFormat = false;
		bool OmitData = false;
		uint32_t ExtraDataBytes = 0;

		static void PutU16(std::vector<std::byte> &out, uint16_t value) {
			out.push_back(static_cast<std::byte>(value & 0xFF));
			out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
		}

		static void PutU32(std::vector<std::byte> &out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
			}
		}

		static void PutTag(std::vector<std::byte> &out, const char *tag) {
			for (int index = 0; index < 4; ++index) {
				out.push_back(static_cast<std::byte>(tag[index]));
			}
		}

		std::vector<std::byte> Build() const {
			std::vector<std::byte> body;

			if (!OmitFormat) {
				PutTag(body, "fmt ");
				PutU32(body, 16);
				PutU16(body, Encoding);
				PutU16(body, Channels);
				PutU32(body, SampleRate);
				PutU32(body, SampleRate * Channels * (Bits / 8u));			 // byte rate
				PutU16(body, static_cast<uint16_t>(Channels * (Bits / 8u))); // block align
				PutU16(body, Bits);
			}

			if (!OmitData) {
				PutTag(body, "data");
				const auto declared = static_cast<uint32_t>(Payload.size()) + ExtraDataBytes +
									  (LieAboutDataSize ? 1000000u : 0u);
				PutU32(body, declared);
				body.insert(body.end(), Payload.begin(), Payload.end());
			}

			std::vector<std::byte> file;
			PutTag(file, "RIFF");
			PutU32(file, LieAboutRiffSize ? 0xFFFFFFFFu : static_cast<uint32_t>(body.size() + 4));
			PutTag(file, "WAVE");
			file.insert(file.end(), body.begin(), body.end());
			return file;
		}
	};

	std::vector<std::byte> Signed16(const std::vector<int16_t> &samples) {
		std::vector<std::byte> bytes;
		for (const int16_t sample : samples) {
			const auto raw = static_cast<uint16_t>(sample);
			bytes.push_back(static_cast<std::byte>(raw & 0xFF));
			bytes.push_back(static_cast<std::byte>((raw >> 8) & 0xFF));
		}
		return bytes;
	}

	bool Near(float value, float expected, float tolerance = 0.0001f) {
		return std::abs(value - expected) <= tolerance;
	}
}

TEST_CASE("a plain 16-bit mono file decodes", "[audio][wav]") {
	WavBuilder builder;
	builder.Payload = Signed16({0, 16384, -16384, 32767});

	const std::vector<std::byte> file = builder.Build();
	CHECK(IsWav(file));

	const auto decoded = DecodeWav(file);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Format().SampleRate == 48000);
	CHECK(decoded->Format().Channels == 1);
	REQUIRE(decoded->Frames() == 4);

	CHECK(decoded->Data()[0] == 0.0f);
	CHECK(Near(decoded->Data()[1], 0.5f));
	// Scaled by 32768 rather than 32767, so -1.0 is exactly representable.
	CHECK(decoded->Data()[2] == -0.5f);
	CHECK(Near(decoded->Data()[3], 1.0f, 0.001f));
}

TEST_CASE("full-scale negative is exactly minus one", "[audio][wav]") {
	// The reason the divisor is 32768. With 32767 every sample is a fraction
	// too loud and full-scale negative clips.
	WavBuilder builder;
	builder.Payload = Signed16({-32768});

	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	CHECK(decoded->Data()[0] == -1.0f);
}

TEST_CASE("stereo interleaving survives", "[audio][wav]") {
	WavBuilder builder;
	builder.Channels = 2;
	builder.Payload = Signed16({16384, -16384, 8192, -8192});

	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	CHECK(decoded->Format().Channels == 2);
	REQUIRE(decoded->Frames() == 2);
	CHECK(Near(decoded->Frame(0)[0], 0.5f));
	CHECK(Near(decoded->Frame(0)[1], -0.5f));
	CHECK(Near(decoded->Frame(1)[0], 0.25f));
}

TEST_CASE("8-bit is unsigned and centred on 128", "[audio][wav]") {
	// The one encoding in this format that is not signed. Forgetting it is a
	// full-scale DC offset rather than a subtle error.
	WavBuilder builder;
	builder.Bits = 8;
	builder.Payload = {std::byte{128}, std::byte{255}, std::byte{0}};

	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->Frames() == 3);
	CHECK(decoded->Data()[0] == 0.0f);
	CHECK(Near(decoded->Data()[1], 0.9921875f));
	CHECK(decoded->Data()[2] == -1.0f);
}

TEST_CASE("24-bit sign-extends", "[audio][wav]") {
	// There is no 24-bit integer, so the sign bit is carried by hand. Getting
	// it wrong turns every negative sample into a large positive one, which is
	// full-scale noise.
	WavBuilder builder;
	builder.Bits = 24;
	// +4194304 (quarter scale) then -4194304.
	builder.Payload = {
		std::byte{0x00},
		std::byte{0x00},
		std::byte{0x40},
		std::byte{0x00},
		std::byte{0x00},
		std::byte{0xC0},
	};

	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->Frames() == 2);
	CHECK(Near(decoded->Data()[0], 0.5f));
	CHECK(Near(decoded->Data()[1], -0.5f));
}

TEST_CASE("32-bit float passes through", "[audio][wav]") {
	WavBuilder builder;
	builder.Encoding = 3;
	builder.Bits = 32;

	const std::vector<float> values{0.0f, 0.5f, -0.25f};
	for (const float value : values) {
		std::byte raw[4];
		std::memcpy(raw, &value, sizeof(value));
		builder.Payload.insert(builder.Payload.end(), raw, raw + 4);
	}

	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->Frames() == 3);
	CHECK(decoded->Data()[1] == 0.5f);
	CHECK(decoded->Data()[2] == -0.25f);
}

// --- the refusals ----------------------------------------------------------

TEST_CASE("something that is not a wav is refused", "[audio][wav]") {
	const std::vector<std::byte> nonsense(64, std::byte{0x41});
	CHECK_FALSE(IsWav(nonsense));
	CHECK_FALSE(DecodeWav(nonsense).has_value());
	CHECK_FALSE(DecodeWav({}).has_value());
}

TEST_CASE("a chunk claiming to run past the end is refused, not clamped", "[audio][wav]") {
	// **The check that matters.** Clamping would turn a truncated file into a
	// shorter sound that plays, so the corruption stays inaudible until
	// somebody wonders why a footstep got quieter.
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000});
	builder.LieAboutDataSize = true;

	CHECK_FALSE(DecodeWav(builder.Build()).has_value());
}

TEST_CASE("a lying RIFF size does not bound the walk", "[audio][wav]") {
	// The RIFF size is a number in the file; the buffer's length is a fact.
	// Believing the file over the fact is how a parser reads past its input.
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000});
	builder.LieAboutRiffSize = true;

	// Still decodes: the walk is bounded by what arrived, so an absurd RIFF
	// size changes nothing.
	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	CHECK(decoded->Frames() == 2);
}

TEST_CASE("a truncated file is refused", "[audio][wav]") {
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000, 3000, 4000});
	const std::vector<std::byte> whole = builder.Build();

	// Cut at every length short of the whole. None of them may produce a
	// buffer, and none may read out of bounds — which is what the sanitiser
	// build is for.
	for (size_t length = 0; length < whole.size(); ++length) {
		const std::vector<std::byte> partial(whole.begin(), whole.begin() + static_cast<ptrdiff_t>(length));
		INFO("truncated to " << length << " bytes");
		CHECK_FALSE(DecodeWav(partial).has_value());
	}
	CHECK(DecodeWav(whole).has_value());
}

TEST_CASE("a file with no format chunk is refused", "[audio][wav]") {
	WavBuilder builder;
	builder.OmitFormat = true;
	builder.Payload = Signed16({1000});
	CHECK_FALSE(DecodeWav(builder.Build()).has_value());
}

TEST_CASE("a file with no data chunk is refused", "[audio][wav]") {
	WavBuilder builder;
	builder.OmitData = true;
	CHECK_FALSE(DecodeWav(builder.Build()).has_value());
}

TEST_CASE("a codec this engine does not have is refused rather than guessed at", "[audio][wav]") {
	// A decoder that guessed would produce noise at full volume, which is the
	// single worst failure this subsystem has.
	for (const uint16_t encoding : {uint16_t{2}, uint16_t{6}, uint16_t{7}, uint16_t{0x11}}) {
		WavBuilder builder;
		builder.Encoding = encoding;
		builder.Payload = Signed16({1000, 2000});
		INFO("encoding " << encoding);
		CHECK_FALSE(DecodeWav(builder.Build()).has_value());
	}
}

TEST_CASE("a bit depth nothing writes is refused", "[audio][wav]") {
	for (const uint16_t bits : {uint16_t{4}, uint16_t{12}, uint16_t{20}, uint16_t{64}, uint16_t{0}}) {
		WavBuilder builder;
		builder.Bits = bits;
		builder.Payload = std::vector<std::byte>(64, std::byte{0});
		INFO("bits " << bits);
		CHECK_FALSE(DecodeWav(builder.Build()).has_value());
	}
}

TEST_CASE("a format whose two fields disagree is refused", "[audio][wav]") {
	// Float is 32-bit and integer PCM is not. When the encoding and the depth
	// contradict each other, believing either one is wrong.
	WavBuilder integer32;
	integer32.Encoding = 1;
	integer32.Bits = 32;
	integer32.Payload = std::vector<std::byte>(16, std::byte{0});
	CHECK_FALSE(DecodeWav(integer32.Build()).has_value());

	WavBuilder shortFloat;
	shortFloat.Encoding = 3;
	shortFloat.Bits = 16;
	shortFloat.Payload = std::vector<std::byte>(16, std::byte{0});
	CHECK_FALSE(DecodeWav(shortFloat.Build()).has_value());
}

TEST_CASE("a zero sample rate or channel count is refused", "[audio][wav]") {
	WavBuilder noRate;
	noRate.SampleRate = 0;
	noRate.Payload = Signed16({1000});
	CHECK_FALSE(DecodeWav(noRate.Build()).has_value());

	WavBuilder noChannels;
	noChannels.Channels = 0;
	noChannels.Payload = Signed16({1000});
	CHECK_FALSE(DecodeWav(noChannels.Build()).has_value());
}

TEST_CASE("an absurd channel count is refused", "[audio][wav]") {
	WavBuilder builder;
	builder.Channels = 4096;
	builder.Payload = std::vector<std::byte>(8192, std::byte{0});
	CHECK_FALSE(DecodeWav(builder.Build()).has_value());
}

TEST_CASE("a data chunk that is not a whole number of frames is refused", "[audio][wav]") {
	// A trailing partial frame is a truncated file, and rounding it away is
	// the clamp this decoder refuses.
	WavBuilder builder;
	builder.Channels = 2;
	builder.Payload = Signed16({1000, 2000, 3000}); // three samples, two channels
	CHECK_FALSE(DecodeWav(builder.Build()).has_value());
}

TEST_CASE("an empty data chunk decodes to silence rather than failing", "[audio][wav]") {
	// A zero-length sound is a legitimate thing to publish, and it is not the
	// same as a corrupt file.
	WavBuilder builder;
	const auto decoded = DecodeWav(builder.Build());
	REQUIRE(decoded.has_value());
	CHECK(decoded->Frames() == 0);
}

TEST_CASE("an unknown chunk between the known ones is skipped", "[audio][wav]") {
	// Real files carry `LIST`, `fact`, `cue ` and whatever else a tool felt
	// like writing. A decoder that stopped at the first one it did not know
	// would refuse most of what exists.
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000});
	std::vector<std::byte> file = builder.Build();

	// Splice a `LIST` chunk in after the RIFF header.
	std::vector<std::byte> extra;
	WavBuilder::PutTag(extra, "LIST");
	WavBuilder::PutU32(extra, 4);
	WavBuilder::PutTag(extra, "INFO");
	file.insert(file.begin() + 12, extra.begin(), extra.end());

	const auto decoded = DecodeWav(file);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Frames() == 2);
}

TEST_CASE("an odd-length chunk is followed by a pad byte", "[audio][wav]") {
	// Chunks are word-aligned and the pad is not counted in the length.
	// Missing it reads every later chunk header one byte off, which looks like
	// a corrupt file rather than a parser bug.
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000});
	std::vector<std::byte> file = builder.Build();

	std::vector<std::byte> extra;
	WavBuilder::PutTag(extra, "cue ");
	WavBuilder::PutU32(extra, 3);
	extra.push_back(std::byte{1});
	extra.push_back(std::byte{2});
	extra.push_back(std::byte{3});
	extra.push_back(std::byte{0}); // the pad
	file.insert(file.begin() + 12, extra.begin(), extra.end());

	const auto decoded = DecodeWav(file);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Frames() == 2);
}

TEST_CASE("a chunk header split across the end of the file stops the walk", "[audio][wav]") {
	WavBuilder builder;
	builder.Payload = Signed16({1000, 2000});
	std::vector<std::byte> file = builder.Build();
	// Four bytes of a chunk header and no length.
	WavBuilder::PutTag(file, "LIST");

	// The data already parsed, so this decodes what it has rather than
	// refusing — a trailing scrap is not a reason to drop a valid sound.
	const auto decoded = DecodeWav(file);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Frames() == 2);
}

TEST_CASE("decoding is deterministic", "[audio][wav]") {
	WavBuilder builder;
	builder.Channels = 2;
	for (int index = 0; index < 500; ++index) {
		const auto value = static_cast<int16_t>((index * 517) % 30000 - 15000);
		const std::vector<std::byte> bytes = Signed16({value});
		builder.Payload.insert(builder.Payload.end(), bytes.begin(), bytes.end());
	}

	const std::vector<std::byte> file = builder.Build();
	const auto first = DecodeWav(file);
	const auto second = DecodeWav(file);
	REQUIRE(first.has_value());
	REQUIRE(second.has_value());
	REQUIRE(first->Data().size() == second->Data().size());
	for (size_t index = 0; index < first->Data().size(); ++index) {
		REQUIRE(first->Data()[index] == second->Data()[index]);
	}
}
