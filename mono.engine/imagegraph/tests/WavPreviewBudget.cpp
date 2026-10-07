#include <engine/imagegraph/WavPreview.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.wav_preview_budget")
TEST_DEPENDS("engine.imagegraph.wav_preview")

using namespace engine::imagegraph;

TEST_CASE("WAV preview admits retained capacity before replacing samples", "[imagegraph][wav_preview]") {
	const AudioBit clip{{1, -1, .5, -.5}, 48000};
	std::vector<float> samples;
	samples.reserve(32);
	samples.push_back(17);
	const auto *retained = samples.data();
	const size_t capacity = samples.capacity();
	const uint64_t exact = 2 * sizeof(samples) + (capacity + clip.Samples.size()) * sizeof(float);
	Diagnostic diagnostic;
	CHECK(BuildWavPreviewSamples(clip, exact - 1, samples, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(samples == std::vector<float>{17});
	CHECK(samples.data() == retained);
	CHECK(samples.capacity() == capacity);
	REQUIRE(BuildWavPreviewSamples(clip, exact, samples, diagnostic) == Status::Ok);
	CHECK(diagnostic.Code == Status::Ok);
	CHECK(samples == std::vector<float>{.5f, -.5f, .25f, -.25f});
	const uint64_t repeat = 2 * sizeof(samples) + (samples.capacity() + 4) * sizeof(float);
	CHECK(BuildWavPreviewSamples(clip, repeat - 1, samples, diagnostic) == Status::LimitExceeded);
	CHECK(samples == std::vector<float>{.5f, -.5f, .25f, -.25f});
	CHECK(BuildWavPreviewSamples(clip, repeat, samples, diagnostic) == Status::Ok);
}

TEST_CASE("cleared WAV preview storage remains charged until replacement", "[imagegraph][wav_preview]") {
	const AudioBit clip{{1}, 48000};
	std::vector<float> samples;
	samples.reserve(64);
	samples.push_back(1);
	samples.clear();
	const size_t capacity = samples.capacity();
	const auto *retained = samples.data();
	Diagnostic diagnostic;
	const uint64_t candidateOnly = 2 * sizeof(samples) + sizeof(float);
	CHECK(BuildWavPreviewSamples(clip, candidateOnly, samples, diagnostic) == Status::LimitExceeded);
	CHECK(samples.empty());
	CHECK(samples.capacity() == capacity);
	CHECK(samples.data() == retained);
	REQUIRE(
		BuildWavPreviewSamples(clip, candidateOnly + capacity * sizeof(float), samples, diagnostic) ==
		Status::Ok
	);
	CHECK(samples == std::vector<float>{.5f});
}

TEST_CASE(
	"WAV preview rejects invalid source samples without disturbing retained storage",
	"[imagegraph][wav_preview]"
) {
	std::vector<float> samples{17, 19};
	const size_t capacity = samples.capacity();
	const auto *retained = samples.data();
	Diagnostic diagnostic;
	for (double sample : {2., -2.0001, std::numeric_limits<double>::infinity()}) {
		const AudioBit clip{{sample}, 48000};
		CHECK(
			BuildWavPreviewSamples(clip, Limits::MaximumEvaluationBytes, samples, diagnostic) != Status::Ok
		);
		CHECK(samples == std::vector<float>{17, 19});
		CHECK(samples.capacity() == capacity);
		CHECK(samples.data() == retained);
	}
	const AudioBit valid{{-2}, 48000};
	REQUIRE(BuildWavPreviewSamples(valid, Limits::MaximumEvaluationBytes, samples, diagnostic) == Status::Ok);
	CHECK(samples == std::vector<float>{-1});
}
