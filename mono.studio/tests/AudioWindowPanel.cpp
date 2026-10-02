#include "../src/AudioWindowPanel.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
TEST_SUITE_ID("studio.audio_window_panel")
TEST_DEPENDS("engine.imagegraph.audio_window_presentation")
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
		Diagnostic Error;
		studio::AudioWindowPanel Panel;
		std::vector<AudioClipSource> Clips{{"clip", {{0, .125, .25, .375, .5, .625, .75, .875}, 8, {}}}};
		Fixture() {
			Doc.FormatVersion = 9;
			Doc.Timeline = TimelineSettings{8, 0, 7, "loop", 4};
			Doc.Nodes = {
				{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}}},
				{"window",
				 "pc.audio_window",
				 "",
				 {},
				 {{"width", int64_t{3}}, {"step", int64_t{1}}, {"cursor_location", EnumValue{0}}}}
			};
			Doc.Links = {{"file", "data", "window", "audio_data"}};
			Doc.Outputs = {{"samples", "window", "bit_array"}};
			Request.AudioClips = Clips;
		}
		bool Update(uint64_t doc = 1, uint64_t input = 1) {
			return Panel.Update(Doc, "window", Request, doc, input, Error);
		}
	};
}
TEST_CASE(
	"Audio Window inspector reuses geometry for signed markers and refuses stale sources",
	"[studio][audio_window_presentation]"
) {
	Fixture f;
	REQUIRE(f.Update());
	const auto *points = f.Panel.Current()->Points.data();
	REQUIRE(SetFrameTime(f.Request, {1, .5, false}));
	REQUIRE(f.Update());
	CHECK(f.Panel.Current()->Cursor == .375);
	CHECK(f.Panel.Current()->Start == .375);
	CHECK(f.Panel.Current()->End == .75);
	CHECK(f.Panel.Current()->Points.data() == points);
	f.Clips[0].Data.Samples[0] = -.5;
	REQUIRE(f.Update(1, 2));
	CHECK(f.Panel.Current()->Points.front().Y == -.5);
	f.Request.AudioClips = {};
	CHECK_FALSE(f.Update(1, 3));
	CHECK(f.Panel.Current() == nullptr);
	CHECK_FALSE(f.Update(1, 3));
	CHECK(f.Panel.Current() == nullptr);
	f.Request.AudioClips = f.Clips;
	REQUIRE(f.Update(1, 4));
	CHECK(f.Panel.Current()->Points.front().Y == -.5);
}
TEST_CASE(
	"Real Audio Window inspector draws selected region and cursor in an owned context",
	"[studio][audio_window_presentation]"
) {
	Fixture f;
	REQUIRE(SetFrameTime(f.Request, {1, 0, false}));
	REQUIRE(f.Update());
	ContextGuard existing;
	ImGui::GetIO().DisplaySize = {901, 601};
	{
		ContextGuard owned;
		REQUIRE(owned.Previous == existing.Owned);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({400, 200});
		ImGui::Begin("Audio Window inspector");
		const auto before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
		f.Panel.Draw();
		CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size > before + 8);
		CHECK(ImGui::GetItemRectSize().y == 80);
		ImGui::End();
		ImGui::Render();
	}
	CHECK(ImGui::GetCurrentContext() == existing.Owned);
	CHECK(ImGui::GetIO().DisplaySize.x == 901);
	ImGui::NewFrame();
	ImGui::Begin("Restored context");
	ImGui::TextUnformatted("usable");
	ImGui::End();
	ImGui::Render();
	CHECK(ImGui::GetDrawData() != nullptr);
}

TEST_CASE(
	"Audio Window cache validates changed image caps and recovers the original request",
	"[studio][audio_window_presentation]"
) {
	Fixture f;
	f.Request.MaximumImageDimension = 128;
	REQUIRE(f.Update());
	const auto points = f.Panel.Current()->Points;
	f.Request.MaximumImageDimension = 0;
	CHECK_FALSE(f.Update());
	CHECK(f.Error.Code == Status::InvalidValue);
	CHECK(f.Panel.Current() == nullptr);
	f.Request.MaximumImageDimension = 128;
	REQUIRE(f.Update());
	CHECK(f.Panel.Current()->Points == points);
}
TEST_CASE(
	"Audio Window target admission refusal cannot poison the previous successful key",
	"[studio][audio_window_presentation]"
) {
	Fixture f;
	constexpr uint64_t cap = 100000;
	REQUIRE(f.Panel.Update(f.Doc, "window", f.Request, 1, 1, f.Error, cap));
	const auto points = f.Panel.Current()->Points;
	// Target identity admission precedes graph lookup. This target cannot fit alongside last good data.
	const std::string refusedTarget(cap + 1, 'b');
	CHECK_FALSE(f.Panel.Update(f.Doc, refusedTarget, f.Request, 1, 1, f.Error, cap));
	CHECK(f.Error.Code == Status::LimitExceeded);
	CHECK(f.Error.Message == "Audio Window target identity exceeds cap");
	CHECK(f.Panel.Current() == nullptr);
	REQUIRE(f.Panel.Update(f.Doc, "window", f.Request, 1, 1, f.Error, cap));
	CHECK(f.Error.Code == Status::Ok);
	CHECK(f.Panel.Current()->Points == points);
}
