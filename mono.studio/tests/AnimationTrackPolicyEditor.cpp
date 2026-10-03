#include "AnimationTrackPolicyEditor.hpp"

#include "ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui_internal.h>

TEST_SUITE_ID("studio.animation_track_policy_editor")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	struct Timeline {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;

		Diagnostic Error;
		unsigned Changes = 0;
		bool Refuse = false;
		Timeline() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 9;
			Doc.Outputs = {{"result", "node", "return_value"}};
			Doc.Nodes = {{"node", "pc.lua_compute", {}, {}, {}}};
			REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(Doc, "node", 1, Error));
			std::erase(Doc.Nodes[0].SourceStaticInputs, "argument_value_0");
			Doc.Nodes[0].SourceAnimatedInputs = {"argument_value_0"};
			Doc.Keyframes = {
				{"node", "argument_value_0", 1, 1.0, "source"}, {"node", "argument_value_0", 3, 3.0, "source"}
			};
			for (auto &key : Doc.Keyframes)
				key.Ease = KeyframeEase{};
			Doc.Tracks = {{"node", "argument_value_0", "loop", -1}};
		}
		~Timeline() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({700, 500});
			ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			studio::detail::DrawAnimationTrackPolicy(
				Doc, Doc.Nodes[0], "argument_value_0", Error, [&](const auto &edit) {
					const bool accepted =
						studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
							edit(doc);
							return !Refuse && Error.Code == Status::Ok;
						});
					if (accepted) ++Changes;
					return accepted;
				}
			);
			ImGui::End();
			ImGui::Render();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		bool Focus(const char *label) {
			for (unsigned step = 0; step < 40; ++step) {
				auto *window = Context->OpenPopupStack.empty() ? ImGui::FindWindowByName("Timeline")
															   : Context->OpenPopupStack.back().Window;
				const auto itemId = window && std::string_view(label) == "Loop tail (-1 = all)"
										? ImHashStr(label, 0, window->GetID("Animation (2 keys)"))
										: (window ? window->GetID(label) : ImGuiID{});
				if (window && Context->NavWindow == window && Context->NavId == itemId) return true;
				Key(ImGuiKey_Tab);
			}
			return false;
		}
		void OpenPolicy() {
			REQUIRE(Focus("Animation (2 keys)"));
			Key(ImGuiKey_RightArrow);
			Frame();
			auto *window = ImGui::FindWindowByName("Timeline");
			REQUIRE(window);
			REQUIRE(window->StateStorage.GetInt(window->GetID("Animation (2 keys)")) != 0);
		}
		void Whole(const char *value) {
			REQUIRE(Focus("Loop tail (-1 = all)"));
			Key(ImGuiKey_Enter);
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			Key(ImGuiKey_A);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
			io.AddInputCharactersUTF8(value);
			Frame();
			Key(ImGuiKey_Enter);
		}
	};
}
TEST_CASE(
	"Dynamic source socket track policy is authored through the inspector widget",
	"[studio][dynamic_track_policy]"
) {
	Timeline ui;
	const auto before = ui.Doc;
	REQUIRE(ui.Doc.Nodes[0].Values.empty());
	Plan plan;
	const auto status = Compile(ui.Doc, plan, ui.Error);
	INFO(ui.Error.NodeId << "/" << ui.Error.Port << ": " << ui.Error.Message);
	REQUIRE(status == Status::Ok);
	ui.Frame();
	ui.Frame();
	ui.OpenPolicy();
	ui.Whole("0");
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Tracks.size() == 1);
	CHECK(ui.Doc.Tracks[0].LoopRange == 0);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	CHECK(ui.Doc.Nodes == before.Nodes);
	CHECK(Compile(ui.Doc, plan, ui.Error) == Status::Ok);
	const auto after = ui.Doc;
	Document loaded;
	REQUIRE(Read(Write(after), loaded, ui.Error) == Status::Ok);
	CHECK(loaded == after);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}
TEST_CASE(
	"Dynamic track policy refuses unknown ports and host rejection atomically",
	"[studio][dynamic_track_policy]"
) {
	Timeline ui;
	const auto before = ui.Doc;
	CHECK_FALSE(studio::SetImageGraphAnimationTrack(ui.Doc, "node", "missing", "loop", 0, ui.Error));
	CHECK(ui.Error.Code == Status::UnknownPort);
	CHECK(ui.Doc == before);
	ui.Error = {};
	ui.Frame();
	ui.Frame();
	ui.OpenPolicy();
	ui.Refuse = true;
	ui.Whole("0");
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
}
