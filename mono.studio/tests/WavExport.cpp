#include "../src/WavExport.hpp"

#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <imgui.h>

TEST_SUITE_ID("studio.wav_export")
TEST_DEPENDS("engine.imagegraph.wav_export")
TEST_DEPENDS("engine.imagegraphexport.graph_file_host")
TEST_DEPENDS("engine.assets.contentpolicy")
namespace {
	using namespace engine::imagegraph;
	struct ExportFixture {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("atomic-wav-export-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Document Doc;
		Plan Graph;
		Diagnostic Error;
		EvaluationRequest Request;
		std::vector<AudioClipSource> Clips{{"clip", {{}, 8, {{0, .5, 1, 0}}}}};
		WavExport LastGood;
		ExportFixture() {
			std::filesystem::create_directory(Directory);
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}}},
				{"window",
				 "pc.audio_window",
				 "",
				 {},
				 {{"width", int64_t(3)},
				  {"step", int64_t(1)},
				  {"cursor_location", EnumValue{0}},
				  {"match_timeline", false}}},
				{"sink",
				 "pc.wav_file_write",
				 "",
				 {},
				 {{"path", (Directory / "output").string()}, {"remap_data", true}}}
			};
			Doc.Links = {
				{"file", "data", "window", "audio_data"}, {"window", "bit_array", "sink", "audio_data"}
			};
			Doc.Outputs = {{"observed", "window", "bit_array"}};
			Request.AudioClips = Clips;
		}
		~ExportFixture() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
		void Compile() {
			REQUIRE(engine::imagegraph::Compile(Doc, Graph, Error) == Status::Ok);
		}
		std::vector<char> ReadFile() {
			std::ifstream stream(Directory / "output.wav", std::ios::binary);
			return {std::istreambuf_iterator<char>(stream), {}};
		}
	};
}
TEST_CASE(
	"Explicit WAV host export replaces atomically and keeps last good on refusal", "[studio][wav_export]"
) {
	ExportFixture fixture;
	fixture.Compile();
	REQUIRE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	const auto original = fixture.ReadFile();
	REQUIRE(original.size() == fixture.LastGood.Bytes.size());
	CHECK(original.front() == 'R');
	fixture.Clips.front().Data.Channels.front() = {1, .25, 0, 1};
	fixture.Doc.Nodes.back().Values.push_back({"bit_depth", EnumValue{1}});
	fixture.Doc.Nodes.back().Values.push_back({"sample", int64_t{8}});
	fixture.Compile();
	REQUIRE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	const auto replacement = fixture.ReadFile();
	REQUIRE(replacement.size() == 50);
	CHECK(replacement != original);
	CHECK(static_cast<unsigned char>(replacement[24]) == 8);
	CHECK(static_cast<unsigned char>(replacement[34]) == 16);
	CHECK(static_cast<unsigned char>(replacement[40]) == 6);
	CHECK(static_cast<unsigned char>(replacement[44]) == 255);
	CHECK(static_cast<unsigned char>(replacement[45]) == 127);
	CHECK(static_cast<unsigned char>(replacement[46]) == 0);
	CHECK(static_cast<unsigned char>(replacement[47]) == 192);
	CHECK(static_cast<unsigned char>(replacement[48]) == 0);
	CHECK(static_cast<unsigned char>(replacement[49]) == 128);
	const auto artifact = fixture.LastGood;
	fixture.Doc.Nodes.back().Values.front().Data = (fixture.Directory / "missing" / "output").string();
	fixture.Compile();
	CHECK_FALSE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	CHECK(fixture.LastGood.Bytes == artifact.Bytes);
	CHECK(fixture.LastGood.Path == artifact.Path);
	CHECK(fixture.ReadFile() == replacement);
	fixture.Doc.Nodes.back().Values.front().Data = (fixture.Directory / "output").string();
	fixture.Doc.Nodes.back().Values.push_back({"data_range", Vector2{1, 1}});
	fixture.Compile();
	CHECK_FALSE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	CHECK(fixture.Error.Port == "data_range");
	CHECK(fixture.LastGood.Bytes == artifact.Bytes);
	CHECK(fixture.LastGood.Path == artifact.Path);
	CHECK(fixture.ReadFile() == replacement);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(fixture.Directory), std::filesystem::directory_iterator{}
		) == 1
	);
}
TEST_CASE("Inspector WAV button writes only on explicit ImGui click", "[studio][wav_export]") {
	ExportFixture fixture;
	fixture.Compile();
	auto *context = ImGui::CreateContext();
	auto &io = ImGui::GetIO();
	io.DisplaySize = {800, 600};
	io.DeltaTime = 1.f / 60;
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.Fonts->AddFontDefault();
	io.Fonts->Build();
	std::string message;
	ImVec2 button;
	const auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({500, 300});
		ImGui::Begin("WAV inspector");
		button = ImGui::GetCursorScreenPos();
		studio::DrawImageGraphWavExport(fixture.Doc, "sink", fixture.Request, fixture.LastGood, message);
		ImGui::End();
		ImGui::Render();
	};
	frame();
	frame();
	CHECK_FALSE(std::filesystem::exists(fixture.Directory / "output.wav"));
	io.AddMousePosEvent(button.x + 20, button.y + 8);
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMouseButtonEvent(0, false);
	frame();
	CHECK(std::filesystem::exists(fixture.Directory / "output.wav"));
	CHECK(message.starts_with("Exported "));
	CHECK_FALSE(fixture.LastGood.Bytes.empty());
	const auto artifact = fixture.LastGood;
	const auto previousFile = fixture.ReadFile();
	fixture.Doc.Nodes.back().Values.front().Data = (fixture.Directory / "missing" / "output").string();
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMouseButtonEvent(0, false);
	frame();
	CHECK_FALSE(message.empty());
	CHECK_FALSE(message.starts_with("Exported "));
	CHECK(fixture.LastGood.Path == artifact.Path);
	CHECK(fixture.LastGood.Bytes == artifact.Bytes);
	CHECK(fixture.ReadFile() == previousFile);
	CHECK_FALSE(std::filesystem::exists(fixture.Directory / "missing"));
	ImGui::DestroyContext(context);
}

TEST_CASE(
	"Studio WAV publication respects the process content policy", "[studio][wav_export][content_policy]"
) {
	using namespace engine::assets;
	using namespace engine::core;
	Flags::Reset();
	struct ResetPolicy {
		~ResetPolicy() {
			Flags::Reset();
		}
	} reset;
	REQUIRE(DeclareContentFlags(ContentVerb::Publish));
	ExportFixture fixture;
	fixture.Compile();
	REQUIRE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	const auto artifact = fixture.LastGood;
	const auto previousFile = fixture.ReadFile();
	REQUIRE(Flags::Set("cdn.publish.wav", "false", FlagSource::ConfigFile) == FlagStatus::Applied);
	Flags::Freeze();
	REQUIRE_FALSE(ContentPolicy::Process(ContentVerb::Publish).AllowsName(artifact.Path));
	fixture.Clips.front().Data.Channels.front() = {1, .25, 0, 1};
	CHECK_FALSE(
		studio::ExportImageGraphWav(
			fixture.Doc, fixture.Graph, "sink", fixture.Request, fixture.LastGood, fixture.Error
		)
	);
	CHECK(fixture.Error.Port == "path");
	CHECK(fixture.Error.Message.find("content policy") != std::string::npos);
	CHECK(fixture.LastGood.Path == artifact.Path);
	CHECK(fixture.LastGood.Bytes == artifact.Bytes);
	CHECK(fixture.ReadFile() == previousFile);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(fixture.Directory), std::filesystem::directory_iterator{}
		) == 1
	);
}
