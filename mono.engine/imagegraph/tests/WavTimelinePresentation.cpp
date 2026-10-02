#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/WavTimelinePresentation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.wav_timeline_presentation")
using namespace engine::imagegraph;
TEST_CASE(
	"Persisted WAV observation uses source channel zero and includes "
	"last packet",
	"[imagegraph][wav_timeline]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}, {"mono", true}}}};
	doc.Outputs = {{"audio", "file", "data"}};
	Document parsed;
	Diagnostic error;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(parsed, plan, error) == Status::Ok);
	std::vector<AudioClipSource> assets{{"clip", {{}, 8, {{0, .25, .5, .75, 1}, {0, -.25, -.5, -.75, -1}}}}};
	EvaluationRequest request;
	request.AudioClips = assets;
	request.Tick = 1;
	WavTimelinePresentation result;
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 4, 100000, result, error) == Status::Ok
	);
	CHECK(result.Points == std::vector<Vector2>{{0, 0}, {1, .5}, {2, 1}});
	CHECK(result.Channels == 2);
	CHECK(result.Duration == .625);
	CHECK(result.Progress == .4);
	const auto original = result;
	REQUIRE(SetFrameTime(request, {1, .25, true}));
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 4, 100000, result, error) == Status::Ok
	);
	CHECK(result.Progress == 0);
	CHECK(result.Points == original.Points);
	REQUIRE(SetFrameTime(request, {10, 0, false}));
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 4, 100000, result, error) == Status::Ok
	);
	CHECK(result.Progress == 1);
	const auto retained = result;
	CHECK(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 4, 1, result, error) ==
		Status::LimitExceeded
	);
	CHECK(result.Points == retained.Points);
	CHECK(result.Progress == retained.Progress);
	CHECK(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 32, 100000, result, error) ==
		Status::InvalidValue
	);
	CHECK(result.Points == retained.Points);
}
TEST_CASE(
	"Linked persisted WAV paths switch exact resources at signed "
	"fractional seeks",
	"[imagegraph][wav_timeline]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"path", "pc.string_merge", "", {}, {}, {{"text_0", ValueType::Text, Value{std::string("a")}}}},
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string("unused")}}}
	};
	doc.Links = {{"path", "text", "file", "path"}};
	doc.Outputs = {{"audio", "file", "data"}};
	doc.Keyframes = {
		{"path", "text_0", 0, std::string("a"), "step"}, {"path", "text_0", 2, std::string("b"), "step"}
	};
	Document parsed;
	Diagnostic error;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(parsed, plan, error) == Status::Ok);
	std::vector<AudioClipSource> assets{
		{"a", {{0, 1, 2, 3, 4}, 8, {}}}, {"b", {{-1, -2, -3, -4, -5}, 8, {}}}
	};
	EvaluationRequest request;
	request.AudioClips = assets;
	WavTimelinePresentation first, second, replay;
	REQUIRE(SetFrameTime(request, {1, .25, false}));
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 3.2, 100000, first, error) == Status::Ok
	);
	CHECK(first.Points == std::vector<Vector2>{{0, 0}, {1, 2}, {2, 4}});
	REQUIRE(SetFrameTime(request, {2, .5, false}));
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 3.2, 100000, second, error) ==
		Status::Ok
	);
	CHECK(second.Points == std::vector<Vector2>{{0, -1}, {1, -3}, {2, -5}});
	REQUIRE(SetFrameTime(request, {1, .25, false}));
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 3.2, 100000, replay, error) ==
		Status::Ok
	);
	CHECK(replay.Points == first.Points);
	CHECK(replay.Progress == first.Progress);
	assets[0].Data.Samples.clear();
	REQUIRE(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 3.2, 100000, replay, error) ==
		Status::Ok
	);
	CHECK(replay.Points.empty());
	CHECK(replay.Progress == 0);
	assets.erase(assets.begin());
	request.AudioClips = assets;
	CHECK(
		ResolveWavTimelinePresentation(parsed, plan, "file", request, 3.2, 100000, replay, error) ==
		Status::InvalidValue
	);
	CHECK(replay.Points.empty());
}
TEST_CASE(
	"Waveform geometry admits actual old and replacement capacities exactly", "[imagegraph][wav_timeline]"
) {
	AudioBit clip{{}, 8, {{0, .5, 1, .5, 0}, {1, 1, 1, 1, 1}}};
	WavTimelinePresentation result;
	Diagnostic error;
	REQUIRE(BuildWavTimelinePresentation(clip, {0, 0, false}, 4, 100000, result, error) == Status::Ok);
	const auto retained = result;
	const uint64_t exact = 2 * result.Points.capacity() * sizeof(Vector2);
	CHECK(
		BuildWavTimelinePresentation(clip, {1, .5, false}, 4, exact - 1, result, error) ==
		Status::LimitExceeded
	);
	CHECK(result.Points == retained.Points);
	CHECK(result.Progress == retained.Progress);
	CHECK(BuildWavTimelinePresentation(clip, {1, .5, false}, 4, exact, result, error) == Status::Ok);
	CHECK(result.Points == std::vector<Vector2>{{0, 0}, {1, 1}, {2, 0}});
}
TEST_CASE(
	"Source timeline loop retains endpoint within default comparison epsilon", "[imagegraph][wav_timeline]"
) {
	AudioBit clip;
	clip.SampleRate = 100000;
	clip.Samples.resize(99999);
	clip.Samples.front() = .25;
	clip.Samples.back() = .75;
	WavTimelinePresentation result;
	Diagnostic error;
	REQUIRE(BuildWavTimelinePresentation(clip, {0, 0, false}, 1, 100000, result, error) == Status::Ok);
	CHECK(result.Points == std::vector<Vector2>{{0, .25}, {1, .75}});
}
