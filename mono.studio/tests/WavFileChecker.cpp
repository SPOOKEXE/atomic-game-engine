#include "../src/WavFileChecker.hpp"

#include "../src/WavFileWatch.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <studio/WavPreview.hpp>

TEST_SUITE_ID("studio.wav_file_checker")
TEST_DEPENDS("studio.wav_preview")

using namespace engine::imagegraph;

TEST_CASE("WAV file checker uses strict newer edits and two host frames", "[studio][wav_checker]") {
	studio::detail::WavFileCheckerState state;
	state.EditSecond = 100;
	Diagnostic diagnostic;
	size_t reloads = 99;
	const auto poll = [&](uint64_t frame, bool enabled, std::optional<int64_t> stamp) {
		return studio::detail::AdvanceWavFileChecker(state, frame, enabled, stamp, reloads, diagnostic);
	};
	REQUIRE(poll(0, true, 101) == Status::Ok);
	CHECK(reloads == 0);
	CHECK(state.Pending == 1);
	REQUIRE(poll(1, true, 102) == Status::Ok);
	CHECK(reloads == 0);
	CHECK(state.Pending == 2);
	REQUIRE(poll(2, false, 999) == Status::Ok);
	CHECK(reloads == 1);
	CHECK(state.EditSecond == 102);
	REQUIRE(poll(2, true, 103) == Status::Ok);
	CHECK(reloads == 0);
	REQUIRE(poll(3, false, std::nullopt) == Status::Ok);
	CHECK(reloads == 1);
	REQUIRE(poll(4, true, 102) == Status::Ok);
	CHECK(reloads == 0);
	REQUIRE(poll(5, true, 99) == Status::Ok);
	CHECK(reloads == 0);
	REQUIRE(poll(6, true, std::nullopt) == Status::Ok);
	CHECK(reloads == 0);
	const auto prior = state;
	reloads = 17;
	CHECK(poll(1, true, 103) == Status::InvalidValue);
	CHECK(state == prior);
	CHECK(reloads == 17);
	CHECK(poll(std::numeric_limits<uint64_t>::max(), true, 103) == Status::LimitExceeded);
	CHECK(state == prior);
	CHECK(reloads == 17);
	state.Pending = state.DueFrames.size();
	state.DueFrames.fill(100);
	state.LastHostFrame = 6;
	const auto full = state;
	CHECK(poll(7, true, 103) == Status::LimitExceeded);
	CHECK(state == full);
	CHECK(reloads == 17);
}

namespace {
	struct WaveFile {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("studio-wav-checker-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
		std::filesystem::file_time_type Initial;
		WaveFile() {
			Write(192);
			Initial = std::filesystem::last_write_time(Path);
		}
		void Write(uint8_t sample) {
			std::vector<char> bytes;
			const auto tag = [&](const char *value) { bytes.insert(bytes.end(), value, value + 4); };
			const auto word = [&](uint32_t value, size_t size) {
				for (size_t i = 0; i < size; ++i)
					bytes.push_back(static_cast<char>(value >> (8 * i)));
			};
			tag("RIFF");
			word(100, 4);
			tag("WAVE");
			tag("fmt ");
			word(16, 4);
			word(1, 2);
			word(1, 2);
			word(8, 4);
			word(8, 4);
			word(1, 2);
			word(8, 2);
			tag("data");
			word(64, 4);
			bytes.insert(bytes.end(), 64, static_cast<char>(sample));
			std::ofstream output(Path, std::ios::binary | std::ios::trunc);
			output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		}
		void Stamp(int second) {
			std::filesystem::last_write_time(Path, Initial + std::chrono::seconds(second));
		}
		~WaveFile() {
			std::error_code error;
			std::filesystem::remove(Path, error);
		}
	};
}

TEST_CASE("Granted WAV reload invalidates previews without opening a device", "[studio][wav_checker]") {
	WaveFile file;
	size_t opened = 0;
	studio::ImageGraphWavPreview preview(
		[&](const engine::audio::DeviceSettings &) -> std::unique_ptr<engine::audio::Device> {
			++opened;
			return nullptr;
		}
	);
	std::vector<AudioClipSource> sources;
	studio::ImageGraphPreviewCache cache;
	Diagnostic diagnostic;
	REQUIRE(preview.LoadSource(sources, cache, "clip", file.Path, diagnostic));
	REQUIRE(sources.size() == 1);
	const auto first = sources.front();
	Image image;
	image.Width = image.Height = 1;
	image.Pixels.resize(4);
	REQUIRE(cache.Store(1, 0, 0, image));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"wav", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}}}};
	document.Outputs = {{"audio", "wav", "data"}};
	EvaluationRequest request;
	size_t reloaded = 0;
	file.Write(64);
	file.Stamp(10);
	REQUIRE(preview.CheckFiles(document, request, 0, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	CHECK(sources.front().SourceId == first.SourceId);
	CHECK(sources.front().Data == first.Data);
	CHECK(cache.Find(1, 0, 0));
	request.Tick = 999;
	REQUIRE(preview.CheckFiles(document, request, 1, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	REQUIRE(preview.CheckFiles(document, request, 2, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 1);
	CHECK(sources.front().Data != first.Data);
	CHECK_FALSE(cache.Find(1, 0, 0));
	CHECK(opened == 0);
	const auto retained = sources.front();
	REQUIRE(preview.CheckFiles(document, request, 2, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	CHECK(sources.front().SourceId == retained.SourceId);
	CHECK(sources.front().Data == retained.Data);
	document.Nodes.front().SourceProperties = {{"file_checker", false}};
	file.Write(192);
	file.Stamp(20);
	REQUIRE(preview.CheckFiles(document, request, 3, sources, cache, reloaded, diagnostic));
	REQUIRE(preview.CheckFiles(document, request, 5, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	CHECK(sources.front().SourceId == retained.SourceId);
	CHECK(sources.front().Data == retained.Data);
	document.Nodes.front().SourceProperties = {{"file_checker", true}};
	REQUIRE(preview.CheckFiles(document, request, 6, sources, cache, reloaded, diagnostic));
	REQUIRE(preview.CheckFiles(document, request, 8, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 1);
	CHECK(sources.front().Data == first.Data);
}

TEST_CASE("Failed watched decode preserves granted source and cached pixels", "[studio][wav_checker]") {
	WaveFile file;
	studio::ImageGraphWavPreview preview;
	std::vector<AudioClipSource> sources;
	studio::ImageGraphPreviewCache cache;
	Diagnostic diagnostic;
	REQUIRE(preview.LoadSource(sources, cache, "clip", file.Path, diagnostic));
	const auto retained = sources;
	Image image;
	image.Width = image.Height = 1;
	image.Pixels.resize(4);
	REQUIRE(cache.Store(1, 0, 0, image));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"wav", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}}}};
	document.Outputs = {{"audio", "wav", "data"}};
	{
		std::ofstream broken(file.Path, std::ios::binary | std::ios::trunc);
		broken << "bad";
	}
	file.Stamp(10);
	EvaluationRequest request;
	size_t reloaded = 0;
	REQUIRE(preview.CheckFiles(document, request, 0, sources, cache, reloaded, diagnostic));
	CHECK_FALSE(preview.CheckFiles(document, request, 2, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	REQUIRE(sources.size() == retained.size());
	CHECK(sources.front().SourceId == retained.front().SourceId);
	CHECK(sources.front().Data == retained.front().Data);
	CHECK(cache.Find(1, 0, 0));
	document.Nodes.front().Values.front().Data = std::string("ungranted-other.wav");
	CHECK_FALSE(preview.CheckFiles(document, request, 3, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	CHECK(sources.front().Data == retained.front().Data);
	CHECK(cache.Find(1, 0, 0));
	document.Nodes.front().Values.front().Data = std::string("clip");
	REQUIRE(preview.CheckFiles(document, request, 3, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 0);
	file.Write(64);
	file.Stamp(20);
	REQUIRE(preview.CheckFiles(document, request, 4, sources, cache, reloaded, diagnostic));
	REQUIRE(preview.CheckFiles(document, request, 6, sources, cache, reloaded, diagnostic));
	CHECK(reloaded == 1);
	CHECK_FALSE(cache.Find(1, 0, 0));
	CHECK(sources.front().Data != retained.front().Data);
}

TEST_CASE("WAV watcher binding cannot grow or replace authority on refusal", "[studio][wav_checker]") {
	WaveFile file;
	studio::detail::WavFileWatches watches;
	studio::detail::PreparedWavFileWatch prepared;
	Diagnostic diagnostic;
	REQUIRE(watches.PrepareBinding("clip", file.Path, Limits::MaximumEvaluationBytes, prepared, diagnostic));
	watches.Publish(std::move(prepared));
	const auto retained = watches.RetainedBytes();
	prepared.Record.SourceId = "retained";
	CHECK_FALSE(watches.PrepareBinding("other", file.Path, 1, prepared, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(prepared.Record.SourceId == "retained");
	const uint64_t pathWorkspace = sizeof(studio::detail::PreparedWavFileWatch) +
								   std::max(size_t{5}, std::string{}.capacity()) +
								   file.Path.native().size() * 32 + 128;
	CHECK_FALSE(watches.PrepareBinding("other", file.Path, pathWorkspace - 1, prepared, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(prepared.Record.SourceId == "retained");
	REQUIRE(watches.PrepareBinding("other", file.Path, pathWorkspace, prepared, diagnostic));
	CHECK(prepared.Record.GrantedPath == file.Path.string());

	CHECK(watches.RetainedBytes() == retained);
	for (size_t index = 1; index < Limits::MaximumNodes; ++index) {
		REQUIRE(watches.PrepareBinding(
			"clip" + std::to_string(index), file.Path, Limits::MaximumEvaluationBytes, prepared, diagnostic
		));
		watches.Publish(std::move(prepared));
	}
	CHECK_FALSE(
		watches.PrepareBinding("overflow", file.Path, Limits::MaximumEvaluationBytes, prepared, diagnostic)
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	REQUIRE(watches.PrepareBinding("clip", file.Path, Limits::MaximumEvaluationBytes, prepared, diagnostic));
	watches.Publish(std::move(prepared));
	CHECK(watches.Files.front()->SourceId == "clip");
	watches.Remove("clip");
	CHECK_FALSE(watches.Files.front());
}
