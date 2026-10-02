#include "../src/WavTimelinePanel.hpp"

#include "../src/StudioWavTimelineDraw.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
TEST_SUITE_ID("studio.wav_timeline_panel")
TEST_DEPENDS("engine.imagegraph.wav_timeline_presentation")
using namespace engine::imagegraph;
namespace {
	struct ContextGuard {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Owned = ImGui::CreateContext();
		ContextGuard() {
			ImGui::SetCurrentContext(Owned);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~ContextGuard() {
			ImGui::SetCurrentContext(Owned);
			ImGui::DestroyContext(Owned);
			ImGui::SetCurrentContext(Previous);
		}
		ContextGuard(const ContextGuard &) = delete;
		ContextGuard &operator=(const ContextGuard &) = delete;
	};
	struct Fixture {
		Document Doc;
		EvaluationRequest Request;
		std::vector<AudioClipSource> Clips{{"clip", {{}, 8, {{0, .5, 1, .5, 0}, {0, -.5, -1, -.5, 0}}}}};
		Diagnostic Error;
		studio::WavTimelinePanel Panel;
		Fixture() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}, {"mono", true}}}
			};
			Doc.Outputs = {{"audio", "file", "data"}};
			Request.AudioClips = Clips;
		}
		bool Update(uint64_t docRevision = 1, uint64_t inputRevision = 1) {
			return Panel.Update(Doc, "file", Request, 4, docRevision, inputRevision, Error);
		}
	};
}
TEST_CASE("Visible WAV inspector retains geometry across signed cursor ticks", "[studio][wav_timeline]") {
	Fixture f;
	REQUIRE(f.Update());
	REQUIRE(f.Panel.Current());
	const auto *points = f.Panel.Current()->Points.data();
	REQUIRE(SetFrameTime(f.Request, {1, .25, false}));
	REQUIRE(f.Update());
	REQUIRE(f.Panel.Current());
	CHECK(f.Panel.Current()->Points.data() == points);
	CHECK(f.Panel.Current()->Progress == .5);
	REQUIRE(SetFrameTime(f.Request, {1, .25, true}));
	REQUIRE(f.Update());
	CHECK(f.Panel.Current()->Points.data() == points);
	CHECK(f.Panel.Current()->Progress == 0);
	f.Clips[0].Data.Channels[0][0] = .75;
	REQUIRE(f.Update(1, 2));
	CHECK(f.Panel.Current()->Points.front().Y == .75);
	const auto *replaced = f.Panel.Current()->Points.data();
	f.Doc.Nodes[0].Values[1].Data = false;
	REQUIRE(f.Update(2, 2));
	CHECK(f.Panel.Current()->Points.data() == replaced);
	CHECK(f.Panel.Current()->Channels == 2);
}
TEST_CASE("Waveform cache failures never present stale resources as current", "[studio][wav_timeline]") {
	Fixture f;
	REQUIRE(f.Update());
	f.Clips.clear();
	f.Request.AudioClips = f.Clips;
	CHECK_FALSE(f.Update(1, 2));
	CHECK(f.Panel.Current() == nullptr);
	CHECK_FALSE(f.Update(1, 2));
	CHECK(f.Panel.Current() == nullptr);
	f.Clips.push_back({"clip", {{1, 0, 1}, 8, {}}});
	f.Request.AudioClips = f.Clips;
	REQUIRE(f.Update(1, 3));
	CHECK(f.Panel.Current()->Points.front().Y == 1);
	f.Doc.Nodes[0].Values[0].Data = std::string("missing");
	CHECK_FALSE(f.Update(2, 3));
	CHECK(f.Panel.Current() == nullptr);
}
TEST_CASE("Real inspector submits waveform and cursor geometry in headless ImGui", "[studio][wav_timeline]") {
	Fixture f;
	REQUIRE(f.Update());
	ContextGuard existing;
	ImGui::GetIO().DisplaySize = {901, 601};
	{
		ContextGuard panelContext;
		CHECK(panelContext.Previous == existing.Owned);
		CHECK(ImGui::GetCurrentContext() == panelContext.Owned);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({400, 200});
		ImGui::Begin("WAV File In inspector");
		const auto before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
		f.Panel.Draw();
		CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > before);
		CHECK(ImGui::GetItemRectSize().y == 80);
		ImGui::End();
		ImGui::Render();
	}
	CHECK(ImGui::GetCurrentContext() == existing.Owned);
	CHECK(ImGui::GetIO().DisplaySize.x == 901);
	ImGui::NewFrame();
	ImGui::Begin("Restored existing context");
	ImGui::TextUnformatted("still usable");
	ImGui::End();
	ImGui::Render();
	CHECK(ImGui::GetDrawData() != nullptr);
}

TEST_CASE(
	"Waveform drawing clips finite extreme source amplitudes before float conversion",
	"[studio][wav_timeline]"
) {
	Vector2 a{0, 1e308}, b{1, -1e308};
	REQUIRE(studio::ClipWavSegment(a, b, 1));
	CHECK(a.Y == .5);
	CHECK(b.Y == -.5);
	CHECK(a.X >= 0);
	CHECK(a.X <= 1);
	CHECK(b.X >= 0);
	CHECK(b.X <= 1);
	Vector2 aboveA{0, 1e308}, aboveB{1, 1e308};
	CHECK_FALSE(studio::ClipWavSegment(aboveA, aboveB, 1));
}

TEST_CASE(
	"WAV inspector resolves linked animated source paths on cursor-only revision changes",
	"[studio][wav_timeline]"
) {
	Document authored;
	authored.FormatVersion = 9;
	authored.Nodes = {
		{"path", "pc.string_merge", "", {}, {}, {{"text_0", ValueType::Text, Value{std::string("a")}}}},
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string("unused")}}}
	};
	authored.Links = {{"path", "text", "file", "path"}};
	authored.Outputs = {{"audio", "file", "data"}};
	authored.Keyframes = {
		{"path", "text_0", 0, std::string("a"), "step"}, {"path", "text_0", 2, std::string("b"), "step"}
	};
	Document document;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(authored), document, diagnostic) == Status::Ok);
	const std::vector<AudioClipSource> clips{
		{"a", {{0, .5, 1, .5, 0}, 8, {}}}, {"b", {{-.5, -1, -.5, -1, -.5}, 8, {}}}
	};
	EvaluationRequest request;
	request.AudioClips = clips;
	studio::WavTimelinePanel panel;
	const auto update = [&](FrameTime frame) {
		REQUIRE(SetFrameTime(request, frame));
		REQUIRE(panel.Update(document, "file", request, 4, 1, 1, diagnostic));
		REQUIRE(panel.Current());
	};
	update({1, .25, false});
	CHECK(panel.Current()->Points == std::vector<Vector2>{{0, 0}, {1, 1}, {2, 0}});
	CHECK(panel.Current()->Progress == .5);
	const auto *points = panel.Current()->Points.data();
	update({1, .5625, false});
	CHECK(panel.Current()->Points.data() == points);
	CHECK(panel.Current()->Progress == .625);
	update({2, .5, false});
	CHECK(panel.Current()->Points == std::vector<Vector2>{{0, -.5}, {1, -.5}, {2, -.5}});
	CHECK(panel.Current()->Progress == 1);
	update({1, .25, false});
	CHECK(panel.Current()->Points == std::vector<Vector2>{{0, 0}, {1, 1}, {2, 0}});
	CHECK(panel.Current()->Progress == .5);
	points = panel.Current()->Points.data();
	update({1, .25, true});
	CHECK(panel.Current()->Points.data() == points);
	CHECK(panel.Current()->Points == std::vector<Vector2>{{0, 0}, {1, 1}, {2, 0}});
	CHECK(panel.Current()->Progress == 0);
}
