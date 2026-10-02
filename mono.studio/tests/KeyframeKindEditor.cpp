#include "../src/KeyframeKindEditor.hpp"

#include "../src/ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

TEST_SUITE_ID("studio.keyframe_kind_editor")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;

	struct ScopedCurrentContext {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();

		ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
		}
		~ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		ScopedCurrentContext(const ScopedCurrentContext &) = delete;
		ScopedCurrentContext &operator=(const ScopedCurrentContext &) = delete;
	};

	struct Popup {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document DocumentValue;
		studio::ImageGraphHistory History;
		studio::KeyframeKindEditor Editor;
		Diagnostic Error;
		ImVec2 ButtonCenter;
		unsigned ApplyCalls = 0;
		unsigned Changes = 0;

		Popup() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {640, 480};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			DocumentValue.FormatVersion = 8;
			DocumentValue.Nodes = {{"point", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 0.0}}}};
			DocumentValue.Keyframes = {{"point", "x", 0, 1.0, "linear"}};
		}
		~Popup() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		Popup(const Popup &) = delete;
		Popup &operator=(const Popup &) = delete;

		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({400, 260});
			ImGui::Begin("Kind editor", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			ImGui::PushID(0);
			Editor.Draw(DocumentValue, 0, [&] {
				++ApplyCalls;
				bool accepted = false;
				if (studio::ApplyImageGraphDocumentEdit(DocumentValue, History, [&](Document &document) {
						accepted = Editor.Commit(document, Error);
					}))
					++Changes;
				return accepted;
			});
			if (!ImGui::IsPopupOpen("##key-kind")) {
				const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
				ButtonCenter = {(minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f};
			}
			ImGui::PopID();
			ImGui::End();
			ImGui::Render();
		}
		void Open() {
			Frame();
			Frame();
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(ButtonCenter.x, ButtonCenter.y);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame();
			Frame();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		bool IsOpen() const {
			return !Context->OpenPopupStack.empty();
		}
		// Tabs through real submitted controls, rather than setting NavId or invoking their actions.
		bool Focus(const char *label) {
			for (unsigned step = 0; step < 12; ++step) {
				if (!IsOpen()) return false;
				auto *window = Context->OpenPopupStack.back().Window;
				if (window && Context->NavWindow == window && Context->NavId == window->GetID(label))
					return true;
				Key(ImGuiKey_Tab);
			}
			return false;
		}
	};
}

TEST_CASE(
	"key-kind popup selects and restores its context when another is current",
	"[studio][key_kind_popup]"
) {
	ScopedCurrentContext existing;
	{
		Popup popup;
		CHECK(ImGui::GetCurrentContext() == popup.Context);
		CHECK(popup.Context->IO.DisplaySize.x == 640);
		CHECK(popup.Context->IO.DisplaySize.y == 480);
		popup.Frame();
	}
	CHECK(ImGui::GetCurrentContext() == existing.Context);
}

TEST_CASE(
	"key-kind popup Apply dispatch creates exactly one undoable persisted edit", "[studio][key_kind_popup]"
) {
	Popup popup;
	const auto before = popup.DocumentValue;
	popup.Open();
	REQUIRE(popup.IsOpen());
	REQUIRE(popup.Focus("Adder"));
	popup.Key(ImGuiKey_Space);
	CHECK(popup.Editor.Draft == KeyframeKind::Adder);
	CHECK(popup.DocumentValue == before);
	CHECK_FALSE(popup.History.CanUndo());
	REQUIRE(popup.Focus("Apply"));
	popup.Key(ImGuiKey_Space);
	CHECK_FALSE(popup.IsOpen());
	CHECK(popup.ApplyCalls == 1);
	CHECK(popup.Changes == 1);
	CHECK(popup.DocumentValue.Keyframes[0].Kind == KeyframeKind::Adder);
	CHECK(popup.DocumentValue.FormatVersion == 9);
	const auto after = popup.DocumentValue;
	REQUIRE(popup.History.Undo(popup.DocumentValue));
	CHECK(popup.DocumentValue == before);
	CHECK_FALSE(popup.History.CanUndo());
	REQUIRE(popup.History.Redo(popup.DocumentValue));
	CHECK(popup.DocumentValue == after);
	CHECK_FALSE(popup.History.CanRedo());
	Document loaded;
	REQUIRE(Read(Write(popup.DocumentValue), loaded, popup.Error) == Status::Ok);
	CHECK(loaded == after);
}

TEST_CASE(
	"key-kind popup Cancel and Escape discard the staged marker without a transaction",
	"[studio][key_kind_popup]"
) {
	Popup popup;
	const auto before = popup.DocumentValue;
	for (const bool escape : {false, true}) {
		popup.Open();
		REQUIRE(popup.IsOpen());
		REQUIRE(popup.Focus("Adder"));
		popup.Key(ImGuiKey_Space);
		REQUIRE(popup.Editor.Draft == KeyframeKind::Adder);
		if (escape)
			popup.Key(ImGuiKey_Escape);
		else {
			REQUIRE(popup.Focus("Cancel"));
			popup.Key(ImGuiKey_Space);
		}
		CHECK_FALSE(popup.IsOpen());
		CHECK_FALSE(popup.Editor.Active);
		CHECK(popup.DocumentValue == before);
		CHECK_FALSE(popup.History.CanUndo());
		CHECK(popup.ApplyCalls == 0);
		CHECK(popup.Changes == 0);
	}
}

TEST_CASE(
	"key-kind popup Apply refuses a replaced signed-time target atomically", "[studio][key_kind_popup]"
) {
	Popup popup;
	popup.Open();
	REQUIRE(popup.IsOpen());
	REQUIRE(popup.Focus("Adder"));
	popup.Key(ImGuiKey_Space);
	REQUIRE(SetFrameTime(popup.DocumentValue.Keyframes[0], {1, .25, true}));
	const auto replaced = popup.DocumentValue;
	REQUIRE(popup.Focus("Apply"));
	popup.Key(ImGuiKey_Space);
	CHECK(popup.IsOpen());
	CHECK(popup.ApplyCalls == 1);
	CHECK(popup.Changes == 0);
	CHECK(popup.Error.Code == Status::InvalidValue);
	CHECK(popup.DocumentValue == replaced);
	CHECK_FALSE(popup.History.CanUndo());
	REQUIRE(popup.Focus("Cancel"));
	popup.Key(ImGuiKey_Space);
	CHECK_FALSE(popup.IsOpen());
}
