// The seam a completion popup hangs off: where the caret is, and putting a
// chosen name where the half-typed one was.
//
// **This is the part that was not obviously possible.** `ScriptEditor.cpp`
// refuses `ImGuiInputTextState` - a private layout whose fields move between
// imgui releases - and that refusal is why the script editor has Replace All and
// no Find Next. A completion popup needs the same two facts that refusal denies,
// so the whole feature rests on `ImGuiInputTextCallbackData` being a different
// kind of thing: documented members, reached through a flag imgui offers for the
// purpose.
//
// It rests on that being true *of the vendored imgui*, which is what this file
// checks. If a future bump changes when `CallbackAlways` fires or what
// `InsertChars` does to the caret, the popup would stop placing itself or start
// mangling text - and both would look like a bug in the editor rather than in a
// dependency.
//
// `mono.studio/AGENTS.md` says to reach for the imgui-context harness before
// extracting a free function to avoid testing something. This is that harness,
// used for exactly that reason.

#include "ScriptFieldWindow.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <studio/Widgets.hpp>

TEST_SUITE_ID("studio.codefield")

using studio::CodeEdit;
using studio::CodeField;
using studio::FindCodeField;

namespace {
	ImGuiContext *TestContext = nullptr;

	// A bare imgui context, per case for the reason `tests/AssetRow.cpp` gives:
	// a case that fails mid-frame leaves a window on the stack, and the next
	// case would inherit it and fail for a reason that is not its own.
	class Context {
	  public:
		explicit Context(const bool scriptFont = false) {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGui::SetCurrentContext(Handle);
			TestContext = Handle;

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigErrorRecoveryEnableTooltip = false;

			if (scriptFont) {
				const auto font = engine::core::Paths::Fonts() / "JetBrainsMono.ttf";
				io.Fonts->AddFontFromFileTTF(font.string().c_str(), 13.0f);
			} else {
				io.Fonts->AddFontDefault();
			}
			io.Fonts->Build();
		}

		~Context() {
			ImGui::SetCurrentContext(Handle);
			ImGui::DestroyContext(Handle);
			TestContext = nullptr;
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	// Submits one frame containing the field, focusing it on the first so that
	// it becomes active - `CallbackAlways` only fires for an active field, which
	// is the behaviour the popup depends on and therefore the behaviour worth
	// pinning.
	void Frame(std::string &text, CodeEdit &edit, const bool focus) {
		// Other suites own their own ImGui contexts. Rebind this case's context so
		// an unfinished external frame cannot receive this case's input events.
		ImGui::SetCurrentContext(TestContext);
		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		if (ImGui::Begin(
				"code",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
			)) {
			if (focus) {
				ImGui::SetKeyboardFocusHere();
			}
			CodeField("##text", text, &edit, 400.0f, 200.0f);
		}
		ImGui::End();

		ImGui::Render();
	}

	// Runs frames until the field reports itself active, so a case does not
	// depend on how many frames imgui takes to hand over focus.
	void Activate(std::string &text, CodeEdit &edit) {
		for (int attempt = 0; attempt < 8 && !edit.Active; attempt++) {
			Frame(text, edit, attempt == 0);
		}
		REQUIRE(edit.Active);
	}

	// Types into an active field, one character per frame.
	//
	// **Typed rather than assigned, because focusing a field from code leaves
	// the caret at zero** - which these cases found rather than assumed. A case
	// that pre-filled the string and expected the caret at the end would be
	// testing a position no author ever produces.
	void Type(std::string &text, CodeEdit &edit, const std::string_view keys) {
		for (const char character : keys) {
			ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(character));
			Frame(text, edit, false);
		}
	}

}

TEST_CASE("an active code field reports its caret", "[studio][codefield]") {
	const Context context;

	std::string text;
	CodeEdit edit;

	Activate(text, edit);
	Type(text, edit, "local part");

	// The caret tracks what was typed, which is the fact the popup places
	// itself against. **Focusing from code leaves it at zero** - so a caret that
	// merely stayed in range would prove nothing, and this pins that it moves.
	CHECK(edit.Caret == static_cast<int>(text.size()));
	CHECK(text == "local part");
}

TEST_CASE("a requested completion replaces the word under the caret", "[studio][codefield]") {
	const Context context;

	// The exact shape of accepting a completion: the caret sits after a partly
	// typed name, and the popup asks for the whole name in its place.
	std::string text;
	CodeEdit edit;

	Activate(text, edit);
	Type(text, edit, "workspace.Anch");

	REQUIRE(text == "workspace.Anch");

	const int caret = edit.Caret;
	REQUIRE(caret == static_cast<int>(text.size()));

	edit.Insert = "Anchored";
	edit.ReplaceFrom = caret - 4;

	Frame(text, edit, false);

	CHECK(text == "workspace.Anchored");

	// **Cleared by the field, not by the caller.** A request left set would be
	// reapplied on the next frame and every frame after, which is an editor that
	// types one word forever.
	CHECK(edit.ReplaceFrom == -1);
	CHECK(edit.Insert.empty());

	// The caret follows the inserted text, so somebody can keep typing.
	CHECK(edit.Caret == static_cast<int>(text.size()));
}

TEST_CASE("an insertion on an empty prefix appends rather than eating a character", "[studio][codefield]") {
	const Context context;

	// The `part.` case, where the popup opened on the separator and there is no
	// prefix to replace. An off-by-one here would delete the dot.
	std::string text;
	CodeEdit edit;

	Activate(text, edit);
	Type(text, edit, "part.");

	edit.Insert = "Anchored";
	edit.ReplaceFrom = edit.Caret;

	Frame(text, edit, false);

	CHECK(text == "part.Anchored");
}

TEST_CASE("a field given no edit still grows its string", "[studio][codefield]") {
	const Context context;

	// The other call shape, and the one every other panel uses. Passing null
	// must not cost the resize behaviour that makes a `std::string` field work
	// at all.
	std::string text = "unchanged";

	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
	if (ImGui::Begin("plain", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
		CodeField("##plain", text, nullptr, 400.0f, 200.0f);
	}
	ImGui::End();
	ImGui::Render();

	CHECK(text == "unchanged");
}

TEST_CASE("a code field child is found through its hierarchical id", "[studio][codefield]") {
	const Context context;
	std::string text = "local visible = true";
	CodeEdit edit;

	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
	ImGuiID fieldId = 0;
	if (ImGui::Begin("code", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::PushID(7);
		fieldId = ImGui::GetID("##text");
		CodeField("##text", text, &edit, 400.0f, 200.0f);
		ImGui::PopID();
	}
	ImGui::End();
	ImGui::Render();

	ImGuiWindow *child = FindCodeField(fieldId);
	REQUIRE(child != nullptr);
	CHECK(child->ChildId == fieldId);
	CHECK(std::string_view(child->Name) != "##text");
}

TEST_CASE("syntax highlighting preserves glyph positions at fractional zoom", "[studio][codefield]") {
	const Context context(true);
	std::string text = "local ReplicatedStorage = game:GetService('ReplicatedStorage')\n"
					   "\trequire(ReplicatedStorage:WaitForChild('Modules'))";
	CodeEdit edit;

	for (const float zoom : {1.0f, 1.25f, 1.5f, 2.0f}) {
		CAPTURE(zoom);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(1250.0f, 600.0f));
		ImGui::Begin("syntax", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::PushFont(nullptr, 13.0f * zoom);
		const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
		const ImGuiID fieldId = ImGui::GetID("##text");
		CodeField("##text", text, &edit, 1200.0f, 400.0f);
		const ImVec2 fieldSize = ImGui::GetItemRectSize();
		ImGuiWindow *child = FindCodeField(fieldId);
		REQUIRE(child != nullptr);
		ImDrawList *draw = child->DrawList;
		const int syntaxStart = draw->VtxBuffer.Size;
		studio::DrawScriptSource(text, edit, fieldMin, fieldSize, fieldId);
		const int syntaxEnd = draw->VtxBuffer.Size;
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		const ImVec2 origin(
			fieldMin.x + padding.x - child->Scroll.x, fieldMin.y + padding.y - child->Scroll.y
		);
		draw->PushClipRect(fieldMin, ImVec2(fieldMin.x + fieldSize.x, fieldMin.y + fieldSize.y), true);
		draw->AddText(
			ImGui::GetFont(),
			ImGui::GetFontSize(),
			origin,
			IM_COL32_WHITE,
			text.data(),
			text.data() + text.size()
		);
		draw->PopClipRect();
		REQUIRE(draw->VtxBuffer.Size - syntaxEnd == syntaxEnd - syntaxStart);
		float maximumDrift = 0.0f;
		for (int vertex = 0; vertex < syntaxEnd - syntaxStart; vertex++) {
			const ImVec2 highlighted = draw->VtxBuffer[syntaxStart + vertex].pos;
			const ImVec2 plain = draw->VtxBuffer[syntaxEnd + vertex].pos;
			maximumDrift = std::max(maximumDrift, std::abs(highlighted.x - plain.x));
			maximumDrift = std::max(maximumDrift, std::abs(highlighted.y - plain.y));
		}
		// Separate text calls snap their origins to pixels. Drift must stay below one pixel.
		CHECK(maximumDrift < 1.0f);

		edit.Active = true;
		edit.Caret = static_cast<int>(text.size());
		const int caretStart = draw->VtxBuffer.Size + syntaxEnd - syntaxStart;
		studio::DrawScriptSource(text, edit, fieldMin, fieldSize, fieldId);
		const int caretEnd = draw->VtxBuffer.Size;
		const char *lastLine = text.data() + text.rfind('\n') + 1;
		const float lineWidth =
			ImGui::GetFont()
				->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, lastLine, text.data() + text.size())
				.x;
		const float caretX = origin.x + lineWidth;
		const float caretY = origin.y + ImGui::GetTextLineHeight();
		draw->AddLine(
			ImVec2(caretX, caretY), ImVec2(caretX, caretY + ImGui::GetTextLineHeight()), IM_COL32_WHITE, 1.0f
		);
		REQUIRE(draw->VtxBuffer.Size - caretEnd == caretEnd - caretStart);
		for (int vertex = 0; vertex < caretEnd - caretStart; vertex++) {
			CHECK(
				std::abs(
					draw->VtxBuffer[caretStart + vertex].pos.x - draw->VtxBuffer[caretEnd + vertex].pos.x
				) < 0.01f
			);
		}
		ImGui::PopFont();
		ImGui::End();
		ImGui::Render();
	}
}
